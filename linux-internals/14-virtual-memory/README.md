# 14 — Virtual Memory

> Every pointer your program holds is a **virtual address**. The MMU turns
> it into a physical address one **page** at a time, using page tables the
> kernel keeps for each process. Because of this one layer of indirection
> Linux can give every process a private address space, allocate RAM only
> when a page is first touched, share library code between processes, copy
> memory only when someone writes to it, map files as memory, and stop a
> buffer overflow at a guard page. This lesson measures each of those
> mechanisms from user space.

---

## 1. Why this exists

Without virtual memory every program would have to know which physical RAM
is free, one stray pointer could overwrite the kernel or another process, and
a 1 GiB `malloc` would need 1 GiB of RAM right away. Virtual memory fixes all
three:

| Problem | Virtual-memory feature | Program |
|---|---|---|
| processes must not see each other's memory | a private page table per process | `c/01` |
| programs reserve more than they use | **demand paging**: RAM only on first touch | `c/02` |
| `fork()` of a 500 MiB process must be fast | **copy-on-write** | `c/02` |
| file I/O without copies, shared libraries | **file-backed `mmap`** | `c/03`, [11](../11-ipc-shared-memory/README.md) |
| catch overflows, stack overruns | **page protections**, guard pages, SIGSEGV | `c/03`, `c/06`, `cpp/02` |
| a real-time loop must never stall on a fault | `mlock`, `MAP_POPULATE` | `c/02`, `c/04` |
| "how much memory does my app really use?" | `/proc/self/smaps`: RSS vs PSS | `c/05` |
| exploits must not know addresses | **ASLR** | `c/06` |

The process layout itself (text, data, heap, stack, the `/proc/self/maps`
format, how `malloc` uses `brk` and `mmap`) is covered in
[dsa/02-memory-management](../../dsa/02-memory-management/README.md) §3.1 and §3.5.
This lesson starts from that picture and explains the machinery underneath.

---

## 2. Mental model

**Virtual memory is a hotel with room numbers and a front desk.** Your
program only knows room numbers (virtual addresses). The front desk (the
MMU) has a ledger (the page tables) that maps each room number to a real
room (a physical page frame). A room number with no entry in the ledger
makes the desk call the manager (a **page fault** into the kernel). The
manager can assign a room on the spot (demand paging), refuse (SIGSEGV), or
fetch your luggage from storage first (a major fault). Two guests can be
given the same real room for reading (shared libraries, copy-on-write)
until one of them wants to write in it.

```
  process A virtual space            page tables (per process)         physical RAM (frames)
  0x0000_5555_5555_4000 code  ----> [ PTE: frame 0x1a2b3, r-x ] ------> [ libc / code frame ]  <-- also
  0x0000_7fff_f7a0_0000 libc  ----> [ PTE: frame 0x0ffee, r-x ] ---+                                 mapped
  0x0000_7fff_f7f0_0000 heap  ----> [ PTE: frame 0x2c001, rw- ] ---|--> [ A's private frame ]           by B
  0x0000_7fff_f7f0_1000       ----> [ PTE: not present       ]    |    (no frame yet: first touch faults)
  0x0000_7fff_ffff_e000 stack ----> [ PTE: frame 0x30a00, rw- ]    |
                                                                    |
  process B virtual space                                           |
  0x0000_7fff_f790_0000 libc  ----> [ PTE: frame 0x0ffee, r-x ] ---+   same physical frame, different VA
```

Remember this sentence: **an address is only a key into the page table: no
entry means a page fault, the kernel decides what happens next, and memory
costs RAM only once it has been touched.**

---

## 3. How it works

### 3.1 Virtual and physical addresses (`c/01_pages_and_translation.c`)

- On 64-bit Linux user space gets the lower half of a 48-bit space (128 TiB):
  `0x0000_0000_0000_0000`–`0x0000_7fff_ffff_ffff`. The kernel uses the top
  half. ARM64 has a separate translation table base register for each half
  (`TTBR0_EL1` for user, `TTBR1_EL1` for the kernel). x86 has one (`CR3`)
  with the kernel half in every table.
- **A page** is the unit of mapping and protection: 4 KiB on x86-64 and on
  most arm64 kernels. arm64 can also use 16 KiB or 64 KiB pages
  (`CONFIG_ARM64_64K_PAGES`). Never hard-code 4096: use
  `sysconf(_SC_PAGESIZE)`. The BeagleBone AI-64 kernel uses 4 KiB.
- The physical address of a page is its **page frame number** (PFN) × page
  size. `/proc/self/pagemap` gives one 64-bit entry per virtual page (present
  bit, swapped bit, PFN). Since Linux 4.0/4.2 the PFN reads as 0 without
  `CAP_SYS_ADMIN`, because physical addresses help Rowhammer attacks. `c/01`
  shows the present bits change as pages are touched, and shows the hidden PFN.

### 3.2 Page tables: the 4-level walk (`c/01`)

With 4 KiB pages and 48-bit addresses (x86-64 and the arm64 default), the
address is cut into four 9-bit indices and a 12-bit offset:

```
   47      39 38      30 29      21 20      12 11           0
  +----------+----------+----------+----------+--------------+
  | L0  PGD  | L1  PUD  | L2  PMD  | L3  PTE  |   offset     |
  +----------+----------+----------+----------+--------------+
       |          |          |          |            |
       v          v          v          v            |
  TTBR0/CR3 -> [512 x 8 B] -> [512] ---> [512] ---> [512] ---> frame base + offset = physical
               one 4 KiB      each entry points to the next table;
               table          an L2 entry can instead map a 2 MiB block (huge page),
                              an L1 entry a 1 GiB block

  64 KiB granule (arm64 option): 13-bit indices, 16-bit offset, 3 levels for 48 bits
  5-level paging (x86 LA57, arm64 LPA2): 57/52-bit VA for huge servers - not on the board
```

- Each table is exactly one page: 512 entries × 8 bytes.
- Tables are created **lazily**, so a sparse 128 TiB space costs only the
  tables for the parts actually in use.
- A page-table entry (PTE) holds the frame number plus flags: present,
  writable, user-accessible, executable (NX on x86, UXN/PXN on ARM), accessed,
  dirty. Those flags are what `mprotect` changes, and what the kernel reads to
  decide on copy-on-write.
- `c/01` splits a real stack address into its L0..L3 indices and puts it back
  together.

### 3.3 The TLB (`c/01`)

A 4-level walk costs up to 4 memory reads per access, which would be
unacceptable. The **TLB** (translation lookaside buffer) caches recent
translations. The Cortex-A72 has a 48-entry L1 instruction TLB, a 32-entry L1
data TLB and a 1024-entry unified L2 TLB. With 4 KiB pages 1024 entries
cover only 4 MiB, so a program that touches 64 MiB at random keeps missing.

```
  load x  ->  TLB hit? --yes--> physical address (0 extra cycles)
                 | no
                 v
          hardware page-table walk (A72 and x86 walk in hardware; caches help)
          -> refill TLB -> retry         ~10-100+ cycles
```

`c/01` performs the same number of loads within 16 pages (TLB hits) and
spread over 16384 pages (misses), and prints the time per load. It then
repeats with `MADV_HUGEPAGE`, where one TLB entry covers 2 MiB.

- **Context switches.** A context switch changes `TTBR0`. ARM uses
  **ASIDs** (address-space IDs) to tag TLB entries, so they do not all have to
  be flushed. x86 uses PCIDs for the same purpose.
- **TLB shootdown.** `munmap` and `mprotect` must invalidate the entry on
  *every* CPU that might have cached it: `TLBI ... IS` broadcast on ARM, an
  IPI on x86. This is one reason `mprotect` in a hot loop is slow.

### 3.4 Page faults (`c/02_page_faults_demand_paging.c`)

A **page fault** is the exception the MMU raises when a translation is
missing or the access is not allowed. The kernel's handler
(`do_page_fault` → `handle_mm_fault`) looks up the **VMA** (virtual memory
area: one line of `/proc/self/maps`) for the address:

```
  fault at addr
    |
    +-- no VMA covers addr ------------------------------> SIGSEGV (SEGV_MAPERR)
    +-- VMA exists, access not allowed by its prot -------> SIGSEGV (SEGV_ACCERR)
    +-- anonymous, never touched ------> allocate zeroed frame, map it    = MINOR fault
    +-- file page already in page cache -> just map it                    = MINOR fault
    +-- file page not in cache / page in swap -> read from storage, map  = MAJOR fault
    +-- write to a COW-shared page ----> copy the page, map the copy RW   = MINOR fault
```

| | Minor fault | Major fault |
|---|---|---|
| needs I/O | no | yes (SD/eMMC/swap) |
| cost | ~0.5–2 µs (zeroing a 4 KiB page + bookkeeping) | ~100 µs (eMMC) to ms (SD card) |
| counted in | `getrusage().ru_minflt`, `/proc/<pid>/stat` field 10 | `ru_majflt`, field 12 |
| watch it | `perf stat -e minor-faults,major-faults ./prog`, `/usr/bin/time -v` |

`c/02` touches a fresh 32 MiB mapping and asserts that at least one minor fault
per page happened and RSS grew by about 32 MiB. A second pass costs 0
faults. It also creates major faults by dropping a file from the page cache
(`posix_fadvise(DONTNEED)`) and reading it through a mapping. That number is
printed, not asserted: readahead and tmpfs change it.

### 3.5 Demand paging, lazy allocation, copy-on-write (`c/02`)

**Demand paging.** `mmap()`, `brk()` and therefore a large `malloc()` only create
or extend a VMA, so RSS does not move (`c/02`: `+12 KiB` for a 32 MiB
mmap). RAM is spent on first touch, one page per fault. A consequence:
`malloc` "succeeding" proves nothing about whether the memory exists
(§3.10).

**MAP_POPULATE** (and `mlock`, §3.9) take all the faults inside `mmap()`
itself, which is what you want before a real-time loop starts. `c/02` shows
the first touch then costs (almost) no faults.

**Copy-on-write.** `fork()` does not copy memory. It copies the page
tables and marks every private writable page **read-only in both
processes**. The first write by either process faults. The kernel then sees a
private writable VMA with a read-only shared page, copies that one page, and
maps the copy writable. `c/02` measures it in the child: reading 2048 pages
takes 0 faults, writing them takes 2048. Under ThreadSanitizer the reads fault
too, because TSan writes its shadow memory on every read and the shadow pages
are copy-on-write as well. That is why `fork()` + `exec()` is cheap, and why a
large process that forks and keeps writing can still double its RAM use.

The zero page: reading a never-written anonymous page maps a single shared
**zero page** read-only (no allocation). The first write then allocates a real
page.

### 3.6 mmap flavours (`c/03_mmap_mprotect_sigsegv.c`)

```c
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
```

| | `MAP_PRIVATE` | `MAP_SHARED` |
|---|---|---|
| **anonymous** (`MAP_ANONYMOUS`, fd −1) | normal heap/stack memory. After fork: COW | shared with children after `fork()` ([11](../11-ipc-shared-memory/README.md)) |
| **file-backed** | reads from the file. Writes create private COW copies, the file never changes (how executables and `.data` are loaded) | the page-cache pages themselves: writes reach the file (`msync` for durability), and other processes see them |

Useful flags: `MAP_FIXED` (exactly this address, *replacing* whatever is
there, so prefer `MAP_FIXED_NOREPLACE`), `MAP_POPULATE`, `MAP_NORESERVE`
(no commit charge, §3.10), `MAP_HUGETLB` (§3.12), `MAP_LOCKED`,
`MAP_GROWSDOWN` (the stack), `MAP_STACK`.

`munmap(addr, len)` removes whole pages. Unmapping the middle of a VMA splits it in
two. `mremap` grows or moves a mapping without copying (glibc `realloc` uses it
for large blocks). The length must be the one you mapped (rounded up to
pages), which is why `cpp/01` keeps `(addr, len)` together in one object.

### 3.7 mprotect, guard pages and SIGSEGV (`c/03`, `cpp/02`)

`mprotect(addr, len, prot)` changes the permissions of whole pages:
`PROT_NONE`, `PROT_READ`, `PROT_WRITE`, `PROT_EXEC`. Modern kernels and
hardening disallow write+exec together in many configurations (W^X).

A forbidden access delivers **SIGSEGV** with a `siginfo_t`:

| field | meaning |
|---|---|
| `si_addr` | the exact faulting address |
| `si_code == SEGV_MAPERR` | no mapping at that address |
| `si_code == SEGV_ACCERR` | mapping exists, permission denied (guard page, read-only) |

`c/03` shows all of them. It overflows a buffer byte by byte into a
`PROT_NONE` **guard page** (fault at exactly `guard`), writes into an
unmapped hole (`MAPERR`), and stores into a `PROT_READ` file mapping
(`ACCERR`).

What a handler may do:

1. **Report and die.** The usual choice. Print with `write(2)`, not `printf`,
   then `_exit`.
2. **Recover.** `sigsetjmp` before the risky access, then `siglongjmp` from the
   handler. Only safe for faults you provoked on purpose at a known point,
   with no locks held and no C++ destructors skipped.
3. **Fix and retry.** Change the protection and *return*. The CPU re-executes
   the faulting instruction. `c/03` uses this to track which pages were written
   since a "snapshot" (only the first write to each page faults). Garbage
   collectors, checkpointing (CRIU uses soft-dirty bits), and `userfaultfd`
   (live migration) are built on this idea.

`cpp/02` packages the guard-page idea: a page-aligned **bump arena** with a
`PROT_NONE` page at each end (`[guard][arena][guard]`), exposed as a
`std::pmr::memory_resource`, plus an "electric fence" buffer whose last byte
sits right before a guard page, so an off-by-one faults on the exact byte.

### 3.8 madvise (`c/04_madvise_mlock_overcommit.c`)

| Advice | Effect |
|---|---|
| `MADV_DONTNEED` | drop the pages now. Private anonymous memory then **reads as zeros**. File/shared pages are re-read from the file. RSS drops at once |
| `MADV_FREE` | lazy version for anonymous memory: freed only under memory pressure (jemalloc, glibc trims) |
| `MADV_WILLNEED` | start readahead of a file range now |
| `MADV_SEQUENTIAL` / `MADV_RANDOM` | tune readahead |
| `MADV_HUGEPAGE` / `MADV_NOHUGEPAGE` | opt in/out of transparent huge pages (§3.12) |
| `MADV_DONTFORK` | the child does not get this range (DMA buffers!) |
| `MADV_COLD` / `MADV_PAGEOUT` | hint for reclaim (Android uses it) |

`mincore()` reports which pages of a range are resident. `c/04` uses it to
watch `MADV_WILLNEED` readahead, and `cpp/01` uses its `ENOMEM` result to
prove a range was unmapped without touching it.

### 3.9 mlock and real time (`c/04`)

A major fault in a 1 kHz control loop on the BeagleBone can take
milliseconds, so the deadline is gone. Real-time programs therefore:

```c
mlockall(MCL_CURRENT | MCL_FUTURE);   /* pin everything now and later        */
/* pre-fault the stack: touch e.g. 256 KiB of a local array once            */
/* allocate all buffers at start-up (MAP_POPULATE / memset) and never free  */
```

- `mlock(addr, len)` faults the range in **now** and keeps it in RAM (it is
  never swapped and never reclaimed). `c/04` checks all 256 pages are
  resident at once and `VmLck` shows 1024 kB.
- `MCL_ONFAULT` (Linux 4.4+) locks pages as they are faulted, without
  pre-faulting.
- Limited by **RLIMIT_MEMLOCK** (`ulimit -l`) unless you have
  `CAP_IPC_LOCK`. On the board, set `LimitMEMLOCK=infinity` in the systemd unit
  of the RT service. `c/04` prints the limit and skips with an explanation on
  `EPERM`/`ENOMEM`.
- ASan and TSan **intercept `mlock`/`mlockall` and turn them into no-ops**
  (locking their terabytes of shadow memory is impossible). `c/04` calls
  `syscall(SYS_mlock)` under a sanitizer so the demo still really locks, and
  skips `mlockall` there.
- The rest of RT setup (SCHED_FIFO, CPU isolation, PREEMPT_RT) is in
  [15](../15-scheduling-and-real-time/README.md).

### 3.10 Overcommit and the OOM killer (`c/04`)

Because memory is allocated on touch, Linux by default promises more than
it has. `/proc/sys/vm/overcommit_memory`:

| value | policy |
|---|---|
| **0** (default) | heuristic: refuse only a single request that is obviously impossible (more than RAM + swap) |
| 1 | always say yes (some scientific codes, Redis recommends it) |
| 2 | strict: total commitments ≤ `CommitLimit` = swap + RAM × `overcommit_ratio`% (+ `overcommit_kbytes`). `malloc` really returns NULL. Good for safety-critical embedded systems that would rather fail early |

`/proc/meminfo` shows `CommitLimit` and `Committed_AS` (what has been promised).
`c/04` maps 16 GiB with `MAP_NORESERVE` (fine, touching 2 pages costs 8 KiB),
then asks for 2 × (RAM + swap) without it and gets `ENOMEM` in mode 0.

When the promises are called in and no memory can be reclaimed, the **OOM
killer** picks the process with the highest `/proc/<pid>/oom_score` (roughly
its share of RAM + swap, scaled 0–1000) adjusted by `oom_score_adj`
(−1000 = never kill, +1000 = kill first). Anyone may **raise** their own
`oom_score_adj`. Lowering it needs `CAP_SYS_RESOURCE`. `c/04` demonstrates
both in a child. Nothing here causes a real OOM. On an embedded device you
protect the critical daemon (`OOMScoreAdjust=-900` in systemd), sacrifice the
uploader, and often prefer `vm.overcommit_memory=2` plus memory cgroups
(`MemoryMax=`) so that failures are predictable.

### 3.11 /proc/self/maps and smaps (`c/05_maps_smaps_hugepages.c`)

`/proc/self/maps` has one line per VMA (format explained in
[dsa/02 §3.5](../../dsa/02-memory-management/README.md)). `/proc/self/smaps`
adds the memory accounting per VMA, and `smaps_rollup` the totals:

| Metric | Meaning | Use |
|---|---|---|
| **VSZ** (`Size`, `statm` field 1) | all reserved address space | almost meaningless: ASan shows ~20 TiB, TSan ~120 TiB (shadow reservations) |
| **RSS** (`Rss`) | pages in RAM, shared pages counted fully in every process | `top`, `ps`. Adding RSS over processes double-counts |
| **PSS** (`Pss`) | a page shared by N processes counts 1/N | the only number you can **sum**. `smem`, Android's memory accounting |
| **USS** | private pages only (`Private_Clean + Private_Dirty`) | what you get back if the process dies |
| `Shared_Dirty` / `Private_Dirty` | modified pages | dirty pages cost swap or writeback |
| `AnonHugePages` | backed by THP | §3.12 |
| `Swap` | swapped-out part | |

`c/05` finds which VMA holds its code, data, heap, an mmap, libc and the
stack (and checks W^X: code is `r-xp`, data `rw-p`). It then forks a child
that touches the same 1 MiB `MAP_SHARED` region and reads its own smaps:
`Rss 1024 kB, Pss 512 kB`. Under a sanitizer `malloc` returns memory from the
sanitizer's own mmap'd arena, not `[heap]`, which is a nice reminder that
"the heap" is just a convention.

### 3.12 Huge pages (`c/05`, `c/01`)

One 2 MiB page = 512 small pages behind **one** TLB entry and **one** fault.

- **Transparent huge pages (THP).** The kernel uses 2 MiB pages for
  anonymous memory when a VMA has an aligned 2 MiB range.
  `/sys/kernel/mm/transparent_hugepage/enabled` is `always`, `[madvise]`
  (Ubuntu default: only where `madvise(MADV_HUGEPAGE)` asked) or `never`.
  `khugepaged` collapses small pages later. `c/05` maps an aligned 4 MiB region
  with `MADV_HUGEPAGE` and prints `AnonHugePages: 4096 kB`. Downsides: up to
  2 MiB of RSS for one touched byte, latency spikes from compaction, and COW of
  a huge page. Real-time and database setups often choose `never` or `madvise`.
- **hugetlbfs.** The admin reserves pages up front
  (`echo 64 > /proc/sys/vm/nr_hugepages`, or `hugepages=` on the kernel command
  line). The program uses `mmap(MAP_HUGETLB)` or a file in a hugetlbfs mount.
  The memory is guaranteed and never swapped. DPDK, databases and some camera
  pipelines use it. `c/05` tries it and skips cleanly when none are reserved.
- arm64 with 4 KiB pages: 2 MiB (PMD) and 1 GiB (PUD) blocks, plus 64 KiB
  "contiguous PTE" hints. With a 64 KiB granule the block size is 512 MiB.

### 3.13 Stack growth and guard pages (`c/06_stack_and_aslr.c`)

- The **main thread's stack** is a VMA marked `VM_GROWSDOWN`. A fault just
  below it extends it, up to `RLIMIT_STACK` (`ulimit -s`, 8 MiB). The kernel
  keeps `stack_guard_gap` (256 pages = 1 MiB) between it and other mappings,
  since the "Stack Clash" fixes of 2017, and GCC's `-fstack-clash-protection`
  touches large frames page by page so a frame cannot jump over the gap.
  `c/06` recurses 256 frames of 4 KiB and watches `[stack]` grow from 136 KiB
  to ~1.1 MiB.
- **Thread stacks** are ordinary fixed-size mmaps (8 MiB default =
  `RLIMIT_STACK`, set with `pthread_attr_setstacksize`). glibc puts a
  **guard page** (`---p`, `pthread_attr_setguardsize`) at the low end. `c/06`
  finds it in `/proc/self/maps` directly below a thread's stack. On a board
  with 4 GiB RAM and 100 threads, 8 MiB each is only *virtual*: RSS grows
  only with the depth actually used.
- Stack overflow = a fault in the guard: SIGSEGV. To report it you need
  `sigaltstack()`, because the handler cannot run on the stack that just
  overflowed.

### 3.14 ASLR (`c/06`)

**Address-space layout randomisation** moves the stack, the mmap base
(libraries, anonymous mappings), the vDSO, the heap start
(`randomize_va_space = 2`), and for PIE executables the program itself to
random places at every `exec()`. `c/06` re-executes itself: without ASLR two
runs print identical addresses, with ASLR they differ. ASan's heap stays at
the fixed `0x6xxxxxxxxxxx` area either way.

- Per process: `setarch $(uname -m) -R prog` or
  `personality(ADDR_NO_RANDOMIZE)`. The flag survives `exec()`. gdb sets it by
  default (`set disable-randomization on`), which is why a bug can "vanish in
  the debugger".
- **Why the Makefile runs TSan under `setarch -R`:** ThreadSanitizer maps its
  shadow memory at fixed addresses and expects the program in specific ranges.
  Ubuntu's kernel uses `vm.mmap_rnd_bits = 32` (more randomness than GCC 11's
  TSan knows about), so libraries can land where TSan expects shadow:
  `FATAL: ThreadSanitizer: unexpected memory mapping`. `c/06` sees
  `ADDR_NO_RANDOMIZE` set when run with `SAN=tsan` and skips the randomised
  re-exec, because that child would abort with exactly that message (see
  [skills/concurrency-testing](../../skills/concurrency-testing/SKILL.md)).
- Entropy on arm64 depends on the VA size: ~18–33 bits for mmap
  (`mmap_rnd_bits`). Kernel ASLR (KASLR) does the same for the kernel image.

### 3.15 The kernel side (brief)

| Kernel API | What you get | Typical use |
|---|---|---|
| `kmalloc` / `kzalloc` | physically contiguous, from the linear map, ≤ a few MiB, fast | small objects, buffers a device may DMA from (with the DMA API) |
| `vmalloc` | virtually contiguous only, built from scattered pages + new page tables | big module buffers, never for DMA |
| `alloc_pages` / `get_free_pages` | 2^order physical pages | page-level allocators |
| `dma_alloc_coherent` | memory both CPU and device see consistently (uncached or snooped) | descriptor rings, small DMA buffers |
| `remap_pfn_range` / `vm_insert_page` in `.mmap` | map kernel or device memory into a user VMA | camera/GPU buffers, `/dev/mem`-style access |

Details are in
[linux-device-drivers/02-kernel-memory-and-data-structures](../../linux-device-drivers/02-kernel-memory-and-data-structures/README.md)
and [linux-device-drivers/08-dma-and-mmap](../../linux-device-drivers/08-dma-and-mmap/README.md).
On the **TDA4VM** devices do not use physical addresses directly. They sit
behind IOMMUs (an Arm SMMU in front of DMA masters, and the C7x/R5F remote
cores have their own MMUs), so a device sees *I/O virtual addresses* mapped by the
kernel's DMA API. TI's vision stack shares buffers between the A72, C7x DSP
and video accelerators through `dma-buf` fds from `/dev/dma_heap/*`. Each core
maps the same physical memory, and cache maintenance on the A72 is done
explicitly with `DMA_BUF_IOCTL_SYNC`.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| TLB hit | ~0 extra cycles | translation cached |
| TLB miss | 4 table reads (often cached), ~10–100 cycles | hardware walk of L0–L3 |
| minor fault | ~0.5–2 µs (`c/02` prints it) | exception + allocate + zero 4 KiB + map |
| major fault | ~0.1 ms (eMMC) – 10 ms (SD/HDD) | block I/O |
| COW fault | minor fault + 4 KiB copy | copy one page |
| `mmap` (anonymous) | O(1), µs | creates a VMA (maple tree insert), no pages |
| `mmap` + `MAP_POPULATE` / `mlock` of N pages | O(N) faults up front | pays the faults at once |
| `munmap` / `mprotect` of N pages | O(N) PTE updates + TLB shootdown | IPI or broadcast TLBI to other CPUs |
| `fork()` | O(page tables), not O(memory) | copy page tables, mark COW |
| `madvise(DONTNEED)` N pages | O(N) | unmap + free frames |
| reading `/proc/self/smaps` | O(VMAs × pages) | walks page tables. Slow for big processes, use `smaps_rollup` |
| page table memory | ~8 B per mapped 4 KiB page (+ upper levels) | 2 MiB of tables per GiB mapped with 4 KiB pages, 4 KiB with 2 MiB pages |

---

## 5. C vs C++

| Concept | C | C++17 |
|---|---|---|
| own a mapping | `void *p` + `size_t len` + discipline | move-only `Mapping` (`cpp/01`): destructor unmaps with the right length, moved-from = empty |
| typed access | cast | `m.as<T>()` with `static_assert(is_trivially_copyable_v<T>)` |
| arena allocation | bump pointer struct ([dsa/02 §3.6](../../dsa/02-memory-management/README.md)) | `GuardedArena` + `std::pmr::memory_resource` → any `std::pmr::vector/string/map` lives in it (`cpp/02`) |
| catching an overflow | guard page + `sigsetjmp` | same. `siglongjmp` skips destructors, so keep only trivial objects between setjmp and the fault |
| errors | `MAP_FAILED` + errno | `std::system_error` |
| standard library equivalent | — | none: the standard has no mmap. Boost.Interprocess `mapped_region`, or your own class |

`std::pmr::monotonic_buffer_resource` is the standard bump allocator. It has
no guard pages and falls back to an upstream resource when full.
`cpp/02`'s resource throws `bad_alloc` instead, which is what you want when
"heap use after start-up" counts as a bug.

---

## 6. Where it is used in the real world

- **Linux kernel.** `mm/memory.c` (`handle_mm_fault`, `do_anonymous_page`,
  `do_wp_page` for COW), `mm/mmap.c`, `mm/mprotect.c`, `mm/madvise.c`,
  `mm/oom_kill.c`, `mm/huge_memory.c`, `fs/proc/task_mmu.c` (maps, smaps,
  pagemap). VMAs live in a maple tree since 6.1. The arm64 fault entry is
  `arch/arm64/mm/fault.c`.
- **glibc malloc.** Uses `brk` for small requests and `mmap` above
  `M_MMAP_THRESHOLD` (128 KiB, adaptive), `madvise(MADV_DONTNEED)`/`munmap` to
  give memory back, and `mremap` for large `realloc`. jemalloc and tcmalloc use
  `MADV_FREE`/`MADV_DONTNEED` and huge pages.
- **Runtimes and GCs.** JVM, V8, Go and .NET reserve huge `PROT_NONE` ranges and
  commit them with `mprotect`, use guard pages for stack overflow detection, and
  write barriers built on page protection. The JVM's safepoint poll is a load
  from a page it `mprotect`s to stop all threads.
- **Databases.** LMDB and SQLite (optional) `mmap` the whole database file.
  PostgreSQL uses huge pages for shared buffers. Redis relies on fork + COW for
  snapshots, and recommends `overcommit_memory=1` and THP `never`.
- **Embedded and real-time.** PREEMPT_RT applications call `mlockall` and
  pre-fault their stacks (the `rt-tests`/`cyclictest` pattern). Camera stacks
  (V4L2 `mmap`, dma-buf) map driver buffers directly into user space for zero
  copy ([16](../16-zero-copy-and-async-io/README.md)). Android uses PSS-based
  accounting and `lmkd` instead of the OOM killer.
- **Security.** ASLR, NX/W^X, stack guard pages, `-fstack-clash-protection`,
  and the hidden PFNs in pagemap are all virtual-memory features.
- **Tools.** `pmap -x <pid>`, `smem`, `/usr/bin/time -v`,
  `perf stat -e page-faults,dTLB-load-misses`, `valgrind --tool=massif`,
  `vmstat 1`, and ASan (its shadow memory is itself one huge `MAP_NORESERVE`
  mapping).

---

## 7. Common mistakes and pitfalls

1. **Believing `malloc` returning non-NULL means the memory exists.** With
   overcommit it can fail later, at first touch, as an OOM kill. For
   predictability use `overcommit_memory=2`, cgroup limits, or pre-touch.
2. **Reading VSZ as memory use.** Use RSS for one process and PSS when
   summing processes.
3. **Hard-coding 4096.** arm64 kernels can use 16 or 64 KiB pages. Use
   `sysconf(_SC_PAGESIZE)`.
4. **`munmap` with the wrong length**, or keeping a pointer after `munmap`
   (`c/07_use_after_munmap_bug.c`). The next `mmap` often returns the *same
   address*, and the stale write silently corrupts the new owner. ASan does not
   track `mmap`/`munmap`.
5. **`MAP_FIXED` over an existing mapping** silently replaces it. Use
   `MAP_FIXED_NOREPLACE`.
6. **Expecting `MADV_DONTNEED` to keep data.** For private anonymous memory
   it zero-fills, so the data is gone.
7. **A real-time thread that page-faults:** forgot `mlockall`, allocates in
   the loop, or touches a new stack depth for the first time. Pre-fault at
   start-up.
8. **`mlockall(MCL_CURRENT|MCL_FUTURE)` with a small `RLIMIT_MEMLOCK`.**
   Later `mmap`/`malloc` calls then fail with `EAGAIN`/`ENOMEM`. Size the limit.
9. **Recovering from arbitrary SIGSEGVs.** Only recover from faults you
   provoked at a known point. After a real bug the program state is
   unknown: log and die.
10. **Huge stack arrays in threads** (`char buf[16 MiB]` on an 8 MiB stack)
    jump over the guard page without `-fstack-clash-protection`. Use the heap.
11. **THP surprises:** RSS of 2 MiB for one byte, latency spikes from
    compaction, fork + write copying 2 MiB. Check `AnonHugePages` and set
    `madvise` mode on RT systems.
12. **Fork after allocating a lot, then writing in the parent**: COW
    doubles memory. Prefer `posix_spawn`/`vfork`+`exec` to start helpers from
    big processes.
13. **Disabling ASLR globally** (`randomize_va_space=0`) to make TSan or a
    debugger happy. Use `setarch -R` for that one process instead.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_pages_and_translation.c](c/01_pages_and_translation.c) | page size (sysconf + auxv), L0–L3 index split of a real address, `/proc/self/pagemap` present bits before/after touching, hidden PFNs, TLB-hit vs TLB-miss vs huge-page load timing | `make run P=linux-internals/14-virtual-memory/c/01_pages_and_translation.c` |
| [c/02_page_faults_demand_paging.c](c/02_page_faults_demand_paging.c) | minor faults + RSS when touching a fresh mmap, lazy large `malloc`, `MAP_POPULATE`, major faults after `FADV_DONTNEED`, copy-on-write faults measured in a forked child | `make run P=linux-internals/14-virtual-memory/c/02_page_faults_demand_paging.c` |
| [c/03_mmap_mprotect_sigsegv.c](c/03_mmap_mprotect_sigsegv.c) | private vs shared file mapping, `PROT_READ` store, guard page (`SEGV_ACCERR` at the exact address), hole (`SEGV_MAPERR`), `siglongjmp` recovery, write tracking by un-protecting in the handler | `make run P=linux-internals/14-virtual-memory/c/03_mmap_mprotect_sigsegv.c` |
| [c/04_madvise_mlock_overcommit.c](c/04_madvise_mlock_overcommit.c) | `MADV_DONTNEED` (anon zero-fill vs file re-read), `MADV_WILLNEED` + `mincore`, `mlock` + `VmLck`, `mlockall` (skipped under sanitizers), overcommit mode, `MAP_NORESERVE` 16 GiB, impossible request → `ENOMEM`, `oom_score` / `oom_score_adj` | `make run P=linux-internals/14-virtual-memory/c/04_madvise_mlock_overcommit.c` |
| [c/05_maps_smaps_hugepages.c](c/05_maps_smaps_hugepages.c) | which VMA holds code/data/heap/mmap/libc/stack, VSZ vs RSS vs PSS, Pss = Rss/2 for a mapping shared with a child, THP via `MADV_HUGEPAGE` (`AnonHugePages`), `MAP_HUGETLB` | `make run P=linux-internals/14-virtual-memory/c/05_maps_smaps_hugepages.c` |
| [c/06_stack_and_aslr.c](c/06_stack_and_aslr.c) | `[stack]` VMA growing during deep recursion, thread stack guard page in maps, `randomize_va_space`, `ADDR_NO_RANDOMIZE` (set under `SAN=tsan`), self re-exec with and without ASLR | `make run P=linux-internals/14-virtual-memory/c/06_stack_and_aslr.c` |
| [c/07_use_after_munmap_bug.c](c/07_use_after_munmap_bug.c) | **BUG**: stale pointer after `munmap`: (a) SIGSEGV in a child, (b) address reused by the next mmap, silent corruption caught by the self-check; ASan's UAF check does not apply | `make run P=linux-internals/14-virtual-memory/c/07_use_after_munmap_bug.c` (exits 1) |
| [cpp/01_mapping_raii.cpp](cpp/01_mapping_raii.cpp) | move-only `Mapping` (anonymous/file, `as<T>()`, protect/advise/sync), destructor verified with `mincore`, moves into a vector, private vs shared file views, `system_error` | `make run P=linux-internals/14-virtual-memory/cpp/01_mapping_raii.cpp` |
| [cpp/02_guard_page_arena.cpp](cpp/02_guard_page_arena.cpp) | `GuardedArena` `[guard][arena][guard]` bump allocator, overflow/underflow caught by SIGSEGV at the guard, `std::pmr::vector` in the arena (bad_alloc when full), electric-fence buffer faulting at byte 100 of 100 | `make run P=linux-internals/14-virtual-memory/cpp/02_guard_page_arena.cpp` |

Every non-bug program is clean under ASan/UBSan and under ThreadSanitizer:

```bash
make run P=linux-internals/14-virtual-memory/c/06_stack_and_aslr.c SAN=tsan BUILD=build/tsan
```

Faults and memory from the outside (build with `SAN=0` for clean numbers):

```bash
make SAN=0 BUILD=build/nosan build/nosan/linux-internals/14-virtual-memory/c/02_page_faults_demand_paging
/usr/bin/time -v build/nosan/linux-internals/14-virtual-memory/c/02_page_faults_demand_paging | tail -20
perf stat -e page-faults,minor-faults,major-faults,dTLB-load-misses build/nosan/...   # perf_event_paranoid <= 2
pmap -x $$          # VSZ/RSS/dirty per mapping of your shell
```

---

## 9. Exercises

**Easy**
1. Run `c/02` with `SAN=0` under `/usr/bin/time -v` and compare "Minor page
   faults" with the program's own counts. Where do the extra faults come from?
2. In `c/05`, fork *two* children that touch the shared region. Predict Pss.
3. Change `c/01`'s TLB walk to a stride of 64 bytes inside 64 MiB. Why is it
   now fast even though it covers the same memory?

**Medium**
4. Write `rss_watch`: `malloc` 256 MiB, touch it in 16 MiB steps printing RSS
   and minflt, then `free` it. Does RSS go down after `free`? Try with
   `malloc_trim(0)` and with `M_MMAP_THRESHOLD` set by `mallopt`.
5. Extend `cpp/02` with a `GuardedStack` (`[guard][stack]` with
   `makecontext`/`swapcontext` or a thread with `pthread_attr_setstack`), and
   overflow it with deep recursion. Install a handler with `sigaltstack` that
   reports "stack overflow in coroutine".
6. Implement the dirty tracking of `c/03` with `/proc/self/pagemap` soft-dirty
   bits instead (write `4` to `/proc/self/clear_refs`, then check bit 55).
   Compare the cost with the mprotect approach.
7. Make `c/06` print the vDSO and `[vvar]` addresses too. Which of them does
   ASLR move?

**Hard**
8. On the BeagleBone (PREEMPT_RT): run `cyclictest -m -p 90 -i 1000` with and
   without `-m` (mlockall) while another process allocates and frees 1 GiB in a
   loop. Compare max latency.
9. As root on the board, read a PFN from pagemap for a buffer, then read the
   same physical address through `/dev/mem` (if `CONFIG_STRICT_DEVMEM` allows)
   and confirm it holds your data. What does this tell you about why PFNs are
   hidden?
10. Read `do_wp_page()` in `mm/memory.c`. When does the kernel *reuse* the
    page instead of copying it on a COW fault (hint: `page_count == 1`)?

---

## 10. Remember forever

```
VA -> page tables (4 levels x 9 bits + 12-bit offset, 4 KiB pages) -> PFN; TLB caches it
Page = unit of mapping/protection: sysconf(_SC_PAGESIZE) (4K; arm64 may be 16K/64K)
mmap/malloc reserve; FIRST TOUCH faults the page in (minor). Major = from storage
fork = copy page tables + mark COW; first write copies one page
fault: no VMA -> SEGV_MAPERR; wrong prot -> SEGV_ACCERR; si_addr = exact address
PROT_NONE guard page: overflow faults on the first byte. Recover only with sigsetjmp at a known point
MADV_DONTNEED on private anon -> zeros. mlock/mlockall + prefault for real time (RLIMIT_MEMLOCK)
Overcommit 0/1/2; OOM picks max oom_score; oom_score_adj -1000..1000
VSZ (reserved) >= RSS (resident) >= PSS (shared/N) - sum PSS, never RSS
ASLR randomises at exec; setarch -R / ADDR_NO_RANDOMIZE turns it off (TSan needs it)
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Walk the MMU through translating a 48-bit virtual address with 4 KiB
   pages. How many bits index each level, and how big is one table?
2. What does the TLB cache, why does a 64 MiB random access pattern miss
   in it, and how do huge pages help?
3. Name the cases the page-fault handler distinguishes. Which are minor,
   which are major, which end in SIGSEGV?
4. Why does a 1 GiB `malloc` not raise RSS? What raises it, and what tool
   or file shows it?
5. Explain copy-on-write after `fork()` step by step. Why does reading a
   page in the child not fault, but writing it does?
6. Private vs shared file mapping: what happens to the file when you write
   through each? What does `msync` add?
7. `SEGV_ACCERR` vs `SEGV_MAPERR`: give an access that causes each. How does
   a guard page turn an overflow into a crash at the exact byte?
8. When is it acceptable to `siglongjmp` out of a SIGSEGV handler, and what
   breaks in C++ if you do?
9. What does `MADV_DONTNEED` do to private anonymous memory versus a shared
   file mapping?
10. Why does a real-time loop call `mlockall` and pre-fault its stack? What
    limit can make it fail, and how do you raise it on the board?
11. What are the three `overcommit_memory` modes? Which would you choose for a
    safety-critical device, and why?
12. VSZ, RSS, PSS, USS: define each and say which one you add up across
    processes.
13. What does ASLR randomise, how do you turn it off for one process, and
    why does the Makefile do that for ThreadSanitizer?
14. `kmalloc` vs `vmalloc` vs `dma_alloc_coherent`: which can a device DMA
    from, and what changes on the TDA4VM with its IOMMUs?
