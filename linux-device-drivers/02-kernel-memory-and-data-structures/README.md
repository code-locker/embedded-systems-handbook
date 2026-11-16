# 02: Kernel Memory and Data Structures

> User space has one `malloc()`. The kernel has half a dozen allocators,
> because "give me memory" depends on how much you want, whether a device
> will read it, whether you are allowed to sleep, and whether the
> allocation may fail at all. This lesson goes through each one (the page
> allocator, slab caches, `kmalloc`, `vmalloc`, `kvmalloc`, mempool,
> per-CPU memory), the GFP flags that say what the allocator may do, and
> the kernel's own **intrusive** containers (`list_head`, `hlist`,
> `rbtree`, `xarray`, IDA, `kfifo`, bitmaps, `kref`) built on
> `container_of()`. Two self-testing modules prove each one on the
> BeagleBone AI-64, and a deliberately leaking module gives kmemleak
> something to find.

---

## 1. Why this exists

A driver allocates memory all the time: a per-device struct at probe, a
request object per I/O, a ring of DMA buffers, a lookup table, a
per-open-file context. In user space all of that is `malloc`/`free`, and
the worst a bug can do is crash your process. In the kernel:

- **No process exit cleans up after you.** Memory a module allocates and
  forgets stays allocated until reboot, even after `rmmod`.
- **Some code may not sleep** (interrupt handlers, softirqs, code holding a
  spinlock; see [03 "may not sleep"](../03-kernel-synchronization/README.md#1-why-this-exists)).
  An allocator that waits for the disk to free memory can't be used there.
- **Devices see physical addresses.** A buffer that is contiguous for the
  CPU may be scattered in RAM, and a DMA engine can't use it as is.
- **Allocation failure is real.** On a 4 GiB board running a camera
  pipeline, an order-4 (64 KiB contiguous) allocation can fail after days
  of uptime because RAM is fragmented.

So the kernel offers several allocators, each with a different promise, and
the GFP flags let you say what each call may do. On top of them sit the
kernel's generic containers, which every subsystem reuses instead of
writing its own lists and trees.

---

## 2. Mental model

**A warehouse.** The **page allocator** is the loading dock: it hands out
whole pallets (4 KiB pages) or stacks of 2, 4, 8 … pallets that sit next to
each other (physically contiguous). The **slab allocator** takes pallets
and puts shelves on them, with boxes of one fixed size per shelf (32 B,
64 B, 128 B … or exactly your struct). `kmalloc` picks the shelf with the
smallest box that fits. **vmalloc** is for when you need 40 pallets and the
dock has no 40 in a row: it takes 40 loose pallets from anywhere and gives
you a catalogue (page tables) that makes them *look* adjacent. A **mempool**
is a few boxes kept in the back room for emergencies.

```
                 your driver
   kmalloc  kzalloc  kcalloc   kmem_cache_alloc   vmalloc  kvmalloc  alloc_percpu
      \        |        /             |               |      /  \         |
       +--- SLUB slab allocator ------+               |  kmalloc  vmalloc  percpu
            kmalloc-8 ... kmalloc-8k,                  |  first,   fallback  chunks
            plus private caches ("esh_obj")           |
                        |                     vmap: page tables in the
                        |                     vmalloc area, any pages
                        v                             |
   alloc_pages(gfp, order) / __get_free_pages  <------+
   BUDDY ALLOCATOR: free lists of 2^0 .. 2^10 contiguous pages per zone
                        |
        ZONE_DMA | ZONE_DMA32 | ZONE_NORMAL  (+ CMA region for big DMA buffers)
                        |
                  physical RAM (board: 4 GiB LPDDR4)
```

The sentence to remember: **`kmalloc` for small things, `kmem_cache` for
many identical things, `vmalloc`/`kvmalloc` for big things only the CPU
touches, `alloc_pages` for whole pages, the DMA API for anything a device
reads, and the GFP flag says whether the call may sleep.**

---

## 3. How it works

### 3.1 The kernel's virtual memory map (arm64)

Every process has its own user half of the address space; the kernel half
is shared by all. On the board (arm64, 48-bit virtual addresses, 4 KiB
pages) the kernel half looks like this (`Documentation/arch/arm64/memory.rst`):

```
 0xffff_ffff_ffff_ffff  +-----------------------------+
                        | fixmap, PCI I/O window       |
                        | vmemmap: struct page array   |  one 64-byte struct page
                        |                              |  per 4 KiB of RAM
                        +-----------------------------+
                        | vmalloc / ioremap area       |  vmalloc(), ioremap(),
                        |   kernel image (_text.._end) |  vmap(); 100s of TiB
                        |   modules area (2 GiB)       |  <- your .ko lives here
 0xffff_8000_0000_0000  +-----------------------------+  (approx; KASLR moves things)
                        | linear map ("lowmem")        |  ALL physical RAM mapped
                        |   virt = phys + const offset |  1:1 at boot: kmalloc,
                        |                              |  alloc_pages, kmem_cache
 0xffff_0000_0000_0000  +-----------------------------+  PAGE_OFFSET
                        |     (not mapped: a hole)     |
 0x0000_ffff_ffff_ffff  +-----------------------------+
                        |   user space of the current  |  TTBR0; reached only via
                        |   process                    |  copy_to/from_user()
 0x0000_0000_0000_0000  +-----------------------------+
```

- **Linear map.** At boot the kernel maps all RAM once, at a fixed offset.
  For any address in it, `virt_to_phys()` is one subtraction, and
  `virt_to_page()` finds its `struct page`. `kmalloc` and the page
  allocator return addresses here. `virt_addr_valid(p)` tells you whether
  `p` is in it.
- **Highmem** is a 32-bit-only thing (RAM that didn't fit in the 1 GiB
  kernel window, mapped temporarily with `kmap`). On arm64 and x86-64
  there is no highmem: all RAM is in the linear map, `page_address()`
  always works. You will still see `kmap_local_page()` in portable drivers.
- **vmalloc area.** A huge virtual range filled on demand by page tables.
  `vmalloc`, `ioremap` (device registers), `vmap` and, on arm64, loadable
  modules all live here. These addresses are **not** in the linear map:
  `virt_to_phys()` on them returns garbage. Use `vmalloc_to_page()`.
- **x86-64** has the same shape, with different addresses (`page_offset_base`,
  `vmalloc_base` randomised by KASLR); see `Documentation/arch/x86/x86_64/mm.rst`.

`kmem_demo show_layout=1` prints the real boundaries and where one
`kmalloc`, one `vmalloc` and the module's own `.data` landed. The
addresses defeat KASLR, so it is off by default. Look around from user
space:

```sh
grep -E 'MemTotal|MemFree|Slab|SReclaimable|VmallocUsed|Percpu|KernelStack|PageTables' /proc/meminfo
sudo cat /proc/vmallocinfo | head      # who mapped what in the vmalloc area
sudo cat /proc/iomem | head -30        # physical map: RAM ranges vs device registers
```

### 3.2 The page allocator and the buddy system  (`kmem_demo` test 4)

Physical memory is managed in **pages** (4 KiB on the board). Each page has
a `struct page` (in `vmemmap`) describing it. The **buddy allocator** keeps,
for each zone, free lists of blocks of 2<sup>order</sup> contiguous pages,
order 0 (4 KiB) up to `MAX_PAGE_ORDER` (10 = 4 MiB).

```
 alloc order 1 (2 pages) when only an order-3 block is free:

  order 3: [0 1 2 3 4 5 6 7]                 split ->
  order 2: [0 1 2 3]           [4 5 6 7] -> free list 2
  order 1: [0 1] <- yours  [2 3] -> free list 1

 free [0 1]: its buddy is [2 3] (address XOR block size). Free too?
 merge -> [0 1 2 3]; its buddy [4 5 6 7] free? merge -> [0..7]
```

Allocation and free are O(number of orders), so effectively O(1). The
cost is **external fragmentation**: after a long uptime there may be
plenty of free memory but no free order-4 block. The kernel fights this
with migration types (movable pages are grouped so they can be moved),
compaction, and **CMA** (a reserved region lent to movable pages and taken
back for large contiguous DMA allocations; camera and display drivers on
the TDA4VM use it).

```sh
cat /proc/buddyinfo        # free blocks per order, per zone
# Node 0, zone      DMA     3      2      4      3      4      2      3      2      3      2    471
# Node 0, zone   Normal    51     95     60     37     23     12      6      2      1      2     86
#                         ^order 0                                                      order 10^
sudo cat /proc/pagetypeinfo   # the same split by migrate type (Unmovable, Movable, ...)
```

**Zones** exist because some devices can't address all of RAM. `ZONE_DMA`
and `ZONE_DMA32` hold the low physical addresses, `ZONE_NORMAL` the rest.
On arm64 the size of `ZONE_DMA` is set at boot from the most limited
device in the device tree. Drivers don't pick zones by hand any more:
the DMA API does it from the device's DMA mask
([08 DMA and mmap](../08-dma-and-mmap/)).

| Call | Returns | Free with |
|---|---|---|
| `alloc_pages(gfp, order)` | `struct page *` of the first page | `__free_pages(page, order)` |
| `alloc_page(gfp)` | one page | `__free_page(page)` |
| `__get_free_pages(gfp, order)` | linear-map address (`unsigned long`) | `free_pages(addr, order)` |
| `get_zeroed_page(gfp)` | one zeroed page, address | `free_page(addr)` |
| `page_address(page)` / `virt_to_page(addr)` | convert between the two | |
| `alloc_pages_exact(size, gfp)` | exact size (frees the tail of the 2^n block) | `free_pages_exact()` |

You must free with the **same order** you allocated: the allocator doesn't
remember it for you.

### 3.3 Slab caches and `kmalloc`  (`kmem_demo` tests 1 and 2)

Most kernel objects are far smaller than a page. The **slab allocator**
(SLUB is the only one left since 6.8) gets pages from the buddy allocator
and cuts them into equal objects. A *cache* is a set of slabs for one
object size, with a per-CPU free list for the fast path (no lock, no
atomic in the common case).

```
 kmem_cache "kmalloc-128"
   per-CPU slab (CPU0) --> [obj][obj][free][obj][free]...   4 KiB page / 128
   per-CPU slab (CPU1) --> [free][obj][obj][free]...
   partial slabs (per node) --> ...
```

**kmalloc** is a set of generic caches, one per size class. Your size is
rounded up to the next class:

| You ask for | You get (ksize) | From |
|---|---|---|
| 1 … 8 | 8 | kmalloc-8 |
| 13 | 16 | kmalloc-16 |
| 33 | 64 | kmalloc-64 |
| 65 | 96 | kmalloc-96 (one of two non-power-of-two classes, with 192) |
| 129 | 192 | kmalloc-192 |
| 300 | 512 | kmalloc-512 |
| 1025 | 2048 | kmalloc-2k |
| 4097 | 8192 | kmalloc-8k (the biggest slab class, `KMALLOC_MAX_CACHE_SIZE`) |
| 8193 | 16384 | page allocator, order 2 |

(On arm64 the smallest classes can be bigger. `kmalloc` must return
memory safe for non-coherent DMA, so with 128-byte cache-line alignment
and no bounce buffering the minimum is kmalloc-128. Since 6.5 the kernel
decides this at boot. `kmem_demo` prints what your board really does.)

- `ksize(p)` tells you how much you really got. `kmalloc_size_roundup(n)`
  tells you in advance. **Don't use the slack** after `ksize()`: KASAN
  treats it as out of bounds. Ask for the rounded size instead.
- `kzalloc` = zeroed. `kcalloc(n, size)` / `kmalloc_array` check `n*size`
  for overflow (`kmem_demo` shows `kcalloc(SIZE_MAX/2, 4)` returning NULL).
  `krealloc` keeps the contents, and on failure the old buffer is still
  yours: never write `p = krealloc(p, …)`.
- `kmalloc(0)` returns `ZERO_SIZE_PTR` (address 16), not NULL. Any access
  faults; `kfree` accepts it.
- `kmalloc` memory is **physically contiguous** and in the linear map.
  Large `kmalloc` (more than a few pages) fails easily on a fragmented system.
- `kfree(NULL)` is fine. `kfree` of a pointer you didn't get from `kmalloc`,
  or twice, corrupts the slab (KASAN or `slub_debug` catches it).

**Your own cache**, `kmem_cache_create()`, for many objects of one type
(inodes, dentries, `sk_buff`s, `task_struct`s, your I/O requests):

```c
cache = kmem_cache_create("esh_obj", sizeof(struct esh_obj), 0,
                          SLAB_HWCACHE_ALIGN, esh_obj_ctor);
o = kmem_cache_alloc(cache, GFP_KERNEL);   /* or kmem_cache_zalloc */
kmem_cache_free(cache, o);
kmem_cache_destroy(cache);                  /* all objects must be freed first */
/* shortcut: KMEM_CACHE(esh_obj, SLAB_HWCACHE_ALIGN) uses the struct name and alignment */
```

- Exact size (no rounding waste), its own line in `/proc/slabinfo` so you
  can see leaks per type, debug options per cache (`slub_debug=FZPU,esh_obj`).
- `SLAB_HWCACHE_ALIGN`: each object starts on a cache line, so two
  objects used by two CPUs never share a line (false sharing).
  `SLAB_ACCOUNT` charges the memory to the caller's cgroup.
  `SLAB_TYPESAFE_BY_RCU`: memory isn't returned to the page allocator until
  an RCU grace period, which some lock-free lookups need.
- **The constructor runs when a slab page is created, not on every
  `kmem_cache_alloc`.** It sets up the object's "free" state once. You
  must give objects back in that state. `kmem_demo` allocates 1000 objects
  and sees the ctor called about 1024 times (whole slabs), then frees and
  reallocates without new ctor calls.

```sh
sudo cat /proc/slabinfo | head -3; sudo grep -E 'kmalloc-(64|128|8k) ' /proc/slabinfo
sudo slabtop -o | head -20                       # biggest caches first
ls /sys/kernel/slab/kmalloc-128/                 # object_size, objs_per_slab, ...
```

`/proc/slabinfo` is root-only because slab addresses and counts help
exploits.

### 3.4 `vmalloc`, `vzalloc`, `kvmalloc`  (`kmem_demo` test 3)

```
 vmalloc(16 KiB):
   virtual   0xffff8000_8a3c0000 [page][page][page][page]   contiguous
                                    |     |     |     |
   physical  ...[p2]....[p0]......[p3]...........[p1]...    anywhere
```

`vmalloc(size)` takes order-0 pages from anywhere and maps them next to
each other in the vmalloc area. It can satisfy big requests that `kmalloc`
can't, but:

- **Not physically contiguous**: never hand it to a device for DMA. Never
  `virt_to_phys()` it. `kmem_demo` counts the physically contiguous runs in
  a 4 MiB `vzalloc` buffer: typically hundreds.
- Slower: page-table setup at allocation, TLB pressure on use, and every
  allocation is at least one page plus a guard page.
- `vmalloc`/`vfree` may sleep: process context only. (`vfree` from
  interrupt context is deferred to a workqueue, but don't rely on it.)

`kvmalloc(size, GFP_KERNEL)` tries `kmalloc` first (without retrying hard)
and falls back to `vmalloc`. That is the right default for "a table whose
size depends on user input". Free it with `kvfree()`, which checks
`is_vmalloc_addr()` for you. `kvmalloc` only accepts `GFP_KERNEL`-compatible
flags, since `vmalloc` must be able to sleep.

### 3.5 GFP flags: what the allocator may do

Every allocation takes a `gfp_t` ("get free pages" flags). It answers:
*may you sleep? may you start I/O to free memory? which zone? zero it?*

| Flag | Sleeps? | Use it in | Notes |
|---|---|---|---|
| `GFP_KERNEL` | yes (reclaim, compaction, I/O) | process context: `probe`, `open`, `ioctl`, kthreads, workqueues | the default; almost never fails for small sizes |
| `GFP_ATOMIC` | **never** | IRQ handlers, softirq/tasklet/timer callbacks, under a spinlock | may use emergency reserves; fails more often: always check |
| `GFP_NOWAIT` | **never** | atomic context when failure is cheap (you can retry or drop) | does not touch the reserves; kinder to the system than `GFP_ATOMIC` |
| `GFP_NOIO` / `GFP_NOFS` | yes, but no I/O / no filesystem calls | block-device / filesystem paths (avoid reclaim recursion) | prefer `memalloc_noio_save()` scopes in new code |
| `GFP_USER` / `GFP_HIGHUSER_MOVABLE` | yes | memory mapped to user space | |
| `__GFP_ZERO` | (modifier) | | zero the memory: `kzalloc` = `kmalloc(…, GFP_KERNEL \| __GFP_ZERO)` |
| `__GFP_NOWARN` | (modifier) | | don't print the allocation-failure splat (you handle failure) |
| `__GFP_NORETRY` / `__GFP_RETRY_MAYFAIL` / `__GFP_NOFAIL` | (modifiers) | | give up early / try hard but may fail / never fail (avoid) |
| `GFP_DMA` / `GFP_DMA32` | (zone modifiers) | legacy | low physical memory for limited devices; use the DMA API instead ([08](../08-dma-and-mmap/)) |

**Rule:** if the code may not sleep (see [03 section 3.1](../03-kernel-synchronization/README.md#31-sources-of-concurrency-and-the-contexts-you-run-in)),
use `GFP_ATOMIC` or `GFP_NOWAIT`. Better still, allocate earlier in
process context and keep the memory ready (a mempool or a preallocated
ring). `GFP_KERNEL` under a spinlock is the classic "BUG: sleeping
function called from invalid context" (with `CONFIG_DEBUG_ATOMIC_SLEEP`).

**PREEMPT_RT (the board):** `spinlock_t` sleeps there, so `GFP_ATOMIC`
under a `spinlock_t` is fine. But you **may not allocate at all**, not
even with `GFP_ATOMIC`, while holding a `raw_spinlock_t`, with interrupts
disabled, or in a real hard-IRQ handler: the allocator itself takes
sleeping locks on RT. Preallocate for those paths.

### 3.6 mempool: allocation that cannot fail  (`kmem_demo` test 5)

```
 mempool_alloc(pool, GFP_KERNEL)
   1. try the normal allocator (kmalloc / kmem_cache_alloc / alloc_pages)
   2. failed? take one of min_nr pre-allocated elements from the reserve
   3. reserve empty? sleep until somebody mempool_free()s an element
 mempool_free(): refill the reserve first, then free normally
```

Used where an allocation is needed *to free memory*: the block layer
needs a `bio` to write dirty pages out, and if allocating that `bio`
failed under memory pressure the system would deadlock. The guarantee holds
because elements always come back. With `GFP_ATOMIC` mempool can still
return NULL (step 3 can't sleep). Helpers:
`mempool_create_kmalloc_pool(min, size)`, `mempool_create_slab_pool(min, cache)`,
`mempool_create_page_pool(min, order)`.

### 3.7 Per-CPU memory  (`kmem_demo` test 6)

`alloc_percpu(type)` returns a `__percpu` pointer: one zeroed copy of
`type` per possible CPU. Access your CPU's copy with `this_cpu_inc(p->x)`,
`this_cpu_ptr(p)` (preemption must be off) or `get_cpu_ptr(p)`/`put_cpu_ptr()`.
Read any CPU's copy with `per_cpu_ptr(p, cpu)`. Free with `free_percpu()`.
There's no sharing, so no lock and no cache-line bouncing: network and
block statistics, SLUB's own free lists, and the scheduler's runqueues
are per-CPU. Static version: `DEFINE_PER_CPU` (see
[03 section 3.2](../03-kernel-synchronization/README.md#32-atomics-bitops-and-barriers--kernelksync_spinc-tests-15)).

### 3.8 Managed resources: `devm_*` (preview)

In a real driver you rarely call `kfree` in `remove()`. Instead you
allocate in `probe()` with the device-managed variants:

```c
priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
buf  = devm_kmalloc_array(&pdev->dev, n, sizeof(*buf), GFP_KERNEL);
base = devm_platform_ioremap_resource(pdev, 0);
```

The driver core frees them, in reverse order, when `probe` fails or the
device is unbound. Error paths shrink to `return -ENOMEM;`. The catch:
lifetime is tied to the *binding*, not to open files. Memory that an open
file descriptor can still reach after `remove()` must be refcounted
instead (`kref`, below). Covered properly in
[06 device model and device tree](../06-device-model-and-device-tree/).
The modules here use plain allocations and goto-unwinding, because they
have no `struct device` and because you should see what `devm` does for you.

### 3.9 Leaks, kmemleak and KASAN  (`kmem_leak_bug.c`, `BUGS=1`)

**kmemleak** (`CONFIG_DEBUG_KMEMLEAK=y`) records every allocation. On a
scan it searches memory (data/bss, stacks, every allocated object) for
values that look like pointers into each block. Blocks nobody points to
are reported, with the stack trace of the allocation:

```sh
sudo mount -t debugfs none /sys/kernel/debug     # if not mounted already
sudo insmod kmem_leak_bug.ko leaks=5
sudo rmmod kmem_leak_bug
echo scan | sudo tee /sys/kernel/debug/kmemleak  # scan now (also runs every 10 min)
sudo cat /sys/kernel/debug/kmemleak
echo clear | sudo tee /sys/kernel/debug/kmemleak # forget what was reported so far
```

Example report (shape as printed by 6.x; addresses and offsets differ):

```
unreferenced object 0xffff000804a3d580 (size 128):
  comm "insmod", pid 1342, jiffies 4295123311
  hex dump (first 32 bytes):
    65 73 68 2d 6c 65 61 6b 2d 33 00 00 00 00 00 00  esh-leak-3......
    00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00  ................
  backtrace (crc 6b0e2c1a):
    kmemleak_alloc+0x34/0x40
    __kmalloc_cache_noprof+0x1f8/0x2f0
    leak_local_pointer+0x2c/0x80 [kmem_leak_bug]
    kmem_leak_bug_init+0x40/0xff8 [kmem_leak_bug]
    do_one_initcall+0x60/0x2a0
```

The `leak_local_pointer+0x2c [kmem_leak_bug]` line is the allocation site;
decode it to a source line as in [09](../09-kernel-debugging-and-tracing/).
kmemleak can report false positives (a pointer stored only in a form it
can't recognise, e.g. XOR-ed or in a device register) and miss leaks (a
stale value that happens to look like a pointer). `kmemleak_not_leak()`
and `kmemleak_ignore()` annotate the false positives.

**KASAN** (`CONFIG_KASAN=y`, generic or arm64 HW tag-based with MTE)
catches out-of-bounds and use-after-free *at the access*, with the
allocation and free stack traces. **KFENCE** is a low-overhead sampling
version that is fine for production kernels. `slub_debug=FZPU` adds
red zones, poisoning and owner tracking without recompiling. The full table
of debug options is in [09](../09-kernel-debugging-and-tracing/).

### 3.10 Physical vs virtual addresses, and user memory

| You have | You want | Call | Valid for |
|---|---|---|---|
| linear-map pointer (`kmalloc`, `page_address`) | physical address | `virt_to_phys(p)` / `__pa(p)` | linear map only |
| physical address of RAM | kernel pointer | `phys_to_virt(pa)` / `__va(pa)` | RAM in the linear map |
| linear-map pointer | `struct page *` | `virt_to_page(p)` | linear map only |
| `struct page *` | pointer / phys / pfn | `page_address()` / `page_to_phys()` / `page_to_pfn()` | any page (no highmem on arm64) |
| `vmalloc` pointer | `struct page *` | `vmalloc_to_page(p)` | one page at a time |
| device register physical address | pointer | `ioremap()` / `devm_ioremap_resource()` | MMIO, never RAM |
| buffer for a device | DMA (bus) address | `dma_map_single()` / `dma_alloc_coherent()` | **always** for DMA; see [08](../08-dma-and-mmap/) |

A physical address is **not** what a device needs: there may be an IOMMU
(the TDA4VM has SMMUs) or an address offset between CPU and bus, and the
cache has to be handled. That is the DMA API's job. `virt_to_phys()` is for
debugging and for a few low-level arch paths.

**User memory** is a third address space. A kernel pointer to user memory
(`char __user *buf`) may be invalid, swapped out, or point into the kernel
on purpose (an attack). Always go through `copy_to_user()`,
`copy_from_user()`, `get_user()`, `put_user()`, which may sleep (page
fault) and return the number of bytes **not** copied
([01 char devices](../01-char-devices-and-file-operations/)). How user
virtual memory, page faults and `mmap` work from the process side is in
[linux-internals/14 virtual memory](../../linux-internals/14-virtual-memory/).

### 3.11 Kernel data structures  (`kds_demo`)

#### `container_of()`: the trick under every container

Kernel containers are **intrusive**: the link node is a member of *your*
struct, not a wrapper around it. The container only ever sees nodes.
`container_of(ptr, type, member)` turns a node pointer back into your
struct by subtracting the member's offset:

```
  struct esh_proc  @ 0x1000              container_of(n, struct esh_proc, rbnode)
  +0   pid, id, comm[16]                   = (struct esh_proc *)((char *)n
  +24  kref (+4 bytes padding)                      - offsetof(struct esh_proc, rbnode))
  +32  lnode   { next, prev }              = 0x1040 - 0x40 = 0x1000
  +48  hnode   { next, pprev }
  +64  rbnode  { parent|colour, left, right }  <- n = 0x1040   (64-bit offsets)
```

(The real offsets are printed by `kds_demo`.) Consequences: one object can
be on several containers at once (here a list, a hash table and a tree)
with no extra allocations; containers are type-agnostic without
templates; and **the container never owns or frees your object**. You do,
after unlinking it from everything. `list_entry`, `rb_entry`,
`hlist_entry` are just `container_of`.

#### `list_head`: doubly linked circular list

The theory is in [dsa/06 linked lists](../../dsa/06-linked-lists/). The
kernel usage:

```c
struct esh_proc { ...; struct list_head lnode; };
static LIST_HEAD(proc_list);                       /* head points to itself = empty */
list_add_tail(&p->lnode, &proc_list);              /* O(1), queue order */
list_for_each_entry(p, &proc_list, lnode) { ... }  /* read-only walk */
list_for_each_entry_safe(p, tmp, &proc_list, lnode)  /* walk while deleting */
        if (p->pid & 1) { list_del(&p->lnode); kfree(p); }
```

`list_for_each_entry` reads `p->lnode.next` *after* the body; if the body
freed `p`, that's a use-after-free. The `_safe` variant loads the next
element into `tmp` first. (`list_del` poisons the node's pointers with
`LIST_POISON1/2`, so a later use oopses at a recognisable address like
`dead000000000100`.) Also: `list_move`, `list_splice`, `list_first_entry_or_null`,
`list_sort`, and RCU versions `list_add_rcu` / `list_for_each_entry_rcu`.

#### `hlist` and `<linux/hashtable.h>`

A hash bucket only needs a pointer to the first node, so `hlist_head` is
one pointer (half a `list_head`), which matters for tables with thousands
of buckets. `hlist_node` has `next` and `pprev`, a pointer to the previous
node's `next` field (or the head), so deletion is O(1) without knowing the
head. Theory: [dsa/08 hashing](../../dsa/08-hashing/).

```c
static DEFINE_HASHTABLE(proc_hash, 6);             /* 2^6 = 64 buckets, static */
hash_add(proc_hash, &p->hnode, p->pid);            /* key hashed with hash_min() */
hash_for_each_possible(proc_hash, p, hnode, pid)   /* walk ONE bucket */
        if (p->pid == pid) return p;               /* must compare: buckets collide */
hash_del(&p->hnode);
hash_for_each(proc_hash, bkt, p, hnode) { ... }    /* every entry */
```

The table never resizes: pick the size for the expected load. For a
growable, RCU-safe hash table use `rhashtable` (networking, netfilter).
`kds_demo` prints the bucket fill and the longest chain.

#### `rbtree`: balanced tree, you write the walk

`<linux/rbtree.h>` gives you balancing (colour fix-up, rotations), but not
search or insert: those depend on *your* key, so you write the
loop and the kernel doesn't need a function pointer per comparison. Theory:
[dsa/09 trees](../../dsa/09-trees/).

```c
struct rb_node **link = &root.rb_root.rb_node, *parent = NULL;
bool leftmost = true;
while (*link) {
        parent = *link;
        if (new->pid < rb_entry(parent, struct esh_proc, rbnode)->pid)
                link = &parent->rb_left;
        else { link = &parent->rb_right; leftmost = false; }
}
rb_link_node(&new->rbnode, parent, link);              /* attach as a red leaf */
rb_insert_color_cached(&new->rbnode, &root, leftmost); /* rebalance */
...
rb_erase_cached(&p->rbnode, &root);
for (n = rb_first_cached(&root); n; n = rb_next(n))    /* in-order = sorted */
```

`rb_root_cached` also remembers the leftmost node, so "smallest key" is
O(1). The CFS/EEVDF scheduler uses it to pick the next task, and hrtimers
to find the next expiry. Newer helpers `rb_add_cached(node, root, less)`
and `rb_find(key, root, cmp)` (5.12+) take a comparison function instead
(`kds_demo` uses the hand-written insert and `rb_find` for lookup). Users:
the scheduler, hrtimers, `vm_area_struct`s before maple trees, ext4
extents, epoll.

#### `xarray`: the radix tree's replacement (and IDR's)

An xarray is an array of pointers indexed by `unsigned long`, stored
sparsely as a radix tree ([dsa/11 tries](../../dsa/11-tries/)), with its
own spinlock and RCU-safe reads built in.

```c
static DEFINE_XARRAY_ALLOC1(proc_xa);                 /* allocating, ids start at 1 */
xa_alloc(&proc_xa, &id, p, xa_limit_32b, GFP_KERNEL); /* pick a free id, store p */
p = xa_load(&proc_xa, id);                            /* lock-free (RCU) read */
xa_set_mark(&proc_xa, id, XA_MARK_0);                 /* 3 tag bits per entry */
xa_for_each_marked(&proc_xa, idx, p, XA_MARK_0) ...
xa_erase(&proc_xa, id);  xa_store(&xa, index, p, GFP_KERNEL);  xa_destroy(&xa);
```

The page cache (`address_space->i_pages`) is an xarray of pages indexed by
file offset. Old `radix_tree_*` and `idr_*` code is being converted to it.

#### IDA and IDR: handing out IDs

`IDA` hands out integers only (lowest free first): `ida_alloc_range`,
`ida_alloc_max`, `ida_free`. Minor numbers, `/dev/i2c-N`, `/dev/videoN`,
`cpuidle` states. `IDR` (now a wrapper around xarray) maps ID to pointer;
new code uses `xa_alloc`. `kds_demo` allocates fake pids from an IDA
(random start, lowest free above it, like `alloc_pid()`).

#### `kfifo`: lock-free SPSC ring

Power-of-two size, free-running `in`/`out` counters, index = counter &
(size−1). One producer and one consumer need no lock; with more of either,
take a lock. `DEFINE_KFIFO` (static or stack), `kfifo_alloc`,
`kfifo_put/get`, `kfifo_in/out`, `kfifo_to_user/from_user`. The `/dev/ksync`
driver in [03](../03-kernel-synchronization/) uses one between `write()`,
a timer and `read()`.

#### Bitmaps

`DECLARE_BITMAP(name, bits)` or `bitmap_zalloc(bits, gfp)`.
`set_bit`/`clear_bit`/`test_and_set_bit` are atomic; the `__set_bit`
forms aren't (use them when you hold a lock or own the map).
`find_first_zero_bit`, `find_next_bit`, `for_each_set_bit`,
`bitmap_find_next_zero_area` (find a run of free slots: DMA channels,
interrupt vectors, MSI ranges), `bitmap_weight`, `bitmap_and/or/xor`,
and printing with `%*pbl` ("0-1,3,7,150"). Theory:
[dsa/18 bit manipulation](../../dsa/18-bit-manipulation/).

#### `kref` and `refcount_t`: who frees the object?

When several parties can hold a pointer (an open file, a pending I/O, a
lookup table), "who frees it" is answered by a reference count. The last
`put` frees:

```c
kref_init(&p->ref);                     /* 1: the creator / table */
kref_get(&p->ref);                      /* +1 for every new holder */
kref_put(&p->ref, esh_proc_release);    /* -1; calls release at 0 */
```

`kref` is a thin wrapper around `refcount_t`, which saturates instead of
wrapping and WARNs on increment-from-zero, so a refcount bug becomes a
leak plus a warning instead of a use-after-free. `kds_demo` keeps a
reference to a process while the table deletes it and checks the object
survives until that last put. Lookups that race with the final put need
`kref_get_unless_zero()` under the lookup lock (or RCU).

### 3.12 Which allocator do I use?

| Need | Use |
|---|---|
| a struct, a small buffer (< a few KiB) | `kzalloc` / `kmalloc` (`devm_kzalloc` in `probe`) |
| an array of n items | `kcalloc(n, size)` / `kmalloc_array` (overflow-checked) |
| thousands of identical objects, allocated and freed often | `kmem_cache_create` + `kmem_cache_alloc` |
| big table, size from user input, CPU-only | `kvmalloc` / `kvcalloc` + `kvfree` |
| really big CPU-only buffer (MiBs) | `vmalloc` / `vzalloc` |
| whole pages (to map to user space, page-sized I/O) | `alloc_pages` / `__get_free_pages` |
| anything a device DMAs to/from | `dma_alloc_coherent` / `dma_map_*` ([08](../08-dma-and-mmap/)) |
| must not fail (I/O path under memory pressure) | `mempool` |
| one counter per CPU | `alloc_percpu` / `DEFINE_PER_CPU` |
| in IRQ / softirq / under a spinlock | `kmalloc(GFP_ATOMIC)`, or better: preallocate |

---

## 4. Complexity table

| Operation | Time | Space / notes | Why |
|---|---|---|---|
| `kmalloc` / `kfree` (slab hit) | O(1) | rounding waste up to ~50% for unlucky sizes | per-CPU free list pop/push |
| `kmem_cache_alloc` / `free` | O(1) | exact size + alignment | same, own cache |
| `alloc_pages(order)` / free | O(MAX_PAGE_ORDER) ≈ O(1) | 2^order pages | split / merge buddies, ≤ 11 levels |
| `vmalloc(n)` | O(n/PAGE_SIZE) | n rounded to pages + guard page | allocate each page + build page tables |
| `vfree` | O(n/PAGE_SIZE), lazy TLB flush | | unmap + free each page |
| `mempool_alloc` | O(1) | `min_nr` elements reserved forever | allocator call or reserve pop |
| `this_cpu_inc` | O(1) | one copy × nr_cpu_ids | one instruction on x86, few on arm64, no lock |
| `list_add` / `list_del` | O(1) | 2 pointers per node | pointer rewiring |
| list search | O(n) | | linear walk |
| `hash_add` / `hash_del` | O(1) | 1 pointer per bucket, 2 per node | hlist head insert; pprev for delete |
| hash lookup | O(1) average, O(n) worst | | one bucket walk; fixed size, no resize |
| rbtree insert / erase / search | O(log n) | 3 words per node | height ≤ 2 log2(n+1) |
| `rb_first_cached` | O(1) | +1 pointer in the root | cached leftmost |
| `xa_load` / `xa_store` | O(log_64 n) | ~1 node per 64 slots | 64-way radix tree, RCU reads |
| `ida_alloc` | O(log n) | ~1 bit per ID | xarray of bitmaps |
| `kfifo_put` / `get` | O(1) | power-of-two buffer | masked counters |
| `find_first_zero_bit` | O(bits / BITS_PER_LONG) | 1 bit per item | word-at-a-time scan |
| `kref_get` / `put` | O(1) | 4 bytes | atomic inc/dec |

---

## 5. C vs C++, and kernel vs user space

The kernel is C (plus Rust for some new drivers), with no STL. The idioms map
like this:

| User space (C / C++) | Kernel |
|---|---|
| `malloc` / `calloc` / `realloc` / `free` | `kmalloc` / `kcalloc` / `krealloc` / `kfree` (+ GFP flag!) |
| `new T` many times / object pool | `kmem_cache_alloc(cache, GFP_KERNEL)` |
| `mmap(MAP_ANONYMOUS)` of a big region | `vmalloc` / `kvmalloc` |
| `posix_memalign(4096)` | `alloc_pages` / `__get_free_pages` |
| `thread_local` / per-thread counters | `alloc_percpu` / `DEFINE_PER_CPU` (per *CPU*, not per thread) |
| `std::list<T>` (owning, node wraps T) | `struct list_head` embedded in T (intrusive, non-owning) |
| `boost::intrusive::list` | the same idea as `list_head` |
| `std::unordered_map<K, T*>` | `DEFINE_HASHTABLE` + `hlist_node` in T, or `rhashtable` |
| `std::map<K, T>` / `std::set` | `rb_root(_cached)` + your own compare loop |
| `std::vector<T*>` indexed by id / sparse array | `xarray` |
| id allocator (free list of ints) | `IDA` |
| `std::bitset` / `std::vector<bool>` | `DECLARE_BITMAP` + bitops |
| lock-free SPSC ring (`boost::lockfree::spsc_queue`) | `kfifo` |
| `std::shared_ptr` / intrusive refcount | `kref` / `refcount_t` (manual get/put) |
| RAII / `unique_ptr` | goto-unwind ladders, `devm_*`, `__free(kfree)` from `<linux/cleanup.h>` (6.5+) |
| Valgrind memcheck / LeakSanitizer | KASAN / kmemleak ([dsa/03 debugging](../../dsa/03-debugging/) has the user-space side) |

Rust in the kernel wraps these: `KBox`/`KVec` with explicit allocation
flags, `Arc` on top of `refcount_t`, and intrusive lists with typed
`container_of`.

---

## 6. Where it is used in the real world

- **Slab caches everywhere:** `cat /sys/kernel/slab/*/object_size` lists
  hundreds: `task_struct`, `inode_cache`, `dentry`, `skbuff_head_cache`,
  `kmalloc-*`. Networking allocates `sk_buff`s from caches in softirq
  context with `GFP_ATOMIC`.
- **TDA4VM drivers:** the TI CSI-2 / VPAC / display drivers allocate frame
  buffers through the DMA API, backed by CMA because a 1080p NV12 frame is
  3 MiB of physically contiguous memory (or scattered + SMMU). Their
  per-device state is `devm_kzalloc`'d in `probe`.
- **mempool:** the block layer (`bio_set`), device-mapper, SCSI and NVMe
  command allocation, writeback.
- **vmalloc:** module loading (code and data), BPF programs and maps,
  firmware images (`request_firmware` buffers), big hash tables at boot,
  kernel stacks (`CONFIG_VMAP_STACK`, which also gives guard pages).
- **per-CPU:** SNMP/network statistics, `vm_event` counters behind
  `/proc/vmstat`, SLUB free lists, RCU's per-CPU data.
- **list_head:** the run lists, the device model (`klist`), every driver's
  "list of pending requests"; **hlist** for the pid hash and the
  inode/dentry hash; **rbtree** for the scheduler's run queue, hrtimers,
  ext4 extents, epoll; **xarray** for the page cache, `struct file` ids
  in io_uring, DRM GEM handles; **IDA** for minors and device indices;
  **kfifo** in serial/TTY, input, USB gadgets; **bitmaps** for CPU masks
  (`cpumask_t`), IRQ allocation and GPIO line requests; **kref** in
  `struct device`, `struct kobject`, DRM objects, V4L2 buffers.

---

## 7. Common mistakes and pitfalls

1. **`GFP_KERNEL` in atomic context** (IRQ handler, timer, under a
   spinlock). Works in testing, sleeps under memory pressure, then
   "scheduling while atomic". Enable `CONFIG_DEBUG_ATOMIC_SLEEP`.
2. **Allocating under a `raw_spinlock_t` / with IRQs off on PREEMPT_RT**,
   even with `GFP_ATOMIC`. Not allowed on the board's kernel. Preallocate.
3. **Not checking for NULL.** Every allocation can fail, `GFP_ATOMIC` and
   high orders especially. (`__GFP_NOFAIL` exists, but you should avoid it.)
4. **`virt_to_phys()` on vmalloc, stack or module memory**, or handing a
   `kmalloc` pointer's physical address straight to a device. Use the DMA
   API. (Stacks are vmalloc'd with `CONFIG_VMAP_STACK`, so a DMA buffer on
   the stack is doubly wrong.)
5. **Freeing with the wrong function or order**: `kfree` on `vmalloc`
   memory, `free_pages(addr, 0)` for an order-2 block, `kfree` on a
   `kmem_cache` object (works with SLUB today, but it's wrong and hides
   bugs), `vfree` in atomic context.
6. **`p = krealloc(p, …)`** leaks the old buffer when it fails.
7. **`kmalloc(n * size)` overflow** instead of `kcalloc`/`array_size()`.
8. **Using the `ksize()` slack.** It's out of bounds as far as KASAN is
   concerned. Use `kmalloc_size_roundup()` before allocating.
9. **Destroying a `kmem_cache` with live objects**: "Objects remaining in
   esh_obj on __kmem_cache_shutdown()" plus a leak. Free every object
   first; with `SLAB_TYPESAFE_BY_RCU`, `rcu_barrier()` first.
10. **Assuming the ctor runs on every alloc.** It runs once per slab
    object lifetime; return objects in the constructed state.
11. **`list_for_each_entry` while deleting.** Use `_safe`, or restart.
    Don't keep using a node after `list_del` (poisoned pointers).
12. **Forgetting the key compare in a hash lookup**: a bucket holds every
    key that hashes there.
13. **Changing an rbtree node's key in place.** The tree is now unsorted.
    Erase, change, re-insert.
14. **Freeing while someone holds a reference**, or `kref_get` racing
    with the final `kref_put` without a lock (`kref_get_unless_zero`).
15. **Leaks on error paths**: unwind in reverse order with a goto ladder
    (`kmem_leak_bug.c` frees the widget but not its name), or use `devm_*`.
16. **Big allocations late in uptime.** High-order `kmalloc`/`alloc_pages`
    fail after fragmentation. Allocate at probe time, use `kvmalloc`, or CMA.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile; `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `BUGS=1`, `W=1`, `clean` | `make drivers` |
| [kernel/kmem_demo.c](kernel/kmem_demo.c) | where kmalloc/vmalloc/module memory lives (`show_layout=1` prints the map); kmalloc size classes via `ksize` and `kmalloc_size_roundup`, `kmalloc(0)`, `kzalloc`, `kcalloc` overflow, `krealloc`; `kmem_cache` with constructor + `SLAB_HWCACHE_ALIGN` (counts ctor calls, checks alignment); 4 MiB `vzalloc` counting physically contiguous runs; `kvmalloc` small/big; `alloc_pages(order)` contiguity + `virt_to_page` round trip, `__get_free_pages(__GFP_ZERO)`; mempool reserve; `alloc_percpu` with `on_each_cpu`; `GFP_ATOMIC`/`GFP_NOWAIT` under a spinlock and in a timer | `insmod kmem_demo.ko [nobj=1000 vmalloc_mb=4 order=3 show_layout=1]` |
| [kernel/kds_demo.c](kernel/kds_demo.c) | `container_of`; a fake process table: pids from an IDA, each object in a `list_head`, a `DEFINE_HASHTABLE`, an `rb_root_cached` (hand-written insert, `rb_find`) and an allocating xarray with marks; lookups through every structure; delete odd pids with `list_for_each_entry_safe`; `kref` keeps a deleted object alive; teardown checks all empty; IDA lowest-hole reuse; bitmap API with `%*pbl`; `kfifo` | `insmod kds_demo.ko [nproc=500]` |
| [kernel/kmem_leak_bug.c](kernel/kmem_leak_bug.c) | **intentionally broken** (only with `BUGS=1`): leaks kmalloc blocks via a lost local pointer, a kstrdup on a bad error path, and a vmalloc buffer, for kmemleak | `make -C …/kernel BUGS=1`, `insmod kmem_leak_bug.ko leaks=5` |

There are no user-space programs in this lesson: everything is tested inside
the kernel at load time.

**Build** (from the repo root):

```sh
make drivers                                                                   # host
make -C linux-device-drivers/02-kernel-memory-and-data-structures/kernel W=1      # extra warnings
make -C linux-device-drivers/02-kernel-memory-and-data-structures/kernel BUGS=1   # + kmem_leak_bug.ko
BB=<path to beaglebone-ai64>/sources/arm64-multiplatform                       # board, see ../README.md#building
make -C linux-device-drivers/02-kernel-memory-and-data-structures/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
make drivers-clean
```

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1`, checkpatch `--strict` clean apart
from the file-name-in-header note that this repo's conventions require),
and cross-built with `W=1` against the board's `6.18.38-arm64-k3` PREEMPT_RT
tree with its own GCC 15 (vermagic `6.18.38-arm64-k3-r45 SMP preempt_rt
mod_unload aarch64`). Loading them is **your** step, on the board or in a VM;
see [../README.md](../README.md#safety).

**Run** (board or VM):

```sh
scp kernel/*.ko debian@beaglebone.local:      # or copy over NFS
sudo insmod kmem_demo.ko && sudo rmmod kmem_demo
sudo insmod kds_demo.ko nproc=2000 && sudo rmmod kds_demo
sudo dmesg | grep -E 'kmem_demo|kds_demo'
```

Expected output (example; sizes depend on the kernel config, counts and
timings vary from run to run):

```
kmem_demo: start: PAGE_SIZE 4096, nobj 1000, vmalloc 4 MiB, order 3
kmem_demo: PASS layout       kmalloc -> linear map, vmalloc -> vmalloc area, module .data -> modules area (42)
kmem_demo: INFO kmalloc(    1) -> ksize     8  (roundup     8)
kmem_demo: INFO kmalloc(   13) -> ksize    16  (roundup    16)
kmem_demo: INFO kmalloc(   65) -> ksize    96  (roundup    96)
   ... one line per size ...
kmem_demo: INFO kmalloc( 8193) -> ksize 16384  (roundup 16384)
kmem_demo: INFO kmalloc(20000) -> ksize 32768  (roundup 32768)
kmem_demo: PASS kmalloc      17 sizes, asked 37737 bytes, slack 28599 bytes (internal fragmentation)
kmem_demo: PASS kmalloc(0)   returns ZERO_SIZE_PTR, kfree() accepts it
kmem_demo: PASS kzalloc      256 bytes all zero
kmem_demo: PASS kcalloc      n*size overflow detected -> NULL instead of a short buffer
kmem_demo: PASS krealloc     64 B -> 16 KiB, first 16 words preserved, ksize now 16384
kmem_demo: PASS kmem_cache   1000 objs of 128 B (kmem_cache_size 128, line 64 B), ctor ran 1024 times, aligned, magic ok
kmem_demo: PASS vmalloc      4 MiB = 1024 pages, zeroed, pattern ok, 37 physically contiguous run(s)
kmem_demo: PASS kvmalloc     512 B -> kmalloc, 4 MiB -> vmalloc
kmem_demo: PASS alloc_pages  order 3 = 8 pages = 32 KiB physically contiguous, virt_to_page() round-trips
kmem_demo: PASS get_free_pg  __get_free_pages(GFP_KERNEL | __GFP_ZERO, 1): 2 zeroed pages
kmem_demo: PASS mempool      reserve 4, 8 allocs ok, reserve refilled to 4 on free
kmem_demo: PASS alloc_percpu on_each_cpu() bumped 2 private counters on 2 CPUs, each CPU wrote its own copy
kmem_demo: PASS gfp_context  GFP_ATOMIC under spinlock ok; in timer cb (in_task=0) GFP_ATOMIC + GFP_NOWAIT ok
kmem_demo: done: all checks passed, everything freed

kds_demo: start: 2000 fake processes
kds_demo: PASS container_of  node at +64 / +32 -> back to the struct (pid 4242)
kds_demo: PASS lookup        2000 procs: found by hash 2000, rbtree 2000, xarray 2000
kds_demo: INFO hashtable    64 buckets, 64 used, longest chain 47 (average 31)
kds_demo: PASS rbtree        in-order walk strictly increasing, leftmost (cached) pid 311 is the minimum
kds_demo: PASS delete-safe   removed 1003 odd pids during list_for_each_entry_safe, 997 left in list/hash/tree/xarray (332 marked)
kds_demo: PASS kref          pid 18717 unlinked but alive while referenced, freed by the user's last put
kds_demo: PASS teardown      2000 objects released, hash/rbtree/xarray/IDA/list all empty
kds_demo: PASS ida           ids 0..7 in order, freed 3 and 5, next alloc reused 3, empty after freeing
kds_demo: INFO bitmap      a = 0-1,3,7,150, a & [0..99] = 0-1,3,7
kds_demo: PASS bitmap        first zero 2, 3-bit free area at 4, weight 5, after AND 4
kds_demo: PASS kfifo         16 ints in, put on full refused, out in FIFO order (sum 120)
kds_demo: done: all checks passed, everything freed
```

Any `FAIL` line, `done: N check(s) FAILED`, or a kernel `BUG:`/`WARNING:`
splat means something is wrong. Init returns 0 even after a FAIL so you can
`rmmod`. Look at the caches while `kmem_demo` is loaded with a large
`nobj` (the cache only exists during init, so put a `msleep(10000)` before
`kmem_cache_destroy` as an experiment): `sudo grep esh_obj /proc/slabinfo`.

kmemleak practice (needs a kernel with `CONFIG_DEBUG_KMEMLEAK=y`, e.g. a
QEMU arm64 VM; the board's production kernel doesn't have it):

```sh
sudo insmod kmem_leak_bug.ko leaks=5     # dmesg: "leaked 5 x 128 B kmalloc + ..."
sudo rmmod kmem_leak_bug
echo scan | sudo tee /sys/kernel/debug/kmemleak
sudo cat /sys/kernel/debug/kmemleak      # 5 x size 128 "esh-leak-N", 1 x size 11
                                         # "esh-widget", 1 x size 65536 (vmalloc)
```

---

## 9. Exercises

**Easy**
1. Load `kmem_demo` on the board and on an x86 VM. Compare the `ksize`
   column. Do the small classes start at 8 or at 128 on the board? Why
   (hint: `dma_get_cache_alignment()`, `CONFIG_DMA_BOUNCE_UNALIGNED_KMALLOC`)?
2. Watch `/proc/buddyinfo` while loading `kmem_demo order=10 vmalloc_mb=64`.
   Which free lists shrink? Run it again after a big `find /` (page cache
   fills RAM). Does order 10 still succeed?
3. Change `nproc` for `kds_demo` from 100 to 10000. How do the longest
   hash chain and the average change? At what load would you double the
   table?

**Medium**
4. Add a `msleep(20000)` before `kmem_cache_destroy()` in `test_kmem_cache`,
   then read `/sys/kernel/slab/esh_obj/{object_size,slab_size,objs_per_slab,order}`
   while it waits. Remove `SLAB_HWCACHE_ALIGN` and compare.
5. Make `kmem_demo` allocate with `GFP_KERNEL` inside `gfp_timer_fn()`. On
   a kernel with `CONFIG_DEBUG_ATOMIC_SLEEP=y`, load it and read the splat.
   What does it print on the PREEMPT_RT board, and why?
6. Replace the hand-written `tree_insert()` with `rb_add_cached()` and a
   `less()` callback. Then add a second rbtree ordered by `comm` to the
   same struct. How many extra bytes per object?
7. Protect the process table with a mutex and add a debugfs file that
   prints it ([05](../05-sysfs-procfs-debugfs/) shows how); then convert
   lookups to RCU (`hash_add_rcu`, `hlist_for_each_entry_rcu`, `kfree_rcu`).

**Hard**
8. Build a QEMU arm64 kernel with `CONFIG_KASAN=y`. Add a one-byte overflow
   past `ksize()` in `test_kmalloc` and read the KASAN report: which shadow
   bytes are shown, and why is it reported even though the slab slot is
   big enough?
9. Write a `kmem_cache`-backed request pool plus a `mempool_create_slab_pool`
   on top of it. Simulate allocation failure with fault injection
   (`CONFIG_FAILSLAB`, `/sys/kernel/debug/failslab`) and show the mempool
   still delivers.
10. Replace the hash table with an `rhashtable` and run lookups from 4
    kthreads while a fifth inserts and deletes (use the runner from
    [03 ksync_spin.c](../03-kernel-synchronization/kernel/ksync_spin.c)).

---

## 10. Remember forever

```
kmalloc: small, phys-contiguous, size classes (ksize); kzalloc/kcalloc/krealloc; kfree(NULL) ok
kmem_cache_create(name,size,align,SLAB_HWCACHE_ALIGN,ctor): many same objects; ctor once per slab
vmalloc: virtually contiguous only, may sleep, never DMA, never virt_to_phys; kvmalloc+kvfree
alloc_pages(gfp, order) = 2^order contiguous pages (buddy); free with the SAME order
GFP_KERNEL may sleep; GFP_ATOMIC/GFP_NOWAIT never (IRQ, softirq, spinlock); RT: no alloc under raw lock
mempool = reserve that makes alloc never fail; alloc_percpu + this_cpu_*; devm_* frees on unbind
virt_to_phys only for linear map; DMA -> DMA API; user memory -> copy_to/from_user
container_of(ptr, type, member): node -> struct. Containers never own your object.
list_for_each_entry_safe to delete; hash_for_each_possible + compare key; rbtree: you write the walk
xarray (xa_alloc) replaces radix tree + IDR; IDA for ints; kfifo SPSC; bitmap ops; kref: last put frees
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Draw the arm64 kernel virtual memory map. Where do `kmalloc`, `vmalloc`,
   `ioremap` and module code end up?
2. Why does `virt_to_phys()` work for a `kmalloc` pointer and not for a
   `vmalloc` pointer? What do you use instead in each case?
3. Explain the buddy allocator: how is an order-2 request served from an
   order-5 block, and how are blocks merged on free? What is "external
   fragmentation"?
4. What do `/proc/buddyinfo` columns mean? Why can a system with 1 GiB free
   fail an order-6 allocation?
5. `kmalloc(100)`: how many bytes do you get, from which cache, and why may
   you not use the extra bytes?
6. When do you create your own `kmem_cache`? When does its constructor run?
7. Give three reasons not to use `vmalloc` for a 64 KiB buffer.
8. Fill in: in a hard-IRQ handler use `GFP_____`; in `probe()` use
   `GFP_____`. What changes on PREEMPT_RT?
9. What problem does a mempool solve, and why doesn't it deadlock?
10. What is the difference between `alloc_percpu` memory and a global array
    indexed by `smp_processor_id()`?
11. What does `devm_kzalloc` buy you and when is it the wrong lifetime?
12. How does kmemleak decide that a block leaked? Give one false positive
    and one false negative.
13. Explain `container_of` with a drawing. Why are kernel lists "intrusive",
    and what are the advantages?
14. Why must you use `list_for_each_entry_safe` when deleting? What is in
    a deleted node's `next` pointer afterwards?
15. Why is `hlist_head` one pointer, and what is `pprev` for?
16. The kernel's rbtree has no `rb_insert(root, key)`. Why? What does
    `rb_root_cached` add?
17. xarray vs IDR vs IDA vs hashtable: which one for (a) minor numbers,
    (b) id-to-pointer for user handles, (c) lookup by a 64-bit key that is
    sparse and random?
18. Why does `kref_put` take a release function, and when do you need
    `kref_get_unless_zero`?
