// SPDX-License-Identifier: GPL-2.0
/*
 * kds_demo.c — the kernel's own data structures in one "process table":
 *              list_head, hlist + hashtable.h, rbtree, xarray, IDA,
 *              bitmap, kfifo, kref, and container_of() under all of them
 *
 * Concept : Kernel containers are INTRUSIVE: the node (list_head, hlist_node,
 *           rb_node) is embedded in your struct and container_of() gets the
 *           struct back from the node. So one object can sit in several
 *           containers at once with no extra allocation. The module builds a
 *           table of fake processes keyed by pid: a list in creation order,
 *           a hash table for O(1) lookup by pid, an rbtree for sorted
 *           walks, an xarray mapping a small id to the object; an IDA hands
 *           out unique pids; kref decides when an object is freed. It then
 *           deletes every odd pid while walking the list and checks every
 *           container is still consistent.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/02-kernel-memory-and-data-structures/kernel)
 * Run     : sudo insmod kds_demo.ko [nproc=500]
 *           dmesg | grep kds_demo       -> one PASS line per test
 *           sudo rmmod kds_demo
 * Read    : ../README.md  (section 3.10 "Kernel data structures")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitmap.h>
#include <linux/hashtable.h>
#include <linux/idr.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/xarray.h>

static int nproc = 500;
module_param(nproc, int, 0444);
MODULE_PARM_DESC(nproc, "fake processes in the table (2..10000, default 500)");

#define ESH_PID_MIN 300		/* like the real pid allocator: low pids are reserved */
#define ESH_PID_MAX 32767

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
		pr_info("PASS %-13s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-13s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ------------------------------------------------------------------------ */
/* The object: one struct, four containers, no extra allocations             */
/* ------------------------------------------------------------------------ */
/*
 *   struct esh_proc  (one kmalloc)
 *   +-------------------+ <- p                container_of(node_ptr,
 *   | pid, id, comm ... |                         struct esh_proc, rbnode)
 *   | kref              |                     = (void *)node_ptr
 *   | lnode   {next,prev}  <- proc_list            - offsetof(struct esh_proc, rbnode)
 *   | hnode   {next,pprev} <- proc_hash[hash(pid)]
 *   | rbnode  {parent,l,r} <- proc_tree  <-- node_ptr
 *   +-------------------+
 *   and proc_xa[id] -> p   (the xarray stores the pointer itself)
 *
 * All containers here are touched only from module init/exit, which run one
 * at a time, so there is no lock. A real driver protects them with a mutex
 * (or a spinlock if an IRQ path looks things up, or RCU for lock-free
 * lookups: hash_add_rcu / hlist_for_each_entry_rcu / xa_load is RCU-safe).
 */
struct esh_proc {
	pid_t pid;
	u32 id;				/* index in proc_xa */
	char comm[TASK_COMM_LEN];
	struct kref ref;		/* the table holds 1, every user +1 */
	struct list_head lnode;		/* proc_list, creation order */
	struct hlist_node hnode;	/* proc_hash bucket, key = pid */
	struct rb_node rbnode;		/* proc_tree, ordered by pid */
};

#define PROC_HASH_BITS 6		/* 64 buckets */
static DEFINE_HASHTABLE(proc_hash, PROC_HASH_BITS);
static struct rb_root_cached proc_tree = RB_ROOT_CACHED;
static LIST_HEAD(proc_list);
static DEFINE_XARRAY_ALLOC1(proc_xa);	/* xa_alloc() ids start at 1 */
static DEFINE_IDA(pid_ida);
static int nr_released;			/* objects freed by the kref release */

static void esh_proc_release(struct kref *kref)
{
	struct esh_proc *p = container_of(kref, struct esh_proc, ref);

	/* Already unlinked from every container by the caller. */
	ida_free(&pid_ida, p->pid);
	kfree(p);
	nr_released++;
}

static void esh_proc_put(struct esh_proc *p)
{
	kref_put(&p->ref, esh_proc_release);
}

/* ---- rbtree: the tree gives you balancing, YOU write the walk ----------- */
/*
 *            [500]             insert 420: go left at 500, right at 300,
 *           /     \            link as 300's right child (rb_link_node),
 *       [300]     [800]        then rb_insert_color*() recolours/rotates.
 *           \
 *          (420)
 */
static void tree_insert(struct esh_proc *new)
{
	struct rb_node **link = &proc_tree.rb_root.rb_node, *parent = NULL;
	bool leftmost = true;

	while (*link) {
		struct esh_proc *cur = rb_entry(*link, struct esh_proc, rbnode);

		parent = *link;
		if (new->pid < cur->pid) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;	/* went right once: not the minimum */
		}
	}
	rb_link_node(&new->rbnode, parent, link);
	/* _cached keeps a pointer to the leftmost node: rb_first_cached() is O(1). */
	rb_insert_color_cached(&new->rbnode, &proc_tree, leftmost);
}

static int pid_cmp(const void *key, const struct rb_node *node)
{
	pid_t pid = *(const pid_t *)key;
	pid_t other = rb_entry(node, struct esh_proc, rbnode)->pid;

	return pid < other ? -1 : pid > other;
}

static struct esh_proc *tree_find(pid_t pid)
{
	/* rb_find() (5.12+) is the generic walk: cmp(key, node) < 0 -> left. */
	struct rb_node *n = rb_find(&pid, &proc_tree.rb_root, pid_cmp);

	return n ? rb_entry(n, struct esh_proc, rbnode) : NULL;
}

/* ---- hashtable.h: an array of hlist_head, hashed with hash_min() -------- */
/*
 *   proc_hash[0]  -> NULL
 *   proc_hash[1]  -> [pid 812] -> [pid 3077] -> NULL     hlist: the head is
 *   proc_hash[2]  -> [pid 455] -> NULL                   ONE pointer (half the
 *   ...                                                  size of list_head)
 */
static struct esh_proc *hash_find(pid_t pid)
{
	struct esh_proc *p;

	/* Walks only the one bucket hash(pid) maps to: O(1) average. */
	hash_for_each_possible(proc_hash, p, hnode, pid)
		if (p->pid == pid)
			return p;
	return NULL;
}

/* Allocate a unique pid, like alloc_pid(): random start, lowest free above it. */
static int alloc_fake_pid(void)
{
	int start = ESH_PID_MIN + get_random_u32_below(ESH_PID_MAX - ESH_PID_MIN);
	int pid = ida_alloc_range(&pid_ida, start, ESH_PID_MAX, GFP_KERNEL);

	if (pid == -ENOSPC)	/* nothing free above start: wrap around */
		pid = ida_alloc_range(&pid_ida, ESH_PID_MIN, ESH_PID_MAX, GFP_KERNEL);
	return pid;
}

static struct esh_proc *proc_add(int n)
{
	struct esh_proc *p = kzalloc(sizeof(*p), GFP_KERNEL);
	int pid, err;

	if (!p)
		return NULL;
	pid = alloc_fake_pid();
	if (pid < 0)
		goto err_free;
	p->pid = pid;
	scnprintf(p->comm, sizeof(p->comm), "worker/%d", n);
	kref_init(&p->ref);	/* refcount 1: the table's reference */

	/* xa_alloc picks a free index for us and stores p there. */
	err = xa_alloc(&proc_xa, &p->id, p, xa_limit_32b, GFP_KERNEL);
	if (err)
		goto err_pid;
	if (n % 3 == 0)		/* marks: up to 3 free bits per entry */
		xa_set_mark(&proc_xa, p->id, XA_MARK_0);

	list_add_tail(&p->lnode, &proc_list);
	hash_add(proc_hash, &p->hnode, p->pid);
	tree_insert(p);
	return p;

err_pid:
	ida_free(&pid_ida, pid);
err_free:
	kfree(p);
	return NULL;
}

/* Unlink from every container, then drop the table's reference. */
static void proc_remove(struct esh_proc *p)
{
	list_del(&p->lnode);
	hash_del(&p->hnode);
	rb_erase_cached(&p->rbnode, &proc_tree);
	xa_erase(&proc_xa, p->id);
	esh_proc_put(p);	/* freed now, unless someone still holds a ref */
}

/* ------------------------------------------------------------------------ */
/* Test 1: container_of                                                      */
/* ------------------------------------------------------------------------ */
static void test_container_of(void)
{
	struct esh_proc p = { .pid = 4242 };
	struct rb_node *n = &p.rbnode;
	struct list_head *l = &p.lnode;

	check(container_of(n, struct esh_proc, rbnode) == &p &&
	      list_entry(l, struct esh_proc, lnode)->pid == 4242 &&
	      (void *)n - offsetof(struct esh_proc, rbnode) == (void *)&p,
	      "container_of", "node at +%zu / +%zu -> back to the struct (pid %d)",
	      offsetof(struct esh_proc, rbnode), offsetof(struct esh_proc, lnode),
	      container_of(n, struct esh_proc, rbnode)->pid);
}

/* ------------------------------------------------------------------------ */
/* Test 2: the process table                                                 */
/* ------------------------------------------------------------------------ */
static void test_proc_table(void)
{
	struct esh_proc *p, *tmp, *held;
	struct rb_node *rb;
	unsigned long idx;
	int i, made = 0, found_hash = 0, found_tree = 0, found_xa = 0;
	int in_list = 0, in_hash = 0, in_tree = 0, in_xa = 0, marked = 0;
	int removed = 0, bkt, used_bkts = 0, max_chain = 0;
	const int nbkt = HASH_SIZE(proc_hash);
	pid_t prev = -1, min_pid = ESH_PID_MAX + 1;
	bool sorted = true;

	for (i = 0; i < nproc; i++) {
		p = proc_add(i);
		if (!p)
			break;
		made++;
		min_pid = min(min_pid, p->pid);
	}

	/* Every pid must be found by the hash, the tree and the xarray. */
	list_for_each_entry(p, &proc_list, lnode) {
		found_hash += hash_find(p->pid) == p;
		found_tree += tree_find(p->pid) == p;
		found_xa += xa_load(&proc_xa, p->id) == p;
	}
	check(made == nproc && found_hash == made && found_tree == made &&
	      found_xa == made, "lookup", "%d procs: found by hash %d, rbtree %d, xarray %d",
	      made, found_hash, found_tree, found_xa);

	/* Hash quality: how full are the buckets? */
	for (bkt = 0; bkt < nbkt; bkt++) {
		int chain = 0;

		hlist_for_each_entry(p, &proc_hash[bkt], hnode)
			chain++;
		used_bkts += chain > 0;
		max_chain = max(max_chain, chain);
	}
	pr_info("INFO hashtable    %d buckets, %d used, longest chain %d (average %d)\n",
		nbkt, used_bkts, max_chain, made / nbkt);

	/* In-order rbtree walk = sorted by pid; leftmost cached = minimum. */
	for (rb = rb_first_cached(&proc_tree); rb; rb = rb_next(rb)) {
		p = rb_entry(rb, struct esh_proc, rbnode);
		sorted &= p->pid > prev;
		prev = p->pid;
	}
	p = rb_entry_safe(rb_first_cached(&proc_tree), struct esh_proc, rbnode);
	check(sorted && p && p->pid == min_pid, "rbtree",
	      "in-order walk strictly increasing, leftmost (cached) pid %d is the minimum",
	      p ? p->pid : -1);

	/*
	 * kref: a "user" looks up an odd-pid process and keeps a reference
	 * while the table deletes it. Its memory must stay valid until the
	 * user's put, whoever comes last frees it.
	 */
	held = NULL;
	list_for_each_entry(p, &proc_list, lnode) {
		if (p->pid & 1) {
			held = p;
			kref_get(&held->ref);	/* refcount 1 -> 2 */
			break;
		}
	}

	/*
	 * Delete every odd pid WHILE walking the list. list_for_each_entry()
	 * would read p->lnode.next AFTER p was freed; the _safe variant saved
	 * the next element in tmp before the body runs.
	 *
	 *    p        tmp
	 *   [A] <-> [B] <-> [C]     list_del(A) + free(A): tmp still valid
	 */
	list_for_each_entry_safe(p, tmp, &proc_list, lnode) {
		if (p->pid & 1) {
			proc_remove(p);
			removed++;
		}
	}

	list_for_each_entry(p, &proc_list, lnode)
		in_list++;
	hash_for_each(proc_hash, bkt, p, hnode)
		in_hash++;
	for (rb = rb_first_cached(&proc_tree); rb; rb = rb_next(rb))
		in_tree++;
	xa_for_each(&proc_xa, idx, p)
		in_xa++;
	xa_for_each_marked(&proc_xa, idx, p, XA_MARK_0)
		marked++;
	check(in_list == made - removed && in_hash == in_list &&
	      in_tree == in_list && in_xa == in_list, "delete-safe",
	      "removed %d odd pids during list_for_each_entry_safe, %d left in list/hash/tree/xarray (%d marked)",
	      removed, in_list, marked);

	if (held) {
		pid_t pid = held->pid;		/* still valid: we hold a ref */
		bool alive = nr_released == removed - 1;

		esh_proc_put(held);		/* last reference -> release */
		check(alive && nr_released == removed, "kref",
		      "pid %d unlinked but alive while referenced, freed by the user's last put",
		      pid);
	}

	/* Teardown: remove the rest, everything must be empty and freed. */
	list_for_each_entry_safe(p, tmp, &proc_list, lnode)
		proc_remove(p);
	check(nr_released == made && hash_empty(proc_hash) &&
	      RB_EMPTY_ROOT(&proc_tree.rb_root) && xa_empty(&proc_xa) &&
	      ida_is_empty(&pid_ida) && list_empty(&proc_list), "teardown",
	      "%d objects released, hash/rbtree/xarray/IDA/list all empty", nr_released);
}

/* ------------------------------------------------------------------------ */
/* Test 3: IDA — "give me the lowest free integer"                           */
/* ------------------------------------------------------------------------ */
/*
 * IDA = ID allocator only (a bitmap inside an xarray). IDR/xa_alloc =
 * ID -> pointer map. Used for minor numbers, /dev/i2c-N, ida for dev ids.
 */
static void test_ida(void)
{
	DEFINE_IDA(ida);
	int ids[8], i, again;
	bool ok = true;

	for (i = 0; i < ARRAY_SIZE(ids); i++) {
		ids[i] = ida_alloc_max(&ida, 99, GFP_KERNEL);
		ok &= ids[i] == i;		/* lowest free first: 0, 1, 2 ... */
	}
	ida_free(&ida, 3);
	ida_free(&ida, 5);
	again = ida_alloc(&ida, GFP_KERNEL);	/* reuses the LOWEST hole */
	ok &= again == 3;
	for (i = 0; i < ARRAY_SIZE(ids); i++)
		if (ids[i] >= 0 && i != 3 && i != 5)
			ida_free(&ida, ids[i]);
	if (again >= 0)
		ida_free(&ida, again);
	check(ok && ida_is_empty(&ida), "ida",
	      "ids 0..7 in order, freed 3 and 5, next alloc reused %d, empty after freeing", again);
	ida_destroy(&ida);
}

/* ------------------------------------------------------------------------ */
/* Test 4: bitmap API                                                         */
/* ------------------------------------------------------------------------ */
/*
 *   bit:   0 1 2 3 4 5 6 7 8 9 ...
 *   map:   1 1 0 1 0 0 0 1 0 0      find_first_zero_bit -> 2
 *                  ^^^^^            bitmap_find_next_zero_area(len 3) -> 4
 */
#define NBITS 200

static void test_bitmap(void)
{
	unsigned long *a = bitmap_zalloc(NBITS, GFP_KERNEL);
	unsigned long *b = bitmap_zalloc(NBITS, GFP_KERNEL);
	unsigned long first0, area, bit;
	unsigned int weight, and_w, n = 0;

	if (!a || !b) {
		check(false, "bitmap", "allocation failed");
		goto out;
	}
	bitmap_set(a, 0, 2);		/* bits 0,1 */
	__set_bit(3, a);		/* __ = non-atomic: we own the map */
	__set_bit(7, a);
	first0 = find_first_zero_bit(a, NBITS);
	area = bitmap_find_next_zero_area(a, NBITS, 0, 3, 0);
	bitmap_set(b, 0, 100);		/* bits 0..99 */
	__set_bit(150, a);
	weight = bitmap_weight(a, NBITS);
	bitmap_and(b, a, b, NBITS);	/* b = a & b: drops bit 150 */
	and_w = bitmap_weight(b, NBITS);
	for_each_set_bit(bit, a, NBITS)
		n++;
	/* %*pbl prints a bitmap as a range list, e.g. "0-1,3,7,150". */
	pr_info("INFO bitmap      a = %*pbl, a & [0..99] = %*pbl\n",
		NBITS, a, NBITS, b);
	check(first0 == 2 && area == 4 && weight == 5 && and_w == 4 && n == 5,
	      "bitmap", "first zero %lu, 3-bit free area at %lu, weight %u, after AND %u",
	      first0, area, weight, and_w);
out:
	bitmap_free(b);
	bitmap_free(a);
}

/* ------------------------------------------------------------------------ */
/* Test 5: kfifo — lock-free single-producer / single-consumer ring          */
/* ------------------------------------------------------------------------ */
/*
 * Size must be a power of two: in/out are free-running counters and the
 * index is (counter & (size - 1)). One producer + one consumer need no lock;
 * more of either need one (see /dev/ksync in ../../03-kernel-synchronization/).
 */
static void test_kfifo(void)
{
	DEFINE_KFIFO(fifo, int, 16);	/* on the stack, 16 ints */
	int i, v, sum = 0;
	bool order_ok = true, full, put_full;

	for (i = 0; i < 16; i++)
		kfifo_put(&fifo, i);
	full = kfifo_is_full(&fifo);
	put_full = kfifo_put(&fifo, 99) == 0;	/* no room: returns 0, drops it */
	for (i = 0; i < 16 && kfifo_get(&fifo, &v); i++) {
		order_ok &= v == i;
		sum += v;
	}
	check(full && put_full && order_ok && kfifo_is_empty(&fifo), "kfifo",
	      "16 ints in, put on full refused, out in FIFO order (sum %d)", sum);
}

/* ------------------------------------------------------------------------ */

static int __init kds_demo_init(void)
{
	if (nproc < 2 || nproc > 10000) {
		pr_err("nproc must be 2..10000\n");
		return -EINVAL;
	}
	pr_info("start: %d fake processes\n", nproc);

	test_container_of();
	test_proc_table();
	test_ida();
	test_bitmap();
	test_kfifo();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed, everything freed\n");
	return 0;
}

static void __exit kds_demo_exit(void)
{
	struct esh_proc *p, *tmp;

	/* Only non-empty if a test bailed out early: free what is left. */
	list_for_each_entry_safe(p, tmp, &proc_list, lnode)
		proc_remove(p);
	xa_destroy(&proc_xa);
	ida_destroy(&pid_ida);
	pr_info("unloaded\n");
}

module_init(kds_demo_init);
module_exit(kds_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: list, hlist/hashtable, rbtree, xarray, IDA, bitmap, kfifo, kref");
