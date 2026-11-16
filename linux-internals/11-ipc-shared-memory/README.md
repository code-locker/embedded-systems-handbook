# 11 — Shared memory

> Every other IPC mechanism copies your data through the kernel. Shared
> memory **maps the same physical pages into two processes**: a store in one
> is a load in the other, with no system call and no copy. That makes it the
> fastest IPC there is. It also makes it the most dangerous, because the
> kernel no longer serialises anything. You own the synchronisation, the
> layout, and crash recovery.

The map of all IPC mechanisms and the "which one should I pick" guide are in
[lesson 09, section 1](../09-ipc-pipes-and-fifos/README.md#1-why-this-exists).

---

## 1. Why this exists

A 1920×1080 NV12 camera frame is ~3 MB. At 30 fps that is 90 MB/s. Through
a pipe or socket every byte is copied twice (user → kernel → user), so
180 MB/s of `memcpy`, plus syscalls and cache pollution. On the BeagleBone
AI-64's two Cortex-A72 cores that is a large share of a core spent moving
bytes nobody changes. With shared memory the camera process writes the frame
**once** into a buffer, and the encoder or AI process reads it **in place**.
Only a tiny notification ("frame 42 is in slot 3") has to cross the kernel.

The price:

- **Synchronisation is yours.** Two processes touching the same bytes
  without a lock or atomics is a data race, and TSan cannot see it across
  processes (`c/09_shared_counter_bug.c`).
- **Notification is yours.** Shared memory has no "data arrived" event. Pair
  it with a futex/condvar/semaphore in the memory itself, or an
  eventfd/socket/mq for the wake-up.
- **Crashes are yours.** A process dying while holding a lock in shared
  memory leaves it locked, unless the mutex is robust (`c/06`).
- **Layout is yours.** Both sides must agree on every byte, with no pointers
  (`c/08`), and on a version.

---

## 2. Mental model

**Shared memory is a whiteboard in a corridor between two offices.** Anyone
can write on it or read it instantly, with no courier. But if two people
write in the same spot at once the result is garbage. So you also need a
marker-pen token (mutex), a "new info" bell (notification), and a rule that
if someone faints holding the pen, the next person may take it and tidy up
(robust mutex).

```
   process A virtual memory           physical RAM            process B virtual memory
   +------------------------+                                 +------------------------+
   | 0x7f12_3400_0000  ---------+                        +--------- 0x7f98_ab00_0000  |
   |   (mapping of "/frames")   |    +---------------+   |    (mapping of "/frames")   |
   |                        |   +--> | page frame #N | <-+                        |
   |                        |   +--> | page frame #M | <-+                        |
   +------------------------+        +---------------+        +------------------------+
          same object, same physical pages, DIFFERENT virtual addresses
          => never store a pointer into it; store offsets (c/08)

   A: frame->pixels[i] = v;  ----- plain store, no syscall ---->  B: x = frame->pixels[i];
      (needs ordering: lock, or release/acquire atomics, to be SEEN correctly)
```

The ways to obtain such pages:

| How | Name / handle | Who can attach | Cleanup |
|---|---|---|---|
| `shm_open` + `mmap(MAP_SHARED)` | `"/name"` → `/dev/shm/name` | anyone with permission | `shm_unlink` (else stays until reboot) |
| `mmap(MAP_SHARED \| MAP_ANONYMOUS)` before `fork` | none | parent + children | automatic at last `munmap`/exit |
| `mmap` of a regular file, `MAP_SHARED` | file path | anyone with permission | the file stays (persistent) |
| `memfd_create` + `mmap` | an fd only | whoever gets the fd (fork, `SCM_RIGHTS`) | automatic at last fd/mapping |
| System V `shmget` + `shmat` | `key_t` → int id | anyone with permission | `IPC_RMID` + last `shmdt` |

---

## 3. How it works

### 3.1 POSIX `shm_open` + `mmap`  (`c/01_posix_shm.c`)

```c
int fd = shm_open("/frames_1234", O_CREAT | O_EXCL | O_RDWR, 0600);
ftruncate(fd, size);                        /* new objects are 0 bytes! */
void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);                                  /* the mapping keeps it alive */
... use p ...
munmap(p, size);
shm_unlink("/frames_1234");                 /* remove the name */
```

- `shm_open` creates a file in the **tmpfs** mounted at `/dev/shm`. The
  memory is RAM (swappable), not disk. `ls -l /dev/shm` shows it.
- **`ftruncate` is mandatory** for a new object. Touching a mapping beyond
  the object's size gives **`SIGBUS`**, not `SIGSEGV`.
- Mappings are whole pages. A 1096-byte object still occupies one 4 KiB
  page.
- After `fork()`, a `MAP_SHARED` mapping stays shared at the **same**
  address. A process that opens and maps the name itself usually gets a
  **different** address (the program prints both).
- `shm_unlink` removes the name only. Existing mappings keep working until
  `munmap`. Forgetting it leaks RAM in `/dev/shm` until reboot.
- In `c/01`, `waitpid()` is the synchronisation: after the child has exited,
  all its stores are visible. Real code needs a lock or atomics (§3.5–3.7).

### 3.2 Anonymous shared memory and file mappings  (`c/02_anon_and_file_mmap.c`)

```
  MAP_SHARED|MAP_ANONYMOUS, then fork        MAP_PRIVATE|MAP_ANONYMOUS, then fork
  parent ──┐                                 parent ──> [page]  (read-only, shared)
           ├──> [page]  writes visible        child  ──┘
  child  ──┘    to both                      child writes -> page fault -> [copy]
                                             parent still sees the old value (COW)
```

- **`MAP_SHARED | MAP_ANONYMOUS` before `fork()`** is the simplest shared
  memory for parent/child. There is no name and no cleanup beyond `munmap`.
- **`MAP_PRIVATE`** (and all ordinary heap/stack memory) becomes
  **copy-on-write** after fork. That is what makes `fork()` cheap, and why
  a `malloc`'d buffer cannot be used to talk to a child.
- **`mmap` of a regular file** with `MAP_SHARED` maps the file's **page
  cache** pages. Stores change the file, `read()`/`write()` on the same file
  see the same pages, and other processes mapping the file share them too.
  `msync(MS_SYNC)` is about **durability** (flush to storage before power
  loss). Other processes see the data immediately, without msync.

### 3.3 `memfd_create` and sealing  (`c/03_memfd_create.c`)

`memfd_create("name", MFD_CLOEXEC | MFD_ALLOW_SEALING)` returns an fd to an
anonymous RAM file. It has no global name to collide with or leak, and it is
freed when the last fd and mapping go away. You share it by handing over the
**fd**: through `fork()`, or to an unrelated process with `SCM_RIGHTS` over a
Unix socket ([lesson 12](../12-ipc-unix-sockets/README.md)).

**Seals** (`fcntl(fd, F_ADD_SEALS, …)`) make guarantees the receiver can
check with `F_GET_SEALS`, instead of trusting the sender:

| Seal | Forbids | Protects the receiver from |
|---|---|---|
| `F_SEAL_SHRINK` | `ftruncate` smaller | `SIGBUS` while reading a buffer that shrank |
| `F_SEAL_GROW` | growing | unexpected size changes |
| `F_SEAL_WRITE` | `write`, writable shared mappings | content changing while being read/validated |
| `F_SEAL_SEAL` | adding/removing seals | — (freezes the set) |

`F_SEAL_WRITE` fails with `EBUSY` while a writable mapping exists, so the
producer unmaps first. Wayland clients send pixel buffers to the compositor
this way. Android replaced ashmem with memfd for the same purpose.

### 3.4 System V shared memory  (`c/04_sysv_shm.c`)

`shmget(key, size, IPC_CREAT | 0600)` → id, `shmat(id, NULL, 0)` → address,
`shmdt(addr)`, `shmctl(id, IPC_RMID, NULL)`. `IPC_RMID` only **marks** the
segment. It is destroyed when the attach count `shm_nattch` reaches 0.
`ipcs -m` shows `dest` for marked segments, and `ipcrm -m <id>` removes one.
Limits: `/proc/sys/kernel/shmmax`, `shmall`, `shmmni`. You meet it in
X11's MIT-SHM extension, older PostgreSQL and legacy industrial code. Prefer
`shm_open`/`memfd` in new code.

### 3.5 Process-shared mutex, condvar, semaphore  (`c/05_pshared_mutex_condvar_sem.c`)

Put the synchronisation objects **inside** the shared memory and mark them
process-shared:

```c
pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);   /* timeouts immune to clock jumps */
sem_init(&box->sem, /*pshared=*/1, 0);
```

```
  struct shm_box (in MAP_SHARED memory)
  +---------+-----------+----------+------------------+---------------+
  | mutex   | not_empty | not_full | head tail count  | slots[8] ring |
  | pshared | pshared   | pshared  |                  |               |
  +---------+-----------+----------+------------------+---------------+
  child: lock; while(full) wait(not_full); put; signal(not_empty); unlock
  parent: lock; while(empty && !done) wait(not_empty); get; signal(not_full); unlock
```

This is the bounded buffer of
[dsa/19 (c/03)](../../dsa/19-system-design-structures/README.md) and
[lesson 04](../04-condition-variables/README.md), unchanged, between two
processes. All of these are built on the **futex** (lesson 06). The futex
word sits in the shared page, and the kernel keys waiters by the *physical*
page, not the virtual address, so waits and wakes meet across processes.
Initialise once (by the creator, before anyone else looks), destroy once
(after everyone is done). Use timed waits so a dead peer fails the test
instead of hanging it. The second part runs a strict ping-pong with two
`sem_init(pshared=1)` semaphores.

### 3.6 Robust mutexes  (`c/06_robust_mutex.c`)

This is the cross-process case that
[lesson 03](../03-mutexes-and-deadlock/README.md) refers to here. Threads of
one process die together. Processes die alone (OOM killer, segfault,
`kill -9`).

```
  normal mutex:  child locks ── SIGKILL ── (mutex still says "owned by child")
                 parent: pthread_mutex_lock -> blocks FOREVER

  robust mutex:  child locks ── SIGKILL ── kernel walks the dead thread's robust list,
                                           sets FUTEX_OWNER_DIED, wakes a waiter
                 parent: pthread_mutex_lock -> EOWNERDEAD  (and now OWNS the lock)
                         repair the data  ->  pthread_mutex_consistent()  ->  unlock
                 forgot consistent()? unlock -> mutex is ENOTRECOVERABLE forever
```

`pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST)` plus `setpshared`.
The program shows all three outcomes. A stalled normal mutex, where
`timedlock` is used so it cannot hang. EOWNERDEAD → repair the broken
invariant (`a + b == 100`) → consistent. And `ENOTRECOVERABLE` when
`consistent()` is forgotten, fixed only by destroy + init. The hard part in
practice is the **repair**: the dead owner may have left the data
half-updated. Keep a journal or make updates idempotent.

### 3.7 Lock-free SPSC ring across processes  (`c/07_shm_spsc_ring.c`)

The single-producer/single-consumer ring of
[dsa/07 §3.8](../../dsa/07-stacks-and-queues/README.md) works between
processes unchanged. Producer and consumer each own one index. The producer
writes the slot, then `store(tail, release)`. The consumer
`load(tail, acquire)`s, then reads the slot. On the A72 those become
`STLR`/`LDAR`. Extra rules across processes:

- atomics must be **lock-free** (`_Static_assert(ATOMIC_INT_LOCK_FREE == 2)`;
  `std::atomic<T>::is_always_lock_free`). A non-lock-free atomic uses a
  lock table in each process's *private* memory, so it gives no
  exclusion at all;
- **indices, never pointers**, in the ring;
- `head` and `tail` on separate **64-byte cache lines** (Cortex-A72 line
  size), checked with `_Static_assert(offsetof(...))`. `mmap` is
  page-aligned, so the alignment holds in every process;
- each side maps the object **by name** (possibly at different addresses).

```
  offset   0: head   (consumer writes)   [64 B line]
  offset  64: tail   (producer writes)   [64 B line]
  offset 128: slots[256] {seq, value, check = ~value}
  producer: slot = s; store_release(tail, t+1)    consumer: t = load_acquire(tail); s = slot; store_release(head, h+1)
```

100 000 samples go across with order and per-slot consistency checked. When
the ring is full or empty the side spins with `sched_yield()`. A real design
blocks with a futex or eventfd after a few spins.

### 3.8 Pointers in shared memory  (`c/08_offsets_not_pointers.c`)

The program maps one object **twice in the same process** (two addresses)
and builds a linked list through mapping A, storing both a pointer and an
offset per link:

```
  mapping A @ 0x7f..ff000            mapping B @ 0x7f..ef000
  header.head_ptr = 0x7f..ff0f8 ──── points into A, NOT into B (wrong in B)
  header.head_off = 248         ──── B: base_B + 248 = correct node
```

Through B the pointer lands outside B. In a child that unmapped A it may
even land in the *new* mapping, if the kernel reuses the address. Then it
"works" by luck, which is the most dangerous kind of bug. Offsets always
work: `ptr = base + off`. The program also shows:

- a **header** (`magic`, `version`, `size`) checked before trusting the
  region. Version it like a file format;
- a **bump allocator** that returns aligned *offsets*. Misaligned atomics
  fault on ARM, and misaligned data is slow;
- **sizing**: whole pages, `ftruncate` big enough (`SIGBUS` otherwise);
- **cache lines**: offset % 64 == 0 ⇒ address % 64 == 0, because mappings
  are page-aligned.

C++ libraries wrap this as `boost::interprocess::offset_ptr<T>`.

### 3.9 The race you cannot see with TSan  (`c/09_shared_counter_bug.c`)

Two processes do `counter++` on a plain `volatile long` in shared memory.
About 40 % of the increments are lost (load–add–store interleaving), and the
program exits 1. **ThreadSanitizer reports nothing.** It instruments one
process at a time, and each process here has a single thread. Cross-process
races are caught by invariants (the final count), stress tests and review.
The fix in the same file, `atomic_fetch_add`, is exact. `volatile` is not
synchronisation (lesson 02).

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `shm_open`/`memfd_create`/`shmget` | one syscall | creates a tmpfs/shmem inode |
| `ftruncate` | O(1) | sets size; pages allocated lazily on first touch |
| `mmap` | O(1) syscall + VMA setup | page tables filled lazily by page faults |
| First touch of each page | one page fault (~1 µs) + zeroing | why big buffers are pre-faulted (`MAP_POPULATE`) in real-time code |
| Data transfer | **0 copies, 0 syscalls** | plain loads/stores; cost = cache-coherency traffic |
| Uncontended pshared mutex lock/unlock | ~20–50 ns, no syscall | futex fast path is one atomic |
| Contended wait/wake | syscall + scheduler wakeup (µs) | futex slow path |
| SPSC ring push/pop | O(1), wait-free | one acquire load + one release store |
| `msync(MS_SYNC)` | O(dirty pages) + I/O | only for durability of file mappings |
| Memory | size rounded up to pages | charged to tmpfs (`/dev/shm` size limit, default 50 % RAM) |

---

## 5. C vs C++

| Concern | C (`c/*.c`) | C++ (`cpp/01_shared_memory_raii.cpp`) |
|---|---|---|
| Setup/teardown | `shm_open`, `ftruncate`, `mmap`, `close`, `munmap`, `shm_unlink` by hand | `SharedMemory<T>::create(name)` / `::open(name)`, destructor cleans up; only the creator unlinks |
| Making an object | memory is just bytes; `memset`/init functions | **placement new** `new (mem) T{}`, explicit `obj->~T()` before unmap |
| Atomics | `_Atomic uint32_t`, `atomic_store_explicit` | `std::atomic<uint64_t>` members, `static_assert(is_always_lock_free)` |
| Layout checks | `_Static_assert(offsetof(...))`, `_Alignas(64)` | `static_assert(std::is_standard_layout_v<T>)`, `alignas(64)` |
| Size check on open | manual | `open()` verifies `st_size == sizeof(T)` |
| Library equivalent | — | Boost.Interprocess (`managed_shared_memory`, `offset_ptr`, `interprocess_mutex`, `message_queue`) |

Placement new matters: the bytes from `mmap` are not a `T` until a
constructor has run there. In standard C++ the creator constructs the object
once. Other processes *assume* it exists. That is formally outside the C++
object model (which knows nothing about processes), but it is how every real
system works, and it is safe for lock-free `std::atomic` and trivial types.
Never put `std::string`, `std::vector` or anything with a vtable in shared
memory. Their internal pointers are process-local.

---

## 6. Where it is used in the real world

- **GStreamer**: `shmsink` / `shmsrc` pass buffers between processes
  through a shared-memory area plus a Unix control socket (only the offsets
  of buffers travel over the socket). Zero-copy pipelines on embedded
  boards go further with **DMA-BUF** fds (`dmabuf` memory, `io-mode=dmabuf`
  on `v4l2src`), so camera, GPU, encoder and display hardware all access
  the same buffer. See
  [gstreamer/08](../../gstreamer/08-queues-threads-tee/README.md) for
  in-process queues and tee.
- **Camera frames on the TDA4VM**: the CSI-2 capture driver fills DMA-BUF
  buffers. The TIOVX/OpenVX graph on the C7x DSP and the VPAC/ISP read the
  same physical memory (reserved-memory carve-outs and CMA declared in the
  device tree). User-space
  processes share those buffers by passing the DMA-BUF **fd** (lesson 12),
  never by copying.
- **Wayland**: clients render into `memfd`/`shm` buffers (`wl_shm`) or
  DMA-BUF and pass the fd to the compositor. X11 used SysV shm (MIT-SHM).
- **Android**: ashmem (now memfd) + binder for passing the fds. Gralloc
  buffers are DMA-BUF.
- **Linux kernel**: `mm/shmem.c` (tmpfs, `shm_open`, `memfd`, SysV shm all
  end up here), `ipc/shm.c`, the futex hash keyed by physical page for
  shared futexes (`kernel/futex/`), robust-list handling in
  `exit_robust_list()`. **io_uring**: SQ/CQ rings shared between user space
  and kernel, SPSC with acquire/release.
- **glibc**: `shm_open` is literally `open("/dev/shm/…")`. pshared/robust
  mutex support lives in `nptl/`.
- **Audio**: PipeWire and JACK move audio between processes through
  shared-memory ring buffers with eventfd wake-ups. PulseAudio uses memfd
  "SHM pools".
- **Databases**: PostgreSQL's shared buffers (SysV shm historically,
  `mmap` anonymous + small SysV segment since 9.3). Redis/LMDB use file
  `mmap`.
- **Heterogeneous SoCs**: on the TDA4VM, Linux on the A72 and an RTOS on
  the R5F share carve-out memory described in the device tree.
  **RPMsg/virtio** vrings in that memory are SPSC rings exactly like `c/07`.
  On FreeRTOS-only systems all tasks share one address space, so a "shared
  buffer" is just a global plus a mutex or stream buffer. The discipline is
  the same.

---

## 7. Common mistakes and pitfalls

1. **No synchronisation**: plain loads/stores from two processes = data
   race, lost updates (`c/09`). Use a pshared mutex, or atomics with the
   right ordering.
2. **Expecting TSan to catch it**: TSan and Helgrind are per process. For
   cross-process code, write invariant checks and stress tests. (Each
   program still runs under TSan for its in-process behaviour:
   `make run P=linux-internals/11-ipc-shared-memory/c/07_shm_spsc_ring.c SAN=tsan`.
   Fork before creating threads, because TSan does not support fork from a
   multi-threaded process.)
3. **Default (process-private) mutex in shared memory**: may appear to work
   and then fail. Always set `PTHREAD_PROCESS_SHARED`.
4. **Non-robust mutex + crashing process** = everyone hangs. Use robust
   mutexes and actually **repair** the data on `EOWNERDEAD`.
5. **Pointers in shared memory**: different mappings = different addresses.
   Store offsets (`c/08`).
6. **Forgetting `ftruncate`** → `SIGBUS` on first access. Shrinking an object
   another process has mapped → `SIGBUS` there. Use seals.
7. **Not unlinking**: `/dev/shm` fills with stale objects (RAM!) after
   crashes. Use unique names with the pid, `O_EXCL`, cleanup on exit, or
   `memfd` (nothing to unlink).
8. **Initialisation races**: the second process maps the region while the
   first is still initialising it. Initialise before publishing the name
   (create under a temporary name and `rename`, or set a `ready` flag with
   release semantics last).
9. **Non-lock-free atomics** across processes (e.g. 16-byte structs):
   silently not atomic between processes.
10. **False sharing**: producer and consumer indices in one cache line ping-pong
    the line between cores. Align hot fields to 64 B.
11. **`std::string`/`std::vector`/virtual classes in shared memory**: they
    contain process-local pointers.
12. **RAII owner objects and fork**: a child returning normally runs the
    owner's destructor and unlinks the parent's object. Use `_exit()` in
    children.
13. **Confusing `msync` with visibility**: other processes see stores
    immediately (subject to memory ordering). `msync` is only for writing
    file mappings to storage.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_posix_shm.c](c/01_posix_shm.c) | `shm_open`, `ftruncate`, `mmap(MAP_SHARED)`, `/dev/shm`, inherited vs re-opened mapping (different address), `shm_unlink` lifetime | `make run P=linux-internals/11-ipc-shared-memory/c/01_posix_shm.c` |
| [c/02_anon_and_file_mmap.c](c/02_anon_and_file_mmap.c) | `MAP_SHARED\|MAP_ANONYMOUS` before fork vs `MAP_PRIVATE` copy-on-write; file `mmap`, `msync`, page-cache coherence with `pread`/`pwrite` | `make run P=linux-internals/11-ipc-shared-memory/c/02_anon_and_file_mmap.c` |
| [c/03_memfd_create.c](c/03_memfd_create.c) | `memfd_create`, a "camera frame" shared with a child, seals (`EPERM` on write/shrink), receiver checks seals | `make run P=linux-internals/11-ipc-shared-memory/c/03_memfd_create.c` |
| [c/04_sysv_shm.c](c/04_sysv_shm.c) | `ftok`, `shmget`, `shmat`/`shmdt`, `shm_nattch`, `IPC_RMID` marks then destroys at last detach | `make run P=linux-internals/11-ipc-shared-memory/c/04_sysv_shm.c` |
| [c/05_pshared_mutex_condvar_sem.c](c/05_pshared_mutex_condvar_sem.c) | pshared mutex + 2 condvars bounded buffer between processes, monotonic timed waits, `sem_init(pshared=1)` ping-pong | `make run P=linux-internals/11-ipc-shared-memory/c/05_pshared_mutex_condvar_sem.c` |
| [c/06_robust_mutex.c](c/06_robust_mutex.c) | owner killed with SIGKILL: stalled mutex vs robust → `EOWNERDEAD` → repair → `pthread_mutex_consistent`; `ENOTRECOVERABLE` | `make run P=linux-internals/11-ipc-shared-memory/c/06_robust_mutex.c` |
| [c/07_shm_spsc_ring.c](c/07_shm_spsc_ring.c) | lock-free SPSC ring in `shm_open` memory between two processes, C11 acquire/release, cache-line layout asserts | `make run P=linux-internals/11-ipc-shared-memory/c/07_shm_spsc_ring.c` |
| [c/08_offsets_not_pointers.c](c/08_offsets_not_pointers.c) | same object mapped twice: pointers break, offsets work; header with magic/version, aligned bump allocator | `make run P=linux-internals/11-ipc-shared-memory/c/08_offsets_not_pointers.c` |
| [c/09_shared_counter_bug.c](c/09_shared_counter_bug.c) | **bug**: unsynchronised counter across processes loses ~40 % of updates; TSan is blind; the self-check catches it (exit 1); atomic fix | `make build/linux-internals/11-ipc-shared-memory/c/09_shared_counter_bug && ./build/linux-internals/11-ipc-shared-memory/c/09_shared_counter_bug; echo $?` |
| [cpp/01_shared_memory_raii.cpp](cpp/01_shared_memory_raii.cpp) | `SharedMemory<T>` RAII template, placement new, `std::atomic` counters from 4 processes, release/acquire ready flag | `make run P=linux-internals/11-ipc-shared-memory/cpp/01_shared_memory_raii.cpp` |

All non-`_bug` programs also run clean under ThreadSanitizer, e.g.
`make run P=linux-internals/11-ipc-shared-memory/c/05_pshared_mutex_condvar_sem.c SAN=tsan`.

---

## 9. Exercises

**Easy**
1. Comment out `shm_unlink` in `c/01`, run it, and find the leftover in
   `ls -l /dev/shm`. Remove it with `rm`. What does it cost while it exists?
2. In `c/02`, replace the anonymous `MAP_SHARED` mapping with `malloc` and
   explain the result.
3. Run `c/09_shared_counter_bug` five times and record the loss. Then build
   it with `make OPT=1` and compare. Why does `volatile` make no
   difference?

**Medium**
4. Give the SPSC ring in `c/07` a blocking mode: after 100 empty spins the
   consumer waits on an `eventfd` (or a pshared semaphore) that the producer
   signals only when the ring goes from empty to non-empty.
5. Split `c/05` into two real programs (producer and consumer) that use
   `shm_open` with a well-known name. Handle "who initialises?" with
   `O_CREAT | O_EXCL` and a `ready` flag stored with release semantics.
6. Extend `c/06` with a journal: before the transfer, store `pending = 30`.
   On `EOWNERDEAD`, use it to decide whether to roll forward or back.

**Hard**
7. Build a **frame pool** for a camera: shared memory with N 1080p NV12
   slots plus an SPSC ring of slot *indices* in each direction (free list
   and ready list). Producer and consumer never copy a frame. This is the
   design of `shmsink`/`shmsrc` and V4L2 buffer queues.
8. Pass a sealed `memfd` to an **unrelated** process with `SCM_RIGHTS`
   (lesson 12), and have the receiver verify the seals before mapping.
9. Cross-compile `c/07` for the BeagleBone AI-64 and run it. Then change
   the producer's `tail` store to `memory_order_relaxed` and run long
   stress tests with a large payload. Try to observe a torn slot on the
   weakly ordered A72 (it may take a while, or never show up on x86).

---

## 10. Remember forever

```
shm = same PHYSICAL pages in 2 processes: 0 copies, 0 syscalls -> YOU synchronise
shm_open("/n", O_CREAT|O_EXCL|O_RDWR) -> ftruncate (else SIGBUS) -> mmap(MAP_SHARED) -> close fd
... munmap -> shm_unlink (else leaks in /dev/shm).  anon MAP_SHARED before fork = no name
MAP_PRIVATE / malloc after fork = copy-on-write = NOT shared.  file mmap: msync = durability only
memfd_create: fd only, auto-freed, seals (SHRINK/GROW/WRITE) the receiver can verify; pass via SCM_RIGHTS
sync IN the shm: mutexattr/condattr_setpshared(PROCESS_SHARED), sem_init(.., 1, ..)
robust mutex: EOWNERDEAD -> repair -> pthread_mutex_consistent -> unlock (forget = ENOTRECOVERABLE)
lock-free atomics only; OFFSETS not pointers; 64-B aligned hot fields; header magic+version
TSan cannot see other processes: check invariants
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why is shared memory the fastest IPC? What exactly is *not* happening
   that happens with a pipe?
2. List the steps to create, use and destroy a POSIX shared-memory object.
   What happens if you skip `ftruncate`? And `shm_unlink`?
3. A parent `malloc`s a buffer, forks, and the child writes into it. Why
   does the parent not see the data? Which mapping flags fix it?
4. When do you need `msync`, and when do you not?
5. What does `memfd_create` give you over `shm_open`? What problem do seals
   solve, and who checks them?
6. Why must a mutex in shared memory be `PTHREAD_PROCESS_SHARED`? How can a
   futex work across processes when the virtual addresses differ?
7. A process is `kill -9`ed while holding a mutex in shared memory. What
   happens with a normal mutex, and what with a robust one? What must the
   next owner do?
8. Why must atomics used across processes be lock-free?
9. Why can't you store pointers in shared memory? Describe the offset
   technique.
10. Why did `c/09` lose updates even though `counter` is `volatile`? Why did
    ThreadSanitizer stay silent?
11. Sketch how a camera process on the TDA4VM can hand frames to an
    inference process without copying them. Which pieces carry the data,
    and which carry the notifications?
12. In the SPSC ring, which side writes `head` and which writes `tail`? Why
    must the producer's `tail` store be a release store?
