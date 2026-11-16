// SPDX-License-Identifier: GPL-2.0
/*
 * kmem_demo.c — every kernel allocator once, self-tested and freed:
 *               kmalloc size classes, kmem_cache, vmalloc, kvmalloc,
 *               the page allocator, mempool, per-CPU memory, GFP_ATOMIC
 *
 * Concept : The kernel has no single malloc(). kmalloc() hands out small,
 *           physically contiguous objects from power-of-two-ish slab size
 *           classes; kmem_cache_create() makes a private slab for one
 *           object type; vmalloc() gives big buffers that are contiguous
 *           only in VIRTUAL addresses; alloc_pages() hands out 2^order
 *           physically contiguous pages from the buddy allocator; mempool
 *           keeps a reserve so an allocation can never fail; alloc_percpu()
 *           gives every CPU its own copy. Each test allocates, checks the
 *           property that allocator promises, frees, and logs PASS/FAIL.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/02-kernel-memory-and-data-structures/kernel)
 * Run     : sudo insmod kmem_demo.ko [nobj=1000] [vmalloc_mb=4] [order=3] [show_layout=1]
 *           dmesg | grep kmem_demo      -> one PASS line per test
 *           sudo rmmod kmem_demo
 *           (on the board or in a VM — never on a machine you care about)
 * Read    : ../README.md  (section 3 "How it works", 3.1 to 3.9)
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/cpu.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mempool.h>
#include <linux/minmax.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>

static int nobj = 1000;
module_param(nobj, int, 0444);
MODULE_PARM_DESC(nobj, "objects allocated from the private kmem_cache (1..100000, default 1000)");

static int vmalloc_mb = 4;
module_param(vmalloc_mb, int, 0444);
MODULE_PARM_DESC(vmalloc_mb, "size of the vmalloc test buffer in MiB (1..64, default 4)");

static int order = 3;
module_param(order, int, 0444);
MODULE_PARM_DESC(order, "alloc_pages() order: 2^order contiguous pages (0..MAX_PAGE_ORDER, default 3)");

static bool show_layout;
module_param(show_layout, bool, 0444);
MODULE_PARM_DESC(show_layout, "print the kernel virtual memory layout (raw addresses: defeats KASLR, dev boards only)");

static int nr_fail;

/* One "PASS name: details" or "FAIL name: details" line per check. */
static __printf(3, 4) void check(bool ok, const char *test, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %-12s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-12s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ------------------------------------------------------------------------ */
/* Test 0: where does each kind of memory live?                             */
/* ------------------------------------------------------------------------ */
/*
 * arm64, 48-bit VA, 4 KiB pages (the board):
 *
 *   0xffff_ffff_ffff_ffff +--------------------------+
 *                         | fixmap, PCI I/O, vmemmap  |  struct page array
 *                         +--------------------------+
 *                         | vmalloc / ioremap area    |  vmalloc(), ioremap()
 *                         | kernel image (.text...)   |  _text .. _end
 *                         | modules area (2 GiB)      |  THIS module's code
 *                         +--------------------------+  _PAGE_END / VMALLOC
 *                         | linear map ("lowmem")     |  ALL of RAM mapped 1:1
 *                         |  virt = phys + offset     |  kmalloc, alloc_pages
 *   PAGE_OFFSET           +--------------------------+
 *   0xffff_0000_0000_0000   (user space is below, in the other TTBR)
 *
 * virt_to_phys()/virt_to_page() only work for addresses in the linear map
 * (virt_addr_valid() is true). A vmalloc address must go through
 * vmalloc_to_page() — page by page, since the pages are scattered.
 */
static int module_static_var = 42;	/* lives in this module's .data */

static void test_layout(void)
{
	void *k = kmalloc(64, GFP_KERNEL);
	void *v = vmalloc(PAGE_SIZE);
	bool ok;

	if (!k || !v) {
		check(false, "layout", "allocation failed");
		goto out;
	}
	ok = virt_addr_valid(k) && !is_vmalloc_addr(k) &&
	     !virt_addr_valid(v) && is_vmalloc_addr(v) &&
	     is_vmalloc_or_module_addr(&module_static_var) &&
	     !virt_addr_valid(&module_static_var);
	check(ok, "layout",
	      "kmalloc -> linear map, vmalloc -> vmalloc area, module .data -> modules area (%d)",
	      module_static_var);

	if (show_layout) {
		/* %lx on purpose: %p would print hashed values, useless here. */
		pr_info("INFO PAGE_OFFSET   %016lx  (linear map start)\n",
			(unsigned long)PAGE_OFFSET);
		pr_info("INFO high_memory   %016lx  (linear map end of RAM)\n",
			(unsigned long)high_memory);
		pr_info("INFO VMALLOC_START %016lx\n", (unsigned long)VMALLOC_START);
		pr_info("INFO VMALLOC_END   %016lx\n", (unsigned long)VMALLOC_END);
		pr_info("INFO kmalloc obj   %016lx  phys %016llx\n",
			(unsigned long)k, (unsigned long long)virt_to_phys(k));
		pr_info("INFO vmalloc obj   %016lx\n", (unsigned long)v);
		pr_info("INFO module .data  %016lx\n",
			(unsigned long)&module_static_var);
	}
out:
	vfree(v);	/* vfree(NULL) and kfree(NULL) are no-ops */
	kfree(k);
}

/* ------------------------------------------------------------------------ */
/* Test 1: kmalloc size classes, kzalloc, kcalloc overflow, krealloc        */
/* ------------------------------------------------------------------------ */
/*
 * kmalloc(n) rounds n up to the next slab size class:
 *
 *   8 16 32 64 96 128 192 256 512 1k 2k 4k 8k | > 8 KiB (2 pages): straight
 *   (kmalloc-8 ... kmalloc-8k caches)         | from the page allocator
 *
 * (arm64 with ARCH_DMA_MINALIGN=128 used to start at kmalloc-128; since 6.5
 * small classes exist there too unless the device is not DMA-coherent.)
 * ksize() reports what you really got; kmalloc_size_roundup() predicts it.
 * Never USE the slack — KASAN treats bytes past the requested size as out
 * of bounds. Ask for the rounded size up front instead.
 */
static void test_kmalloc(void)
{
	static const size_t sizes[] = { 1, 8, 13, 24, 33, 65, 100, 129, 200,
					300, 500, 1000, 1025, 2049, 4097, 8193,
					20000 };
	size_t i, waste = 0, asked = 0, huge;
	bool ok = true;
	u8 *z;
	u32 *arr, *grown;

	for (i = 0; i < ARRAY_SIZE(sizes); i++) {
		void *p = kmalloc(sizes[i], GFP_KERNEL);
		size_t got;

		if (!p) {
			ok = false;
			break;
		}
		got = ksize(p);
		/* KFENCE may serve a sample object with ksize == exact size. */
		if (got < sizes[i])
			ok = false;
		pr_info("INFO kmalloc(%5zu) -> ksize %5zu  (roundup %5zu)\n",
			sizes[i], got, kmalloc_size_roundup(sizes[i]));
		waste += got - sizes[i];
		asked += sizes[i];
		kfree(p);
	}
	check(ok, "kmalloc", "%zu sizes, asked %zu bytes, slack %zu bytes (internal fragmentation)",
	      ARRAY_SIZE(sizes), asked, waste);

	/* kmalloc(0) is legal and returns ZERO_SIZE_PTR (not NULL!). */
	z = kmalloc(0, GFP_KERNEL);
	check(ZERO_OR_NULL_PTR(z) && z, "kmalloc(0)", "returns ZERO_SIZE_PTR, kfree() accepts it");
	kfree(z);

	/* kzalloc = kmalloc + __GFP_ZERO. */
	z = kzalloc(256, GFP_KERNEL);
	if (z) {
		check(!memchr_inv(z, 0, 256), "kzalloc", "256 bytes all zero");
		kfree(z);
	}

	/*
	 * kcalloc(n, size) checks n * size for overflow. A hand-written
	 * kmalloc(n * size) would wrap to a small number and hand you a tiny
	 * buffer you then overflow. (No warning is printed: it just fails.)
	 */
	huge = SIZE_MAX / 2;
	OPTIMIZER_HIDE_VAR(huge);	/* runtime value: no compile-time size warning */
	arr = kcalloc(huge, sizeof(u32), GFP_KERNEL);
	check(!arr, "kcalloc", "n*size overflow detected -> NULL instead of a short buffer");
	kfree(arr);

	/* krealloc keeps the old contents; on failure the OLD buffer survives. */
	arr = kmalloc_array(16, sizeof(u32), GFP_KERNEL);
	if (!arr)
		return;
	for (i = 0; i < 16; i++)
		arr[i] = i * i;
	grown = krealloc(arr, 4096 * sizeof(u32), GFP_KERNEL);
	if (!grown) {
		kfree(arr);	/* never "arr = krealloc(arr...)": leaks on failure */
		check(false, "krealloc", "allocation failed");
		return;
	}
	ok = true;
	for (i = 0; i < 16; i++)
		ok &= grown[i] == i * i;
	check(ok, "krealloc", "64 B -> 16 KiB, first 16 words preserved, ksize now %zu",
	      ksize(grown));
	kfree(grown);
}

/* ------------------------------------------------------------------------ */
/* Test 2: a private slab cache with a constructor                          */
/* ------------------------------------------------------------------------ */
/*
 *   kmem_cache "esh_obj"                one slab = 1+ pages cut into objects
 *   +-------------------------------+   +-----+-----+-----+-----+-----+
 *   | object size, align, ctor      |-->| obj | obj | obj | obj | ... |
 *   | per-CPU freelist (fast path)  |   +-----+-----+-----+-----+-----+
 *   +-------------------------------+     ^ ctor ran ONCE, when the slab
 *                                           page was created, not per alloc
 *
 * The constructor puts an object into its "free" state when a new slab is
 * created. kmem_cache_alloc() does NOT run it again, so an object must be
 * returned in that state (here: magic intact) before kmem_cache_free().
 * SLAB_HWCACHE_ALIGN starts every object on a cache line: no false sharing
 * between two objects used by different CPUs.
 */
#define ESH_OBJ_MAGIC 0xE5110B1Eu

struct esh_obj {
	u32 magic;		/* set by the ctor, must survive free/alloc */
	int id;
	struct list_head node;
	char payload[100];
};

static atomic_t ctor_calls = ATOMIC_INIT(0);

static void esh_obj_ctor(void *p)
{
	struct esh_obj *o = p;

	o->magic = ESH_OBJ_MAGIC;
	atomic_inc(&ctor_calls);
}

static void test_kmem_cache(void)
{
	struct kmem_cache *cache;
	struct esh_obj *o, *tmp;
	LIST_HEAD(objs);
	int i, made = 0, bad_magic = 0, misaligned = 0, ctor_first;

	cache = kmem_cache_create("esh_obj", sizeof(struct esh_obj), 0,
				  SLAB_HWCACHE_ALIGN, esh_obj_ctor);
	if (!cache) {
		check(false, "kmem_cache", "kmem_cache_create failed");
		return;
	}

	for (i = 0; i < nobj; i++) {
		o = kmem_cache_alloc(cache, GFP_KERNEL);
		if (!o)
			break;
		if (o->magic != ESH_OBJ_MAGIC)
			bad_magic++;
		if (!IS_ALIGNED((unsigned long)o, cache_line_size()))
			misaligned++;
		o->id = i;
		list_add_tail(&o->node, &objs);
		made++;
	}
	ctor_first = atomic_read(&ctor_calls);

	/* Free everything (owner: this list). _safe: we unlink while walking. */
	list_for_each_entry_safe(o, tmp, &objs, node) {
		list_del(&o->node);
		kmem_cache_free(cache, o);	/* magic untouched: ctor state */
	}

	/* Allocate again: objects come back from the freelist, no new ctor. */
	o = kmem_cache_alloc(cache, GFP_KERNEL);
	if (o) {
		bad_magic += o->magic != ESH_OBJ_MAGIC;
		kmem_cache_free(cache, o);
	}

	check(made == nobj && !bad_magic && !misaligned && ctor_first >= nobj,
	      "kmem_cache",
	      "%d objs of %zu B (kmem_cache_size %u, line %d B), ctor ran %d times, aligned, magic ok",
	      made, sizeof(struct esh_obj), kmem_cache_size(cache),
	      cache_line_size(), ctor_first);

	/* Every object must be freed first, or this WARNs "objects remaining". */
	kmem_cache_destroy(cache);
}

/* ------------------------------------------------------------------------ */
/* Test 3: vmalloc / vzalloc / kvmalloc                                     */
/* ------------------------------------------------------------------------ */
/*
 *   virtual (vmalloc area)   [p0][p1][p2][p3][p4] ...  contiguous
 *                              |   |    \   |   \
 *   physical (any RAM)       [p1]..[p0]...[p3][p2]...[p4]  scattered
 *
 * Good for big buffers only the CPU touches (module code, firmware images,
 * big tables). Bad for DMA (the device sees physical addresses) and slower
 * (page-table setup, TLB misses). See ../../08-dma-and-mmap/ for DMA.
 */
static void test_vmalloc(void)
{
	size_t size = (size_t)vmalloc_mb << 20;
	size_t i, npages = size >> PAGE_SHIFT, runs = 1;
	unsigned long prev_pfn = 0;
	u32 *buf, *kv_small, *kv_big;
	bool zero, pattern_ok = true;

	buf = vzalloc(size);
	if (!buf) {
		check(false, "vmalloc", "vzalloc(%zu) failed", size);
		return;
	}
	zero = !memchr_inv(buf, 0, size);
	for (i = 0; i < size / sizeof(u32); i++)
		buf[i] = (u32)i;
	for (i = 0; i < size / sizeof(u32); i += 997)
		pattern_ok &= buf[i] == (u32)i;

	/* Count physically contiguous runs: a new run starts at every gap. */
	for (i = 0; i < npages; i++) {
		unsigned long pfn = page_to_pfn(vmalloc_to_page((u8 *)buf + i * PAGE_SIZE));

		if (i && pfn != prev_pfn + 1)
			runs++;
		prev_pfn = pfn;
	}
	check(zero && pattern_ok && is_vmalloc_addr(buf), "vmalloc",
	      "%d MiB = %zu pages, zeroed, pattern ok, %zu physically contiguous run(s)",
	      vmalloc_mb, npages, runs);
	vfree(buf);	/* vfree may sleep: never call it in atomic context */

	/*
	 * kvmalloc: try kmalloc first (fast, contiguous), fall back to
	 * vmalloc. Free with kvfree(), which works out which one it was.
	 */
	kv_small = kvmalloc(512, GFP_KERNEL);
	kv_big = kvmalloc(size, GFP_KERNEL);
	check(kv_small && kv_big && !is_vmalloc_addr(kv_small), "kvmalloc",
	      "512 B -> kmalloc, %d MiB -> %s", vmalloc_mb,
	      kv_big && is_vmalloc_addr(kv_big) ? "vmalloc" :
	      "kmalloc (a big contiguous block was free)");
	kvfree(kv_big);
	kvfree(kv_small);
}

/* ------------------------------------------------------------------------ */
/* Test 4: the page allocator (buddy system)                                */
/* ------------------------------------------------------------------------ */
/*
 * Free memory is kept in lists of blocks of 2^order pages (see
 * /proc/buddyinfo). Asking for order 3 (8 pages):
 *
 *   free list order 4: [ 16 pages            ]        split it in two:
 *   -> order 3: [ 8 pages ] <- yours    [ 8 pages ] -> back on order-3 list
 *
 * On free, a block merges with its "buddy" (address XOR size) when that is
 * free too, rebuilding big blocks. High orders fail first on a fragmented
 * system: that is why drivers allocate big DMA buffers early or use CMA.
 */
static void test_pages(void)
{
	unsigned int i, n = 1U << order;
	struct page *page;
	unsigned long addr;
	phys_addr_t phys;
	bool contig = true, pattern_ok;
	u8 *va;

	page = alloc_pages(GFP_KERNEL, order);
	if (!page) {
		check(false, "alloc_pages", "order %d failed (fragmented?)", order);
		return;
	}
	va = page_address(page);	/* linear-map address (no highmem on arm64) */
	phys = page_to_phys(page);
	for (i = 0; i < n; i++)
		contig &= virt_to_phys(va + i * PAGE_SIZE) == phys + i * PAGE_SIZE &&
			  page_to_pfn(page + i) == page_to_pfn(page) + i;
	memset(va, 0xA5, n * PAGE_SIZE);
	pattern_ok = !memchr_inv(va, 0xA5, n * PAGE_SIZE);
	check(contig && pattern_ok && virt_to_page(va) == page, "alloc_pages",
	      "order %d = %u pages = %lu KiB physically contiguous, virt_to_page() round-trips",
	      order, n, (n * PAGE_SIZE) >> 10);
	__free_pages(page, order);	/* must pass the SAME order */

	/* Same thing, address-based API. */
	addr = __get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	if (!addr) {
		check(false, "get_free_pg", "order 1 failed");
		return;
	}
	check(!memchr_inv((void *)addr, 0, 2 * PAGE_SIZE), "get_free_pg",
	      "__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1): 2 zeroed pages");
	free_pages(addr, 1);
}

/* ------------------------------------------------------------------------ */
/* Test 5: mempool — a reserve so allocation cannot fail                    */
/* ------------------------------------------------------------------------ */
/*
 *   mempool_alloc():  1. try the normal allocator (kmalloc here)
 *                     2. failed? take an element from the reserve
 *                     3. reserve empty? SLEEP until mempool_free() refills
 *   mempool_free():   refill the reserve first, give the rest back
 *
 * Used where failure is not an option but forward progress is guaranteed
 * because elements come back: block-layer bios, writeback in low memory.
 */
#define POOL_MIN 4

static void test_mempool(void)
{
	void *elem[2 * POOL_MIN];
	mempool_t *pool;
	int i, got = 0, reserve0, reserve1;

	pool = mempool_create_kmalloc_pool(POOL_MIN, 256);
	if (!pool) {
		check(false, "mempool", "mempool_create failed");
		return;
	}
	reserve0 = pool->curr_nr;	/* pre-filled at create time */
	for (i = 0; i < ARRAY_SIZE(elem); i++) {
		/* GFP_KERNEL + mempool: never returns NULL (may sleep instead). */
		elem[i] = mempool_alloc(pool, GFP_KERNEL);
		got += !!elem[i];
	}
	for (i = 0; i < ARRAY_SIZE(elem); i++)
		if (elem[i])
			mempool_free(elem[i], pool);
	reserve1 = pool->curr_nr;
	check(got == ARRAY_SIZE(elem) && reserve0 == POOL_MIN && reserve1 == POOL_MIN,
	      "mempool", "reserve %d, %d allocs ok, reserve refilled to %d on free",
	      reserve0, got, reserve1);
	mempool_destroy(pool);	/* all elements must be back */
}

/* ------------------------------------------------------------------------ */
/* Test 6: alloc_percpu — one copy per CPU                                  */
/* ------------------------------------------------------------------------ */
struct pcpu_stats {
	u64 hits;
	int last_cpu;
};

static void pcpu_hit(void *info)
{
	struct pcpu_stats __percpu *st = info;

	/* Runs on EVERY online CPU (IPI, irqs off): touch only our copy. */
	this_cpu_inc(st->hits);
	this_cpu_write(st->last_cpu, smp_processor_id());
}

static void test_percpu(void)
{
	struct pcpu_stats __percpu *st = alloc_percpu(struct pcpu_stats);
	unsigned int online;
	u64 sum = 0;
	int cpu, wrong_cpu = 0;

	if (!st) {
		check(false, "alloc_percpu", "allocation failed");
		return;
	}
	/* alloc_percpu memory is zeroed for every possible CPU. */
	cpus_read_lock();	/* no CPU may come or go while we count */
	online = num_online_cpus();
	on_each_cpu(pcpu_hit, st, 1);
	for_each_online_cpu(cpu) {
		struct pcpu_stats *s = per_cpu_ptr(st, cpu);

		sum += s->hits;
		wrong_cpu += s->last_cpu != cpu;
	}
	cpus_read_unlock();
	check(sum == online && !wrong_cpu, "alloc_percpu",
	      "on_each_cpu() bumped %llu private counters on %u CPUs, each CPU wrote its own copy",
	      sum, online);
	free_percpu(st);
}

/* ------------------------------------------------------------------------ */
/* Test 7: GFP flags vs context — allocating where you may not sleep        */
/* ------------------------------------------------------------------------ */
/*
 * A timer callback runs in softirq context (on PREEMPT_RT: in the ktimers
 * thread, but still with the "may not sleep" rules for the code you write).
 * GFP_KERNEL may sleep to reclaim memory -> forbidden there.
 * GFP_ATOMIC never sleeps and may dip into emergency reserves.
 * GFP_NOWAIT never sleeps and does NOT touch the reserves (fails sooner).
 */
static struct {
	struct timer_list timer;
	struct completion done;
	bool atomic_ok, nowait_ok, was_task;
} gfp_t_ctx;

static void gfp_timer_fn(struct timer_list *t)
{
	void *a = kmalloc(128, GFP_ATOMIC);
	void *b = kmalloc(128, GFP_NOWAIT);

	gfp_t_ctx.atomic_ok = !!a;
	gfp_t_ctx.nowait_ok = !!b;
	gfp_t_ctx.was_task = in_task();
	kfree(b);
	kfree(a);
	complete(&gfp_t_ctx.done);
}

/* static: lockdep needs locks in static memory or spin_lock_init() */
static DEFINE_SPINLOCK(gfp_lock);

static void test_gfp_context(void)
{
	void *p;

	/* Under a spinlock: also atomic context (on !RT), GFP_ATOMIC only. */
	spin_lock(&gfp_lock);
	p = kmalloc(64, GFP_ATOMIC);
	spin_unlock(&gfp_lock);
	kfree(p);

	init_completion(&gfp_t_ctx.done);
	timer_setup(&gfp_t_ctx.timer, gfp_timer_fn, 0);
	mod_timer(&gfp_t_ctx.timer, jiffies + 1);
	wait_for_completion(&gfp_t_ctx.done);
	timer_shutdown_sync(&gfp_t_ctx.timer);	/* callback has finished */

	check(p && gfp_t_ctx.atomic_ok && gfp_t_ctx.nowait_ok, "gfp_context",
	      "GFP_ATOMIC under spinlock ok; in timer cb (in_task=%d) GFP_ATOMIC + GFP_NOWAIT ok",
	      gfp_t_ctx.was_task);
}

/* ------------------------------------------------------------------------ */

static int __init kmem_demo_init(void)
{
	if (nobj < 1 || nobj > 100000 || vmalloc_mb < 1 || vmalloc_mb > 64 ||
	    order < 0 || order > MAX_PAGE_ORDER) {
		pr_err("nobj must be 1..100000, vmalloc_mb 1..64, order 0..%d\n",
		       MAX_PAGE_ORDER);
		return -EINVAL;
	}
	pr_info("start: PAGE_SIZE %lu, nobj %d, vmalloc %d MiB, order %d\n",
		PAGE_SIZE, nobj, vmalloc_mb, order);

	test_layout();
	test_kmalloc();
	test_kmem_cache();
	test_vmalloc();
	test_pages();
	test_mempool();
	test_percpu();
	test_gfp_context();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed, everything freed\n");
	/* Return 0 even on FAIL so the module can be inspected and rmmod'ed. */
	return 0;
}

static void __exit kmem_demo_exit(void)
{
	/* Every test freed its memory before returning: nothing to do. */
	pr_info("unloaded\n");
}

module_init(kmem_demo_init);
module_exit(kmem_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: kmalloc, kmem_cache, vmalloc, pages, mempool, per-CPU, GFP");
