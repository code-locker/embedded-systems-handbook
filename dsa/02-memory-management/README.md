# 02 — Memory management: from the process map to malloc's chunks to RAII

> Where does every byte of your program live, who hands it out, who takes
> it back — and what exactly happens inside `malloc`? After this lesson the
> answers are pictures you can draw, not guesses.

---

## 1. Why this exists

A C program has three ways to get memory, with very different rules:

| Kind | How you get it | Lifetime | Who frees |
|---|---|---|---|
| **static** (`.data`, `.bss`, `.rodata`) | globals, `static` locals | whole program | nobody (the OS at exit) |
| **automatic** (stack) | local variables | until the function returns | automatic, instantly |
| **dynamic** (heap / mmap) | `malloc`, `new`, `mmap` | until *you* free it | **you** — exactly once |

Every memory bug is a violation of one of those rules: using stack memory
after its function returned, freeing heap memory twice or never, writing past
the end of a block. Embedded systems add more: limited RAM, no swap,
real-time deadlines that forbid an unpredictable `malloc`, DMA buffers with
alignment rules. So you need to know the *layout*, the *allocator internals*,
the *alternative allocators* (arenas, pools), and the C++ tools (RAII, smart
pointers) that make ownership automatic.

---

## 2. Mental model

**Analogy — a hotel.** The *virtual address space* is the hotel's room
numbering: billions of numbers, mostly unbuilt. The kernel builds a floor (a
VMA) when asked, but only puts furniture (a physical page) in a room the first
time someone walks in (demand paging). The **stack** is the lift lobby: guests
(function calls) stack up and leave in strict reverse order. The **heap** is
the reception desk (malloc) renting rooms of any size, keeping a ledger of
free rooms, and occasionally asking the kernel to build another floor
(`brk`/`mmap`).

```
 high addresses  0x7fff_ffff_ffff
 ┌────────────────────────────────────┐
 │ kernel space (not accessible)      │
 ├────────────────────────────────────┤ 0x0000_7fff_ffff_f000
 │ [stack]  main thread               │  grows DOWN ▼   limit: ulimit -s (8 MiB)
 │   argv, envp, auxv at the very top │
 │ ── guard gap (1 MiB) ──            │
 │            ▼                       │
 │                                    │
 │ mmap area: shared libs (libc.so),  │  grows DOWN ▼ (top-down allocation)
 │   anonymous mmaps, big mallocs,    │
 │   thread stacks, [vdso] [vvar]     │
 │                                    │
 │            ▲                       │
 │ [heap]  (brk)                      │  grows UP ▲   sbrk()/brk()
 ├────────────────────────────────────┤ ◄── random gap (ASLR)
 │ .bss   zero-initialised globals rw-│
 │ .data  initialised globals      rw-│
 │ .rodata constants, literals     r--│
 │ .text  machine code             r-x│
 │ ELF headers                     r--│
 ├────────────────────────────────────┤ 0x5555_5555_4000-ish (PIE, randomised)
 │ unmapped: page 0 never mapped →    │
 │ NULL dereference = SIGSEGV         │
 └────────────────────────────────────┘
 low addresses   0x0
```

On arm64 Linux (the TDA4VM's A72 cores) the picture is the same; only the
exact numbers differ (48-bit VA by default, user space below `0x0000_ffff_ffff_ffff`).

---

## 3. How it works

### 3.1 Process memory layout

`c/01_process_layout.c` takes the address of one object of each kind and
finds it in `/proc/self/maps`. Real output (built with `SAN=0`):

```
  object                                address  perm  region
  main()          (.text)        0x5fad9bfbd889  r-xp  pl
  string literal  (.rodata)      0x5fad9bfbe104  r--p  pl
  g_rodata        (.rodata)      0x5fad9bfbe008  r--p  pl
  g_data          (.data)        0x5fad9bfc0010  rw-p  pl
  g_bss           (.bss)         0x5fad9bfc0018  rw-p  pl
  malloc(4)       (heap)         0x5fadd41462a0  rw-p  [heap]
  mmap(1 MiB)     (anon)         0x7c896e4d9000  rw-p  (anonymous)
  strlen()        (libc)         0x7c896e38b7c0  r-xp  libc.so.6
  local variable  (stack)        0x7fff7cf092f0  rw-p  [stack]
  argv[0] string  (stack)        0x7fff7cf0a67e  rw-p  [stack]
```

Read the **permissions**: code is `r-x` (never writable — W^X), constants
`r--` (writing a string literal = SIGSEGV), variables `rw-`, the stack is
**not executable** (NX). The ELF file is mapped several times with different
permissions, one mapping per *segment*.

> **ASan note.** With the default `make` (ASan on), `malloc` is *ASan's*
> allocator (big anonymous regions like `0x5020...`, not `[heap]`), libc
> functions like `strlen` resolve to ASan interceptors, and address-taken
> locals may live on a "fake stack". To see the classic picture:
> `make SAN=0 run P=dsa/02-memory-management/c/01_process_layout.c` (the Makefile notices the
> flag change and rebuilds; the next normal `make` rebuilds with ASan again).

### 3.2 Stack frames and the calling convention

Every call creates a **frame**; every return destroys it — in strict LIFO order,
which is why stack allocation is just "subtract from the stack pointer" (O(1),
nothing to free).

```
          higher addresses
     ┌─────────────────────────┐
     │ main's locals           │
     │ return addr into libc   │
     │ saved fp (libc's)       │◄── main's frame pointer (rbp / x29)
     ├─────────────────────────┤
     │ args 7+ (if any)        │
     │ return addr into main   │  pushed by `call` (x86) / saved LR x30 (arm64)
     │ saved fp (main's)  ─────┼──► points up to main's frame record
     │ callee-saved registers  │◄── f's frame pointer
     │ f's locals              │
     │ (red zone / padding)    │◄── stack pointer (rsp / sp), 16-byte aligned
     └─────────────────────────┘
          lower addresses  ▼ next call grows down here
```

**What happens on a call** (x86_64 System V ABI; arm64 AAPCS64 in brackets):

1. Caller puts the first 6 integer/pointer args in `rdi rsi rdx rcx r8 r9`
   [first 8 in `x0–x7`], floats in `xmm0–7` [`v0–v7`], the rest on the stack.
2. `call f` pushes the return address and jumps [`bl f` puts it in the link
   register `x30`].
3. The prologue saves the old frame pointer and sets up a new one
   (`push rbp; mov rbp, rsp` [`stp x29, x30, [sp, #-16]!; mov x29, sp`]),
   then `sub rsp, N` for locals.
4. Return value goes in `rax` [`x0`]; the epilogue restores `rsp`/`rbp`;
   `ret` pops the return address [`ret` jumps to `x30`].

`c/02_stack_frames.c` shows frames moving **down** across nested calls, walks
the frame-record chain by hand (`fp[0]` = caller's fp, `fp[1]` = return
address — what `gdb bt` and `perf` do), and grows the `[stack]` VMA from
136 KiB to 1.6 MiB by recursing with 64 KiB locals.

**Limits and the guard page.** The main thread's stack may grow up to
`RLIMIT_STACK` (`ulimit -s`, typically 8192 KiB). Below the stack the kernel
keeps a guard gap (`stack_guard_gap`, 256 pages); touching it → SIGSEGV
instead of silently overwriting the mmap area. Thread stacks (pthread) are
fixed-size mmaps (default = `ulimit -s`) with a one-page `PROT_NONE` guard.
Kernel stacks are tiny (16 KiB on arm64) — never put big arrays on them.

**Why returning the address of a local is a bug:** the frame is not erased on
return, just *released*; the next call reuses the same addresses:

```c
int *bad(void) { int x = 5; return &x; }  // gcc: "function returns address of local variable"
int *p = bad();      // p points into a dead frame
printf("%d", *p);    // printf's own frame now overwrites that memory → garbage / crash
```

### 3.3 `malloc`, `calloc`, `realloc`, `free`

| Call | Contract | Gotchas |
|---|---|---|
| `malloc(n)` | `n` **uninitialised** bytes aligned for any type (16 B), or `NULL` | `malloc(0)` = NULL or a unique pointer; must still `free` it |
| `calloc(n, sz)` | `n*sz` **zeroed** bytes; **checks `n*sz` overflow** | large callocs are cheap: fresh mmap pages are already zero |
| `realloc(p, n)` | resize; contents kept up to `min(old, n)`; **may move** | on failure returns NULL and **p is still valid** → never `p = realloc(p, n)` |
| `free(p)` | return the block | `free(NULL)` OK; double free / foreign pointer = UB |
| `aligned_alloc(a, n)` / `posix_memalign` | alignment `a` (power of 2) | for DMA, SIMD, cache lines; free with `free` |
| `malloc_usable_size(p)` | real size of the block (glibc) | ≥ requested; don't rely on the slack |

### 3.4 Inside glibc malloc (ptmalloc2)

> Run the tour without ASan: `make SAN=0 run P=dsa/02-memory-management/c/04_glibc_malloc_internals.c`.
> Everything here is **glibc-specific** and is shown for understanding only.

**Chunks.** Every block is a *chunk* with a header just **before** the
pointer you get:

```
   chunk ─► ┌──────────────────────────────┐
            │ prev_size                    │ 8 B: size of previous chunk, ONLY if it is free
            ├──────────────────────────────┤
            │ size                 │A│M│P │ 8 B: chunk size (multiple of 16) + 3 flag bits
   mem   ─► ├──────────────────────────────┤ ◄── malloc() returns chunk + 16
            │ user data                    │
            │  (when free: fd / bk list    │
            │   pointers live HERE)        │
   next  ─► ├──────────────────────────────┤
            │ next chunk's prev_size       │ ◄── usable by THIS chunk while in use
            └──────────────────────────────┘
   P = PREV_INUSE (previous chunk is allocated)   M = IS_MMAPPED   A = NON_MAIN_ARENA
```

* chunk size = `max(32, align16(request + 8))`; minimum chunk 32 bytes;
  usable size = chunk − 8 (a heap chunk borrows the next chunk's `prev_size`
  field) or chunk − 16 for mmapped chunks. Real output:

```
  malloc(0)    size field=0x21  -> chunk   32 bytes  flags[--P]  usable 24
  malloc(24)   size field=0x21  -> chunk   32 bytes  flags[--P]  usable 24
  malloc(25)   size field=0x31  -> chunk   48 bytes  flags[--P]  usable 40
  malloc(100)  size field=0x71  -> chunk  112 bytes  flags[--P]  usable 104
  malloc(1 MiB) size field=0x101002 -> chunk 1052672 bytes flags[-M-] usable 1052656
```

So `malloc(1)` costs 32 bytes, and a million `malloc(8)`s cost 32 MB.
That is why pools/arenas win for many small objects.

**Where free chunks go — the bins:**

```
 free(p) ─► size ≤ 1032 B and that tcache bin has < 7 entries?
              └─ yes ─► tcache[size]  (per-THREAD, singly linked, LIFO, no locking)
              └─ no  ─► size ≤ 128 B? ─► fastbin[size] (singly linked, LIFO, not coalesced)
                        else          ─► coalesce with free neighbours, then
                                         unsorted bin (one "recently freed" list)
 malloc(n) looks in: tcache → fastbin → smallbin (exact size, < 1024 B, 62 bins)
                     → unsorted (sorts chunks into small/large bins as it scans)
                     → largebins (ranges of sizes, sorted, best fit)
                     → split the TOP chunk (the free space at the end of the heap)
                     → grow the heap with brk, or mmap a new region
```

`c/04_glibc_malloc_internals.c` proves three of these:
* **tcache reuse** — `free(a); b = malloc(same size)` returns *the same
  address*; free `x` then `y` and the next two mallocs return `y` then `x` (LIFO).
  (Also the reason a use-after-free often "works": the memory is reused by the
  very next allocation of that size.)
* **coalescing** — two adjacent freed 2000-byte chunks merge, so a 4000-byte
  request is served at the first one's address.
* **top chunk** — new chunks are carved from the top; `sbrk(0)` moves when it runs out.

**brk vs mmap.** Requests below `M_MMAP_THRESHOLD` (128 KiB by default,
*dynamic*: it rises up to 32 MiB when you free mmapped chunks) come from the
`[heap]`, which grows with `brk`. Bigger requests get their own `mmap` (flag
`M`), and `free` gives them straight back with `munmap`.

**Why `free()` rarely returns memory to the OS.** The heap can only shrink
from the **top**. One live block near the end pins everything below it:

```
 [heap] ┌──free──┬──free──┬──free── ... ──free──┬─LIVE─┐ ◄ brk can't move down past LIVE
```

The output shows exactly that: 400 × `malloc(2000)` grows the heap by 792 KiB;
freeing all but the topmost leaves it at +792 KiB. `malloc_trim(0)` releases
the free *pages inside* the heap with `madvise(MADV_DONTNEED)` (RSS drops, the
VMA stays); freeing the top block lets the heap shrink. glibc trims the top
automatically when it exceeds `M_TRIM_THRESHOLD` (128 KiB).

**Arenas and threads.** The main thread uses the *main arena* (the brk heap).
Other threads that contend get extra arenas (each a 64 MiB-aligned mmap'd
heap; flag `A`), up to `8 × cores`. That is why a multithreaded program's
VmSize jumps by 64 MiB chunks, and why `MALLOC_ARENA_MAX=2` is a common
embedded tuning.

**Knobs and stats:** `mallopt(M_MMAP_THRESHOLD | M_TRIM_THRESHOLD | M_ARENA_MAX, v)`,
env `GLIBC_TUNABLES=glibc.malloc.tcache_count=0:glibc.malloc.mmap_threshold=65536`,
`malloc_usable_size(p)`, `mallinfo2()` (`arena` = brk bytes, `hblkhd` = mmap bytes,
`uordblks` = in use, `fordblks` = free), `malloc_stats()` (to stderr),
`malloc_info(0, stdout)` (XML).

### 3.5 The memory map in depth

`/proc/<pid>/maps`, one line per **VMA** (virtual memory area):

```
 7c896e228000-7c896e3b0000 r-xp 00028000 103:03 41028066   /usr/lib/x86_64-linux-gnu/libc.so.6
 └──── address range ────┘ │    └offset─┘ └dev┘ └inode─┘   └ backing file (or [heap], [stack], blank = anonymous)
                           └ r w x  +  p = private (copy-on-write) / s = shared
```

| Column | Meaning |
|---|---|
| range | `[start, end)` virtual addresses, page-aligned |
| perms | `r`/`w`/`x`; `p` private COW mapping, `s` shared (writes visible to others / the file) |
| offset | where in the file this mapping starts (0 for anonymous) |
| dev | major:minor of the device holding the file |
| inode | inode of the file (0 = anonymous) |
| path | file, or `[heap]`, `[stack]`, `[vdso]` (kernel code mapped for fast `clock_gettime`), `[vvar]` (data the vDSO reads), `[vsyscall]` (legacy x86), blank = anonymous mmap |

Every shared library appears 4–5 times: `r--` (ELF header, `.rodata`),
`r-x` (`.text`), `r--` (RELRO: GOT made read-only after relocation), `rw-`
(`.data`), followed by an anonymous `rw-` for its `.bss`.
`c/05_proc_maps_and_paging.c` prints the map with each line explained.

`/proc/self/smaps` adds per-VMA accounting:

| Field | Meaning |
|---|---|
| `Size` | virtual size of the VMA |
| `Rss` | resident: pages actually in RAM now |
| `Pss` | proportional: shared pages divided by the number of sharers (fair "cost") |
| `Shared_Clean/Dirty`, `Private_Clean/Dirty` | dirty private pages = memory only you hold, cannot be dropped |
| `Swap` | pages swapped out |

`/proc/self/status`: `VmSize` (total virtual), `VmRSS` (resident), `VmData`
(heap + anon private), `VmStk`, `VmExe`, `VmLib`, `VmPeak`/`VmHWM` (peaks).

**Demand paging** — real output:

```
  after mmap(1 GiB):     VmSize +1048576 kB   VmRSS     +0 kB   (address space only!)
  after touching 64 MiB: VmRSS +65536 kB   smaps Rss of the region = 65536 kB
  after madvise(DONTNEED): smaps Rss of the region = 0 kB
```

`mmap` only creates a VMA. The first write to each 4 KiB page traps into the
kernel (page fault), which allocates a zeroed physical page and fills the
page-table entry. So VmSize says "what I might use", VmRSS says "what I use".
(Under ASan, VmSize is ~20 TB: ASan *reserves* shadow memory it never touches.)
This is also why `malloc` "never fails" on Linux with overcommit: you get
address space, and the OOM killer appears only when you touch it.

**Heap growth with `sbrk`:** `sbrk(n)` moves the *program break* (end of
`[heap]`) by n bytes and returns the old end; `sbrk(0)` reads it. The program
moves it up 64 KiB, writes there, moves it back.

**ASLR**: `/proc/sys/kernel/randomize_va_space = 2` randomises stack, mmap
base, heap start and the PIE load address on every run. Run any program twice
and compare addresses; `setarch -R ./prog` (or `setarch $(uname -m) -R`) turns
it off for one run; gdb turns it off by default (`set disable-randomization`).

**Going deeper** — page tables (4 levels on x86_64/arm64 with 4 KiB pages),
the TLB, minor vs major faults, copy-on-write after `fork`, huge pages, and the
kernel side of all of this are in the Linux-internals track:
[linux-internals/03-virtual-memory](../../linux-internals/03-virtual-memory/).
What you need here: *a virtual address is translated page by page to a
physical frame; unmapped or permission-violating accesses fault; the kernel
fills pages lazily.*

### 3.6 Writing allocators: bump (arena) and free list

**Bump / arena** (`c/06_bump_allocator.c`): one buffer + one offset.

```
 base                                                 base+cap
 ┌──────┬──┬──────────────┬────┬──────────┬───────────────────┐
 │ A    │p │ B (align 8)  │pad │C (al. 64)│      free         │
 └──────┴──┴──────────────┴────┴──────────┴───────────────────┘
                                          ^ off   alloc: off = align_up(off) + n   O(1)
                                                  free one: impossible
                                                  free all: off = 0              O(1)
```

**Free list** (`c/07_freelist_allocator.c`): a header per block, first-fit
search, **split** big blocks, **coalesce** neighbours on free — a miniature
glibc.

```
 malloc(50) with a 112-byte free block:  [hdr|free 112] → [hdr|USED 64][hdr|free 16]  (split)
 free(B) with free neighbours:           [free A][hdr|B][free C] → [free A+hdr+B+hdr+C] (coalesce)
```

### 3.7 Fragmentation

* **Internal**: wasted space *inside* blocks (rounding 25 → 48 bytes, headers,
  alignment padding).
* **External**: enough free bytes in total, but no single hole big enough.
  `07_freelist_allocator.c` allocates 100 × 256 B, frees every second one:
  ~12.8 KB free in total, yet a 4 KB request must come from the tail.

Long-running embedded processes (a camera pipeline running for months) die of
external fragmentation, not leaks. Cures: fixed-size pools, arenas with a
clear lifetime, allocating at start-up only, size-class allocators.

### 3.8 Alignment and padding

Each type has an alignment (`alignof`); the compiler pads structs so every
field is aligned, plus tail padding so arrays stay aligned:

```
 struct bad { char a; double b; char c; int d; char e; };   sizeof = 32
  0   1           8                16  17   20       24  25            32
 [a][ 7 pad     ][ b (8)          ][c][3p ][ d (4)  ][e][  7 tail pad  ]

 struct good { double b; int d; char a, c, e; };            sizeof = 16
  0                8        12 13 14 15
 [ b (8)          ][ d (4) ][a][c][e][p]
```

Rules: order fields by decreasing alignment; `offsetof(T, f)` gives offsets;
`_Static_assert` the layout of anything that is ABI (register maps, file
formats); `__attribute__((packed))` removes padding for wire formats but makes
fields misaligned (use `memcpy` to read them; taking their address warns);
`alignas(64)` separates per-CPU counters onto their own cache lines (false
sharing). Padding bytes have unspecified values — `memset` before `memcmp`/hashing/sending.
`pahole` (package `dwarves`) prints the holes of every struct in a binary.

### 3.9 Memory pools and arenas — what embedded systems use

```
 fixed-size pool (c/09_memory_pool.c)
 free_list ─► [blk0|next●]─► [blk1|next●]─► [blk2|next●]─► NULL
   alloc: pop head   O(1)          free: push head   O(1)
   the 'next' pointer lives INSIDE the free block → zero overhead
```

| | malloc | arena (bump) | fixed pool |
|---|---|---|---|
| alloc | O(1) avg, unbounded worst (brk/mmap syscall, bin scan) | O(1) | O(1) |
| free | O(1) avg | all at once only | O(1) |
| fragmentation | yes | none (within a lifetime) | none |
| determinism | no | yes | yes |
| typical user | general code | per-frame / per-request scratch, compilers | RTOS objects, packets, messages, drivers |

In the Linux kernel: the **slab allocator** (`kmem_cache_create` +
`kmem_cache_alloc`) is a pool per object type; `mempool_t` guarantees a
reserve for I/O paths; `dma_pool` hands out small DMA-coherent blocks. In
RTOSes: FreeRTOS `heap_1` (bump, no free) … `heap_4` (free list with
coalescing), Zephyr `k_mem_slab`. In video: GStreamer `GstBufferPool`
recycles frame buffers instead of allocating per frame.

### 3.10 C++: `new` and `delete`

```
 new T(args)   =  void *raw = operator new(sizeof(T));   // → malloc in libstdc++
                  T *p = new (raw) T(args);              // constructor (if it throws: operator delete(raw))
 delete p      =  p->~T();  operator delete(p);          // → free
 new T[n]      =  operator new[](n*sizeof(T) + cookie) ; n constructors; cookie stores n
 delete[] p    =  read cookie, n destructors (reverse order), operator delete[]
```

* `new` **throws** `std::bad_alloc` instead of returning NULL;
  `new (std::nothrow) T` returns `nullptr`.
* For types with a non-trivial destructor, `new T[3]` asks for
  `3*sizeof(T) + 8` bytes — the 8-byte **array cookie** (measured in
  `cpp/01_new_delete.cpp`). That is why `delete` on a `new[]` pointer is UB:
  it frees the wrong address and runs one destructor.
* `operator new` is replaceable. `cpp/02_counting_operator_new.cpp` replaces it
  globally to count allocations: `std::string("cam0")` → 0 (SSO),
  1000 `push_back`s → 11 allocations (or 1 with `reserve`),
  `shared_ptr<int>(new int)` → 2, `make_shared` → 1, `std::map` → 1 per node.

### 3.11 Placement new

`new (addr) T(args)` constructs at `addr` and allocates nothing. You call
`p->~T()` yourself and never `delete`. It is how `std::vector` separates
capacity from size, how `std::optional`/`std::variant` work, and how you put
objects in SRAM, shared memory or a pool. Storage must be big enough and
aligned: `alignas(T) unsigned char buf[sizeof(T)]`. `cpp/03_placement_new.cpp`
builds a heap-free `StaticVector<T, N>`.

### 3.12 RAII

**Resource Acquisition Is Initialisation**: the constructor acquires, the
destructor releases, and C++ guarantees destructors run when a scope ends —
normal exit, `return`, `break`, or exception unwinding. So:

```cpp
{
    Fd fd(open(...));                 // acquired
    auto buf = std::make_unique<char[]>(4096);
    std::lock_guard<std::mutex> lk(m);
    if (error) return -1;             // ← all three released here, in reverse order
    may_throw();                      // ← and here
}                                     // ← and here
```

The C equivalent is the `goto out_free_buf; ... out_close_fd:` cleanup ladder
in kernel code. RAII types own exactly one resource, are usually move-only,
and are the foundation for everything below.

### 3.13 Smart pointers

```
 unique_ptr<T>      [ptr]──►[T]                      1 owner, move-only, sizeof == sizeof(T*)
 shared_ptr<T>      [ptr|ctrl]──►[strong|weak|deleter][T]   (make_shared: one allocation)
 weak_ptr<T>        [ptr|ctrl]  observes; .lock() → shared_ptr or empty; .expired()
```

* Prefer `make_unique` / `make_shared` (exception-safe; `make_shared` = one allocation).
* `unique_ptr<FILE, decltype(&fclose)>` wraps any C handle.
* Refcount updates are **atomic** → shared_ptr copies are not free; pass
  `const shared_ptr&` or a raw `T*`/`T&` when you are not taking ownership.
* **Reference cycles leak**:

```
   a ──owns──► B          after the locals a, b go away:
   ▲           │            A.strong = 1 (held by B), B.strong = 1 (held by A)
   └──owns─────┘            → neither reaches 0 → never destroyed → LEAK
```

`cpp/05_smart_pointers.cpp` creates the cycle, proves both objects are still
alive after their owners left scope, then breaks it. The fix: make
back-pointers (child → parent, observer → subject) `weak_ptr`.

### 3.14 Rule of 0 / 3 / 5

* A class that owns a raw resource needs **destructor + copy constructor +
  copy assignment** (rule of 3), otherwise the generated copy copies the
  *pointer* and two destructors free it → double free.
* Add **move constructor + move assignment** (rule of 5) to steal instead of
  copy. Mark them `noexcept`.
* Best: **rule of 0** — hold resources in members that manage themselves
  (`std::vector`, `std::string`, `std::unique_ptr`) and write none of the five.

### 3.15 Move semantics

An rvalue (temporary, or `std::move(x)`) will not be used again, so its
resources can be stolen: copy 3 pointers, not N bytes. `std::move` is only a
cast to `T&&`; the moved-from object is valid but unspecified (assign to it or
destroy it). `std::vector` moves elements on reallocation **only if the move
constructor is `noexcept`** — otherwise it copies. Real output of
`cpp/07_move_semantics.cpp`:

```
noexcept move ctor:     copies=0 moves=31
move ctor NOT noexcept: copies=31 moves=0  <- every reallocation deep-copies!
```

### 3.16 Allocators and `std::pmr`

All containers accept an allocator. `cpp/08_allocators_and_pmr.cpp` shows a
minimal custom `ArenaAllocator<T>` for `std::vector`, and C++17
`std::pmr::monotonic_buffer_resource` over a stack buffer: 100 frames × 20
strings with **zero** heap calls; overflow falls through to an upstream
resource; `unsynchronized_pool_resource` recycles size-class blocks like malloc bins.

### 3.17 The kinds of memory errors

| Error | Example | Symptom | Best tool |
|---|---|---|---|
| **Leak** | `malloc` without `free`; shared_ptr cycle | RSS grows until OOM | LeakSanitizer (in ASan), valgrind |
| **Double free** | `free(p); free(p);` | glibc: `free(): double free detected in tcache 2`, abort | ASan, valgrind |
| **Use-after-free** | `free(p); p->x = 1;` | silent corruption of whoever reuses the block | ASan (`heap-use-after-free`) |
| **Heap overflow** | `malloc(n)` then `p[n] = 0` | corrupts the next chunk header → crash *later* in malloc/free | ASan (`heap-buffer-overflow`) |
| **Stack overflow (buffer)** | `char b[8]; strcpy(b, long)` | smashes the return address; `*** stack smashing detected ***` | ASan, `-fstack-protector` |
| **Stack exhaustion** | infinite recursion | SIGSEGV on the guard page | gdb `bt`, ASan `stack-overflow` |
| **Uninitialised read** | `int x; if (x)` | random behaviour, differs -O0 vs -O2 | valgrind, MSan (clang), `-Wall -O2` |
| **Mismatched free** | `new[]`/`delete`, `malloc`/`delete` | UB, heap corruption | ASan (`alloc-dealloc-mismatch`) |
| **Invalid free** | `free(&local)`, `free(p + 1)` | `free(): invalid pointer` | ASan, valgrind |
| **Returning address of local** | `return &x;` | garbage after the next call | `-Wall`, ASan `stack-use-after-return` |

Each has a runnable `*_bug` program and the exact debugging session in
[03-debugging](../03-debugging/README.md).

---

## 4. Complexity table

| Operation | Time | Space overhead | Why |
|---|---|---|---|
| stack allocation (local) | O(1) | 0 | `sub sp, N` at function entry |
| `malloc` small (tcache/fastbin hit) | O(1) | 8–16 B header + rounding | pop from a singly linked list |
| `malloc` (bin search / split / coalesce) | O(1) amortised, O(#bins) worst | same | bounded number of bins |
| `malloc` growing heap / big block | O(1) + syscall (µs) + page faults | page granularity | `brk`/`mmap`; pages zero-filled on first touch |
| `free` | O(1) | — | flag + list insert + neighbour merge |
| `calloc(n)` | O(n) to zero (big: free, fresh mmap pages are zero) | — | |
| `realloc` | O(1) in place, O(n) if it moves | — | copy old contents |
| bump/arena alloc | O(1) | alignment padding only | add + compare |
| arena reset | O(1) | — | `off = 0` |
| pool alloc/free | O(1) | 0 (next pointer inside free block) | list pop/push |
| toy free-list malloc (first fit) | O(#blocks) | 32 B header | linear scan |
| `new`/`delete` | malloc/free + ctor/dtor | +8 B cookie for `new[]` of non-trivial T | |
| `unique_ptr` | same as raw | 0 | |
| `shared_ptr` copy | O(1) atomic inc/dec | control block (16+ B) | |
| page fault (first touch) | ~0.5–2 µs | 4 KiB page | kernel allocates + zeroes |

---

## 5. C vs C++

| Task | C | C++ |
|---|---|---|
| allocate one object | `T *p = malloc(sizeof *p);` + init | `auto p = std::make_unique<T>(args);` |
| array | `malloc(n * sizeof *p)` | `std::vector<T>(n)` |
| free | `free(p); p = NULL;` | automatic (destructor) |
| cleanup on error | `goto out_free;` ladders | RAII, exceptions unwind |
| shared ownership | manual refcount (`kref`, `g_object_ref`) | `std::shared_ptr` |
| object in existing memory | just cast + init fields | placement new + explicit dtor |
| custom allocator | your own `pool_alloc()` API | `Allocator` template / `std::pmr::memory_resource` |
| zeroed allocation | `calloc` | `T{}`, `std::vector<T>(n)` value-initialises |
| alignment | `aligned_alloc`, `_Alignas` | `alignas`, `new` honours over-aligned types (C++17) |

---

## 6. Where it is used in the real world

* **Linux kernel**: `kmalloc` (power-of-two slab caches), `kmem_cache` (slab
  per object type), `vmalloc` (virtually contiguous), `mempool`, `dma_alloc_coherent`
  / CMA for camera and display buffers on the TDA4VM, `kref` refcounting, the
  `goto` cleanup idiom, page cache and demand paging.
* **GStreamer**: `GstBufferPool` (recycling), `GstMemory` + allocators (`dmabuf`
  allocator for zero-copy between V4L2 capture, the TI VPAC/DSS and the GPU),
  refcounted `GstMiniObject`.
* **Embedded/RTOS**: static allocation at start-up only, fixed pools, arenas;
  MISRA C forbids `malloc` after initialisation.
* **Databases / servers**: arenas per query/request (PostgreSQL memory
  contexts, nginx pools), jemalloc/tcmalloc/mimalloc as malloc replacements.
* **Compilers / games**: per-function or per-frame arenas, freed in O(1).
* **C++ real-time audio/video**: `std::pmr` monotonic buffers so the hot loop never calls `malloc`.

---

## 7. Common mistakes and pitfalls

1. `p = realloc(p, n)` — leaks the block when realloc fails.
2. `malloc(n * size)` without overflow check — use `calloc` or check first.
3. Forgetting `+1` for the string terminator.
4. Returning the address of a local; keeping pointers into a `realloc`ed or grown vector.
5. Mixing allocation families (`new`/`free`, `new[]`/`delete`, `malloc`/`delete`).
6. Assuming `free` returns memory to the OS (RSS does not drop) — see §3.4.
7. Measuring memory with VmSize instead of VmRSS/Pss.
8. Big arrays on the stack (8 MiB limit on the main thread, much less on threads
   and 16 KiB in the kernel).
9. Writing a class with a raw owning pointer and no copy constructor (rule of 3).
10. Move constructor not marked `noexcept` → vector copies on growth.
11. `shared_ptr` cycles (parent ↔ child) → leak; use `weak_ptr` for back-links.
12. Packed structs + taking the address of a misaligned member.
13. Relying on glibc internals (chunk headers, tcache reuse) in real code.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_process_layout.c](c/01_process_layout.c) | address of each segment, matched against `/proc/self/maps` | `make run P=dsa/02-memory-management/c/01_process_layout.c` |
| [c/02_stack_frames.c](c/02_stack_frames.c) | frames grow down, frame-record walk, `ulimit -s`, on-demand stack growth | `make run P=dsa/02-memory-management/c/02_stack_frames.c` |
| [c/03_malloc_family.c](c/03_malloc_family.c) | malloc/calloc/realloc/free/aligned_alloc contracts, safe realloc | `make run P=dsa/02-memory-management/c/03_malloc_family.c` |
| [c/04_glibc_malloc_internals.c](c/04_glibc_malloc_internals.c) | chunk headers, size rounding, tcache, coalescing, mmap threshold, trim, mallinfo2 (full tour needs `SAN=0`) | `make run P=dsa/02-memory-management/c/04_glibc_malloc_internals.c` |
| [c/05_proc_maps_and_paging.c](c/05_proc_maps_and_paging.c) | annotated maps, smaps, status, demand paging with 1 GiB, sbrk, ASLR | `make run P=dsa/02-memory-management/c/05_proc_maps_and_paging.c` |
| [c/06_bump_allocator.c](c/06_bump_allocator.c) | arena: aligned bump allocation, marks, O(1) reset | `make run P=dsa/02-memory-management/c/06_bump_allocator.c` |
| [c/07_freelist_allocator.c](c/07_freelist_allocator.c) | toy malloc: headers, first fit, split, coalesce, fragmentation | `make run P=dsa/02-memory-management/c/07_freelist_allocator.c` |
| [c/08_alignment_padding.c](c/08_alignment_padding.c) | `alignof`, `offsetof`, padding, packed structs, cache-line alignment | `make run P=dsa/02-memory-management/c/08_alignment_padding.c` |
| [c/09_memory_pool.c](c/09_memory_pool.c) | fixed-size block pool with an embedded free list | `make run P=dsa/02-memory-management/c/09_memory_pool.c` |
| [cpp/01_new_delete.cpp](cpp/01_new_delete.cpp) | new/delete/new[]/delete[], the array cookie, nothrow, bad_alloc | `make run P=dsa/02-memory-management/cpp/01_new_delete.cpp` |
| [cpp/02_counting_operator_new.cpp](cpp/02_counting_operator_new.cpp) | replacing global operator new to count STL allocations | `make run P=dsa/02-memory-management/cpp/02_counting_operator_new.cpp` |
| [cpp/03_placement_new.cpp](cpp/03_placement_new.cpp) | placement new, explicit destructor, heap-free StaticVector | `make run P=dsa/02-memory-management/cpp/03_placement_new.cpp` |
| [cpp/04_raii.cpp](cpp/04_raii.cpp) | fd/mmap/FILE/mutex owners; cleanup on return and exceptions | `make run P=dsa/02-memory-management/cpp/04_raii.cpp` |
| [cpp/05_smart_pointers.cpp](cpp/05_smart_pointers.cpp) | unique/shared/weak, control block, aliasing, the cycle leak | `make run P=dsa/02-memory-management/cpp/05_smart_pointers.cpp` |
| [cpp/06_rule_of_0_3_5.cpp](cpp/06_rule_of_0_3_5.cpp) | rule-of-5 Buffer with copy-and-swap vs rule-of-0 | `make run P=dsa/02-memory-management/cpp/06_rule_of_0_3_5.cpp` |
| [cpp/07_move_semantics.cpp](cpp/07_move_semantics.cpp) | stealing buffers, `std::move` is a cast, noexcept and vector growth | `make run P=dsa/02-memory-management/cpp/07_move_semantics.cpp` |
| [cpp/08_allocators_and_pmr.cpp](cpp/08_allocators_and_pmr.cpp) | custom allocator for vector, `std::pmr` monotonic and pool resources | `make run P=dsa/02-memory-management/cpp/08_allocators_and_pmr.cpp` |

---

## 9. Exercises

1. **(easy)** Run `01_process_layout` twice with ASan off. Which addresses
   change? Now run it with `setarch -R`. Which stay identical?
2. **(easy)** In `03_malloc_family.c`, print `malloc_usable_size` for requests
   1..64 (SAN=0) and predict each value with the chunk formula before running.
3. **(easy)** Reorder the fields of a struct from a real driver header to
   minimise padding; check with `pahole`.
4. **(medium)** Add `arena_calloc` and an `arena_strdup` to the bump allocator.
5. **(medium)** Change `07_freelist_allocator.c` to **best fit**. Re-run the
   fragmentation demo. Then keep a separate list of free blocks only.
6. **(medium)** Make the pool in `09_memory_pool.c` thread-safe with a mutex,
   then lock-free with a CAS on the head (hint: ABA problem).
7. **(medium)** Write a program that allocates 100 MB in 1 KB blocks, frees
   all but every 100th, and prints VmRSS before/after `malloc_trim(0)`.
8. **(hard)** Implement `my_malloc`/`my_free` on top of `mmap` with size
   classes (16, 32, 64, … 2048) each served by a pool, and large blocks by
   direct mmap. Benchmark against glibc.
9. **(hard)** Write a `RingBuffer<T, N>` with placement new and full rule-of-5 semantics.
10. **(hard)** Use `LD_PRELOAD` with a shared library that wraps `malloc`/`free`
    (via `dlsym(RTLD_NEXT, ...)`) to log every allocation of an existing program.

---

## 10. Remember forever

```
Layout (low→high): text r-x | rodata r-- | data rw- | bss rw- | heap↑ (brk) ... mmap↓ | stack↓ [guard]
Stack frame = {saved fp, return addr} + locals; LIFO, O(1); dies on return → never return &local.
malloc chunk: [prev_size][size|A|M|P] user… ; user ptr = chunk+16 ; chunk = max(32, align16(n+8)).
Bins: tcache (per thread, 7 each, ≤1032B) → fast → small → unsorted → large → top → brk/mmap.
≥128 KiB → mmap (M bit), freed with munmap. Heap only shrinks from the top → free() rarely lowers RSS.
VmSize = promised address space; VmRSS = pages touched. Pages appear on first touch (demand paging).
Arena: bump + reset-all. Pool: fixed blocks, free list inside free blocks. Both O(1), no fragmentation.
new = operator new (malloc) + ctor; delete = dtor + operator delete (free); new[] ↔ delete[] (cookie).
RAII: ctor acquires, dtor releases; unique_ptr default; shared_ptr cycles leak → weak_ptr back-links.
Rule of 0 (best) / 3 / 5; moves must be noexcept or vector copies.
```

---

## 11. Self-test questions

1. Name every region of a process map from low to high address and the permissions of each.
2. Why can't you write to a string literal? Which mapping holds it?
3. What exactly happens, register by register, on a call and return on x86_64 and on arm64?
4. Why is returning the address of a local a bug, and why does it sometimes "work"?
5. What is in the 16 bytes before a pointer returned by glibc `malloc`? What do the P, M, A bits mean?
6. How big is the chunk for `malloc(1)`, `malloc(24)`, `malloc(25)`?
7. Why does `free(a); b = malloc(same)` usually return `a`? Why not under ASan?
8. When does malloc use mmap instead of brk, and why does `free()` not lower RSS?
9. What is the difference between VmSize, VmRSS and Pss? Why can a process have 1 GiB VmSize and 2 MiB RSS?
10. What is demand paging? What happens on the first write to a freshly mmap'd page?
11. Internal vs external fragmentation — one example of each and one cure each.
12. Why does `struct { char a; double b; }` have size 16? How do you shrink structs?
13. Arena vs pool vs malloc: when would you pick each on an embedded camera pipeline?
14. What does `new T[3]` allocate that `new T` does not? Why is `delete` on it UB?
15. Explain a shared_ptr cycle with a drawing and how weak_ptr fixes it.
16. State the rule of 3, 5 and 0. Why does `noexcept` on a move constructor matter?

Review schedule: see [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).
