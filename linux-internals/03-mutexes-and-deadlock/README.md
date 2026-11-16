# 03 — Mutexes and Deadlock

> A **mutex** (MUTual EXclusion lock) lets exactly one thread at a time into
> a piece of code that touches shared data. It is the default tool for
> protecting shared state, in user space and in the kernel. Most locking
> bugs fall into two groups: forgetting the lock (a race, lesson 02) and
> taking locks in the wrong order (a **deadlock**, this lesson).

---

## 1. Why this exists

Lesson [02](../02-race-conditions-and-atomics/README.md) showed that
`counter++` from two threads loses updates, and that atomics fix *one*
variable. Real shared state is bigger than one variable: a linked list plus
its length, a ring buffer's `head`, `tail` and slots, a bank account pair
during a transfer. These have **invariants** (`len` equals the number of
nodes, `a + b == 100`) that are broken for a moment in the middle of every
update. A mutex makes the whole update look instant to every other thread
that uses the same mutex.

| You need... | Use |
|---|---|
| protect a multi-step update of shared data | **mutex** (this lesson) |
| one counter or flag | atomic ([02](../02-race-conditions-and-atomics/README.md)) |
| wait until some condition becomes true | mutex **+ condition variable** ([04](../04-condition-variables/README.md)) |
| count free resources, signal from an ISR or signal handler | semaphore ([05](../05-semaphores/README.md)) |
| a lock held for a few dozen instructions, never sleeping | spinlock ([06](../06-spinlocks-and-futex/README.md)) |
| many readers, rare writers | rwlock / seqlock / RCU ([07](../07-rwlocks-seqlocks-and-rcu/README.md)) |

---

## 2. Mental model

**A mutex is the single key to a one-person bathroom.** The person who takes
the key *owns* it. Others queue at the door (they sleep, they do not keep
trying the handle). Only the owner gives the key back. If the owner leaves
the building with the key, everyone waits forever. That last case is what
**robust mutexes** handle.

```
              critical section (one thread at a time)
             +-------------------------------------+
  T1 ---lock-+-> read list, modify, fix len  -unlock+--->
             +-------------------------------------+
  T2 ---lock-X  (sleeps in the kernel: futex wait)   wakes, owns it ---->
  T3 ---lock-X  (sleeps)                                     later ...

  pthread_mutex_t on glibc (40 B on x86-64, 48 B on aarch64) is mostly ONE int:
     0 = unlocked
     1 = locked, nobody waiting        <- lock/unlock = 1 atomic op, no syscall
     2 = locked, maybe waiters         <- unlock must call futex(FUTEX_WAKE)
  plus owner tid, type, recursion count, robust-list links.
```

The fast path (no contention) never enters the kernel. Only a thread that
must wait calls `futex(FUTEX_WAIT)` and sleeps. Lesson
[06](../06-spinlocks-and-futex/README.md) builds such a mutex from scratch.

Remember this sentence: **lock = acquire, unlock = release.** Everything
the owner wrote before `unlock()` is visible to the next owner after its
`lock()`, even on the weakly ordered Cortex-A72. So plain variables are safe
*inside* the lock, and you need no `volatile` and no extra barriers.

---

## 3. How it works

### 3.1 Critical section, mutual exclusion, ownership (`c/01_mutex_basics.c`)

```c
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;   /* static init  */
pthread_mutex_lock(&m);      /* blocks until we own it                 */
shared++;                    /* critical section: short, no blocking I/O */
pthread_mutex_unlock(&m);    /* only the owner may do this             */
```

- **Static init** with `PTHREAD_MUTEX_INITIALIZER` for globals; **dynamic
  init** with `pthread_mutex_init(&m, &attr)` for mutexes inside heap objects
  or with attributes. Pair every `init` with `destroy`. Never destroy a
  locked mutex, and never copy a `pthread_mutex_t` (`memcpy` of a struct that
  contains one is a bug).
- Every function returns an **error number** (0 = ok). It does not set `errno`.

### 3.2 trylock and timedlock

```
 pthread_mutex_trylock(&m)            -> 0 or EBUSY immediately
 pthread_mutex_timedlock(&m, &abs)    -> 0 or ETIMEDOUT at ABSOLUTE time abs
 pthread_mutex_clocklock(&m, CLOCK_MONOTONIC, &abs)   (glibc >= 2.30)
```

`timedlock`'s deadline is on **CLOCK_REALTIME**, the wall clock. On an
embedded board that boots at 1970-01-01 and then receives NTP time, the clock
jumps forward by 50+ years: every pending timed wait expires at once.
A clock stepped backwards makes them wait far too long. Use
`pthread_mutex_clocklock(..., CLOCK_MONOTONIC, ...)` when available (guard
with `__GLIBC_PREREQ(2, 30)`), or the C++ `try_lock_for`.

```
 deadline = now + 100 ms          REALTIME:  ----|---- NTP step +3 h ---> already expired!
                                  MONOTONIC: ----|----------------------> fires after 100 ms
```

### 3.3 RAII in C: `__attribute__((cleanup))`

The most common mutex bug is an early `return` (or `goto err`) that skips
the unlock. GCC and Clang can run a function when a variable leaves scope:

```c
static void unlock_cleanup(pthread_mutex_t **pm) { pthread_mutex_unlock(*pm); }
#define LOCK_GUARD(m) \
    pthread_mutex_t *lock_guard_ __attribute__((cleanup(unlock_cleanup))) = \
        (pthread_mutex_lock(m), (m))
```

The Linux kernel does the same since 6.5 (`include/linux/cleanup.h`):
`guard(mutex)(&dev->lock);` and `scoped_guard(spinlock_irqsave, &l) { ... }`.
In C++ this is simply `std::lock_guard`.

### 3.4 Mutex types (`c/02_mutex_types.c`)

| Type | Owner relocks | Non-owner unlocks | Unlock when unlocked | Use |
|---|---|---|---|---|
| `NORMAL` | **deadlocks itself, silently** | undefined | undefined | fastest |
| `ERRORCHECK` | returns `EDEADLK` | returns `EPERM` | returns `EPERM` | debug builds |
| `RECURSIVE` | count++ (needs as many unlocks) | `EPERM` | `EPERM` | legacy re-entrant APIs |
| `DEFAULT` | undefined (glibc: = NORMAL) | undefined | undefined | what you get by default |

```
 RECURSIVE: lock lock lock unlock unlock unlock
   count:     1    2    3     2      1      0   <- only now can others get it
```

A recursive mutex usually hides a design problem: you no longer know which
invariants hold when you enter a function. It also breaks condition
variables, because `wait()` releases only *one* level. Prefer splitting into
`foo()` (locks) and `foo_locked()` (expects the caller to hold the lock). The
kernel does this with `lockdep_assert_held(&lock)`.

### 3.5 Robust mutexes (`c/03_robust_mutex.c`)

If the owner dies while holding a normal mutex, everybody waits forever.
Between threads this is rare (a crashing thread usually kills the process).
Between **processes** sharing memory it is a real risk: a camera daemon
crashes while holding the lock in a shared frame buffer. A **robust** mutex
fixes that:

```
 owner:  lock(m) ... half-updated data ... CRASH
 kernel: walks the dead thread's robust list (set_robust_list), marks m
         FUTEX_OWNER_DIED, wakes one waiter
 next:   lock(m) == EOWNERDEAD   -> you OWN m; the data may be inconsistent
         repair the data
         pthread_mutex_consistent(&m)
         unlock(m)               -> back to normal
 forget consistent() and unlock  -> every later lock() == ENOTRECOVERABLE
```

`pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST)`. The program uses a
thread that exits while holding the lock. The cross-process version (mutex in
`shm_open` memory with `PTHREAD_PROCESS_SHARED`, a child that is killed) is in
[11-ipc-shared-memory](../11-ipc-shared-memory/README.md).

### 3.6 Priority inversion (`c/04_priority_inversion.c`)

With fixed priorities (SCHED_FIFO, every RTOS), a mutex can make a
high-priority task wait for a *medium* one that does not even use the lock:

```
 prio
  H  .  .  H  x  x  x  x  x  x  .  .  .  H      x = H blocked on the mutex
  M  .  .  .  M  M  M  M  M  M  .  .  .  .      M preempts L, so L cannot unlock
  L  L  L  .  .  .  .  .  .  .  L  L  L  .
     0  1  2  3  4  5  6  7  8  9 10 11 12      H waits 11 ticks: UNBOUNDED,
                                                 it grows with M's run time
 with priority INHERITANCE (L runs at H's prio while H waits):
  H  .  .  H  x  x  x  H  .  .  .  .  .  .      H waits 5 ticks: bounded by
  M  .  .  .  .  .  .  .  M  M  M  M  M  M      the critical section length
  L  L  L  .  L  L  L  .  .  .  .  .  .  .
```

**Mars Pathfinder, 1997.** On Mars, the lander kept resetting itself. A
high-priority bus-management task shared a VxWorks mutex with a
low-priority meteorological task. A medium-priority communications task
preempted the low one while it held the mutex. The bus task missed its
deadline and the watchdog rebooted the system. JPL reproduced the bug on a
ground copy, then uploaded a fix that turned on priority inheritance for
that one mutex.

Two protocols, both set as mutex attributes:

| Protocol | Attribute | How | Cost |
|---|---|---|---|
| Inheritance | `PTHREAD_PRIO_INHERIT` | while a higher-priority thread waits, the owner runs at that priority | Linux: PI futex, the kernel's `rt_mutex` boosts the owner (syscall on contention) |
| Ceiling (protect) | `PTHREAD_PRIO_PROTECT` + `setprioceiling(n)` | the owner *always* runs at ceiling `n` while holding the lock | a priority change on every lock/unlock; ceiling must be ≥ every user's priority |

The program prints the three timelines (none, inheritance, ceiling) from a
deterministic scheduler simulation. It also sets and reads back both
attributes (no root needed) and tries a real `SCHED_FIFO` thread. As a normal
user that gives `EPERM`, which the program reports and skips. On the board,
run it with `sudo` or raise `rtprio` in `/etc/security/limits.conf`. Priority
only matters for real-time threads: under `SCHED_OTHER` (CFS/EEVDF) every
thread gets CPU time eventually, so the inversion is bounded (but still
causes latency spikes).

### 3.7 Lock granularity (`c/05_lock_granularity.c`)

```
 COARSE: 1 lock for the table      STRIPED: S locks, bucket b -> lock b % S      FINE: lock per bucket
 simple, correct, serialises       good trade-off, fixed memory                  most parallel, most memory
```

- **Too coarse:** all threads queue on one lock, and the lock's cache line
  bounces between cores. Throughput stays at one core's speed or gets worse.
- **Too fine:** more memory, more lock/unlock overhead per operation, and
  operations that touch several buckets now need several locks, which brings
  ordering problems.
- **Striping** (`locks[hash % S]`) is the usual compromise. Whole-table
  operations (resize, snapshot) take *all* stripes in index order.

Make it correct with one lock first. Measure. Split only the lock that shows
up in `perf lock` / `perf c2c` / mutrace.

### 3.8 Deadlock (`c/07_deadlock_abba_bug.c`)

```
     thread 1                 thread 2
     lock(A)                  lock(B)
     lock(B) ---waits---+     lock(A) ---waits---+
              ^         |              ^          |
              +---------+--- held by --+          |
                        +----------- held by -----+     ABBA: a cycle
```

**The four Coffman conditions (1971).** A deadlock can happen only if all
four hold:

1. **Mutual exclusion**: a resource has one owner. (This is what a mutex is for.)
2. **Hold and wait**: a thread holds A while it waits for B.
3. **No preemption**: nobody can take A away from its owner.
4. **Circular wait**: T1 waits for T2, which waits for T1.

Break any one and deadlock is impossible. In practice you break **4**
(lock ordering) or **2** (try-lock and back off, or take everything at once).

Related failures:

- **Self-deadlock:** a NORMAL mutex locked twice by the same thread, e.g.
  through a callback. ERRORCHECK reports it as `EDEADLK`.
- **Livelock:** threads are not blocked, but they keep reacting to each
  other and make no progress. Two try-lock-and-back-off threads that retry
  in lockstep do this, like two people stepping aside in a corridor again and
  again. Fix it with randomised, growing back-off.
- **Starvation:** progress happens, just never for *one* thread. Unfair locks
  let a thread that just unlocked re-grab the lock before the woken waiter
  runs. Low priorities under SCHED_FIFO never run. Ticket locks
  ([06](../06-spinlocks-and-futex/README.md)) and FIFO wait queues are fair.

### 3.9 Avoiding deadlock (`c/06_deadlock_avoidance.c`, `cpp/02_hierarchical_mutex.cpp`)

| Strategy | Breaks | How | Used in |
|---|---|---|---|
| Global lock order (by id / level) | circular wait | document "A before B"; lower id first | kernel: `mm/rmap.c` and `fs/inode.c` header comments list lock orders |
| Address order | circular wait | `(uintptr_t)a < (uintptr_t)b` first | kernel `double_rq_lock()`, `lock_two_nondirectories()` |
| Try-lock + back-off | hold and wait | lock one, trylock others, else release all, pause, retry | `std::lock`/`std::scoped_lock` internally |
| Lock hierarchy check | detects violations | thread-local "current level", throw on upward lock | kernel **lockdep**, `cpp/02` |
| One lock for both | (no nesting at all) | coarser lock | early-stage code |
| Never call unknown code with a lock held | hidden nesting | callbacks, `printf` to a pipe, signal handlers | GStreamer: no pad push while holding the object lock |

### 3.10 Mutex vs spinlock vs semaphore (short version)

| | Mutex | Spinlock | Semaphore |
|---|---|---|---|
| Waiting | sleeps (futex) | busy-loops on the CPU | sleeps |
| Owner | yes: only the locker unlocks | yes | **no**: any thread may post |
| Count | 1 | 1 | N |
| Priority inheritance | possible (`PRIO_INHERIT`) | n/a | **no** |
| Use for | protecting data | very short sections, no sleeping, multi-core | counting resources, signalling events |
| Lesson | this one | [06](../06-spinlocks-and-futex/README.md) | [05](../05-semaphores/README.md) |

---

## 4. Complexity table

| Operation | Cost (uncontended) | Cost (contended) | Why |
|---|---|---|---|
| `pthread_mutex_lock` / `unlock` | ~20–50 ns, one atomic RMW, no syscall | futex syscall + context switch, µs | fast path in user space, slow path sleeps in the kernel |
| `trylock` | one atomic | one atomic (returns EBUSY) | never sleeps |
| `timedlock` | as lock | sleeps up to the deadline | futex wait with timeout |
| ERRORCHECK / RECURSIVE | + owner tid compare | same | extra bookkeeping |
| PI mutex | one atomic (tid in futex word) | `FUTEX_LOCK_PI` syscall, rt_mutex chain walk | kernel boosts the owner |
| PROTECT mutex | **2 syscalls** (raise/restore prio) | same | priority changes even without contention |
| Lock S stripes for a snapshot | O(S) | O(S) | take every stripe, in order |
| Deadlock detection (lockdep, TSan) | O(edges) per new lock pair | — | graph of "held → acquired" edges |
| Memory | `pthread_mutex_t`: 40 B on x86-64, 48 B on aarch64 glibc | — | futex word + owner + kind + count + robust links |

---

## 5. C vs C++

| Concept | C (pthreads) | C++17 |
|---|---|---|
| Mutex | `pthread_mutex_t` + attrs | `std::mutex` (no attributes; `native_handle()` if you need them) |
| Scope lock | `__attribute__((cleanup))` macro | `std::lock_guard<std::mutex> g(m);` |
| Flexible lock | manual lock/unlock | `std::unique_lock` — `defer_lock`, `try_to_lock`, `adopt_lock`, movable, `unlock()` early |
| Two or more locks | manual ordering | `std::scoped_lock lk(a, b);` or `std::lock(ua, ub);` (deadlock-avoiding) |
| trylock | `pthread_mutex_trylock` | `m.try_lock()`, `std::try_lock(a, b)` returns −1 or busy index |
| Timed | `pthread_mutex_timedlock` (REALTIME) / `clocklock` | `std::timed_mutex::try_lock_for/until` |
| Recursive | `PTHREAD_MUTEX_RECURSIVE` | `std::recursive_mutex` |
| Error-check, robust, PI | attributes | not in the standard: use pthreads |
| Hierarchy check | — | custom Lockable class (`cpp/02`) works with every `std::` lock holder |

`std::mutex` on Linux is a thin wrapper around a default (`NORMAL`)
`pthread_mutex_t`. `std::scoped_lock` with one mutex behaves like
`lock_guard`. Always give the variable a name: `std::scoped_lock(m);` without
a name creates a temporary that unlocks at the semicolon, and with only a
mutex argument it can even declare a new variable called `m`.

---

## 6. Where it is used in the real world

- **Linux kernel.** `struct mutex` (`kernel/locking/mutex.c`) sleeps,
  has an owner, spins optimistically while the owner is running on another
  CPU, and must not be used in interrupt context. `rt_mutex` adds priority
  inheritance and backs user-space PI futexes. With `PREEMPT_RT` (the
  real-time patch set, merged in 6.12) most spinlocks *become* rt_mutexes.
  **lockdep** (`CONFIG_PROVE_LOCKING`) checks lock order for every lock
  class and prints "possible circular locking dependency" on the first
  inversion, even if no deadlock happened. See
  [linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md).
- **glibc.** `pthread_mutex_lock` is in `nptl/pthread_mutex_lock.c`: a CAS
  fast path, then `futex_wait`. `malloc` arenas each have a mutex (a form of
  lock striping: threads are spread over several arenas). `stdio` FILE
  streams have a recursive lock (`flockfile`).
- **GStreamer.** Every `GstObject` has `GST_OBJECT_LOCK`. `GstPad` has the
  stream lock (`GST_PAD_STREAM_LOCK`, a recursive mutex). The documented rule
  "never push a buffer downstream while holding the object lock" exists to
  avoid ABBA deadlocks with the streaming thread. See
  [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md).
- **Databases.** Lock striping in hash tables, lock ordering on B-tree pages
  (top-down "lock coupling"). Deadlock *detection* with a wait-for graph:
  PostgreSQL runs `deadlock_timeout` and aborts one transaction.
- **Embedded/RTOS.** FreeRTOS `xSemaphoreCreateMutex()` gives a mutex
  with priority inheritance (a binary semaphore does not have it, see
  [05](../05-semaphores/README.md)). Zephyr `k_mutex` is recursive with
  priority inheritance. VxWorks `semMCreate(SEM_INVERSION_SAFE)` is the
  option Pathfinder was missing. AUTOSAR OS uses the priority-ceiling
  protocol (`GetResource`).
- **TDA4VM board.** The A72 cores run Linux, so these rules apply directly.
  The R5F cores run FreeRTOS/SafeRTOS, which uses the RTOS mutex. A
  hardware spinlock block (`hwspinlock`) handles locking *between* the A72
  and the R5F.

---

## 7. Common mistakes and pitfalls

1. **Forgetting to unlock on an error path.** Use a guard (C++ RAII, or
   `cleanup` in C), or a single `goto out_unlock` exit.
2. **Protecting the writer but not the reader.** Every access to shared
   data, reads included, must hold the same lock. TSan finds this.
3. **Different locks for the same data.** Two mutexes "protecting" one
   variable protect nothing. Write the rule next to the data:
   `/* guarded by dev->lock */`.
4. **Holding a lock across blocking I/O, `sleep`, or a callback.** This
   adds latency for everyone, and if the callback takes another lock you have
   hidden lock nesting and a deadlock risk.
5. **ABBA ordering.** Define one order, document it, check it (lockdep,
   TSan, `cpp/02`). Run `make run P=linux-internals/03-mutexes-and-deadlock/c/07_deadlock_abba_bug.c SAN=tsan`
   to see the report.
6. **Relocking a NORMAL mutex** (a callback that calls back into the
   locked API). It hangs silently. Use ERRORCHECK in debug builds.
7. **Unlocking from another thread.** Undefined for NORMAL. If you need
   "thread A takes, thread B releases", you want a semaphore.
8. **`timedlock` on CLOCK_REALTIME** on a board whose clock is set by NTP
   after boot. Use `clocklock` with `CLOCK_MONOTONIC`.
9. **Copying, moving or `memset`ting a locked mutex**, or destroying it while
   locked or while threads wait on it.
10. **Robust mutex without `pthread_mutex_consistent()`.** The mutex is then
    `ENOTRECOVERABLE` forever.
11. **Assuming priority inheritance helps SCHED_OTHER threads.** It matters
    for real-time policies. Also, a semaphore never has it.
12. **Unnamed temporaries:** `std::lock_guard<std::mutex>(m);` locks and
    unlocks immediately, or declares a variable `m`. Always name the guard.
13. **Running TSan on code that uses glibc's newer `*_clocklock` calls.**
    Older TSan runtimes (GCC 11) do not intercept `pthread_mutex_clocklock`,
    which libstdc++ uses for `steady_clock` deadlines. A successful one is
    invisible to TSan, and the later unlock is reported as "unlock of an
    unlocked mutex". `cpp/01` shows the workaround.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_mutex_basics.c](c/01_mutex_basics.c) | critical section with 4 threads, static/dynamic init, trylock `EBUSY`, timedlock `ETIMEDOUT` (REALTIME caveat), `clocklock` (guarded), cleanup-attribute guard | `make run P=linux-internals/03-mutexes-and-deadlock/c/01_mutex_basics.c` |
| [c/02_mutex_types.c](c/02_mutex_types.c) | NORMAL / ERRORCHECK (`EDEADLK`, `EPERM`) / RECURSIVE (count, ownership) / DEFAULT | `make run P=linux-internals/03-mutexes-and-deadlock/c/02_mutex_types.c` |
| [c/03_robust_mutex.c](c/03_robust_mutex.c) | owner thread exits holding the lock → `EOWNERDEAD`, repair, `pthread_mutex_consistent`; forgetting it → `ENOTRECOVERABLE` | `make run P=linux-internals/03-mutexes-and-deadlock/c/03_robust_mutex.c` |
| [c/04_priority_inversion.c](c/04_priority_inversion.c) | scheduler simulation (none / inheritance / ceiling timelines), `PRIO_INHERIT` / `PRIO_PROTECT` attributes read back, `SCHED_FIFO` with EPERM skip | `make run P=linux-internals/03-mutexes-and-deadlock/c/04_priority_inversion.c` |
| [c/05_lock_granularity.c](c/05_lock_granularity.c) | same hash-table workload with 1 lock, 8 stripes, 64 locks; exact per-bucket check; all-stripes snapshot | `make run P=linux-internals/03-mutexes-and-deadlock/c/05_lock_granularity.c` |
| [c/06_deadlock_avoidance.c](c/06_deadlock_avoidance.c) | bank transfers: order by id, order by address, trylock + randomised back-off; money conserved | `make run P=linux-internals/03-mutexes-and-deadlock/c/06_deadlock_avoidance.c` |
| [c/07_deadlock_abba_bug.c](c/07_deadlock_abba_bug.c) | **BUG**: ABBA deadlock, hangs every run; TSan reports `lock-order-inversion` | `TSAN_OPTIONS=halt_on_error=1 timeout 5 make run P=linux-internals/03-mutexes-and-deadlock/c/07_deadlock_abba_bug.c SAN=tsan` |
| [cpp/01_std_mutex_raii.cpp](cpp/01_std_mutex_raii.cpp) | `lock_guard`, `unique_lock` (defer/try/adopt/move), `scoped_lock` and `std::lock` for opposite-order transfers, `std::try_lock`, `recursive_mutex`, `timed_mutex` | `make run P=linux-internals/03-mutexes-and-deadlock/cpp/01_std_mutex_raii.cpp` |
| [cpp/02_hierarchical_mutex.cpp](cpp/02_hierarchical_mutex.cpp) | a Lockable that throws when locked "upwards": lockdep in 60 lines | `make run P=linux-internals/03-mutexes-and-deadlock/cpp/02_hierarchical_mutex.cpp` |

Every non-bug program is also clean under ThreadSanitizer. Add `SAN=tsan`:

```bash
make run P=linux-internals/03-mutexes-and-deadlock/c/06_deadlock_avoidance.c SAN=tsan
```

The bug program without tools just hangs. `timeout 5 make run P=...` exits
with status 124. `gdb -p <pid>` then `thread apply all bt` shows both threads
in `__lll_lock_wait`.

---

## 9. Exercises

**Easy**
1. In `01`, remove the lock from `count_worker` and run with `SAN=tsan`.
   Read the report: which two accesses, which threads?
2. In `02`, change `demo_normal` to call `pthread_mutex_lock` twice. Run it
   with `timeout 3` and explain the exit code.
3. Add an `ERRORCHECK` mode switch to `01` controlled by `#ifdef DEBUG`.

**Medium**
4. Extend `05` with a `resize()` that doubles the bucket count while holding
   all stripes. Why must it take the stripes in index order?
5. In `06`, make the back-off deterministic (no randomness, fixed pause).
   Can you make two threads livelock? Count the retries.
6. Write `transfer3(a, b, c)` that locks three accounts with address
   ordering (sort three pointers). Then do it with `std::scoped_lock`.
7. Put the `Shared` struct from `03` in `mmap(MAP_SHARED|MAP_ANONYMOUS)`
   memory, set `PTHREAD_PROCESS_SHARED`, `fork()`, and let the child
   `_exit()` while it holds the lock. (This is the
   [11](../11-ipc-shared-memory/README.md) version.)

**Hard**
8. On the BeagleBone AI-64 as root: pin three `SCHED_FIFO` threads (prio
   10/20/30) to CPU 1 with `pthread_setaffinity_np`, and reproduce the
   Pathfinder inversion with a real mutex. Measure H's latency with and
   without `PTHREAD_PRIO_INHERIT`.
9. Implement a tiny "lockdep": record every (held → acquired) pair in a
   global graph keyed by mutex address, and report a cycle the first time
   an edge closes one. Test it on `07` without TSan.
10. Read `kernel/locking/mutex.c`. What is "optimistic spinning", and why
    does it help when the owner is running on another CPU?

---

## 10. Remember forever

```
lock = acquire, unlock = release; only the OWNER unlocks; same data -> same lock
trylock: EBUSY now. timedlock: ABSOLUTE deadline on REALTIME -> prefer clocklock(MONOTONIC)
NORMAL relock = silent self-deadlock; ERRORCHECK -> EDEADLK/EPERM; RECURSIVE = count
ROBUST: EOWNERDEAD -> repair -> pthread_mutex_consistent -> unlock (else ENOTRECOVERABLE)
Inversion: H waits on L, M preempts L.  Fix: PRIO_INHERIT (boost owner) / PRIO_PROTECT (ceiling)
Deadlock needs all 4: mutual exclusion, hold&wait, no preemption, circular wait
Fix: one global order (id/address/level) | trylock+random back-off | scoped_lock(a, b)
Coarse first, measure, then stripe. Never call unknown code while holding a lock.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What does "the mutex makes the update look atomic" mean if the CPU still
   executes several instructions? What does it guarantee about memory
   visibility on ARM?
2. Why does `pthread_mutex_timedlock` take an absolute time, and why is
   CLOCK_REALTIME a problem on an embedded board?
3. What happens when the owner of a NORMAL mutex locks it again? Of an
   ERRORCHECK one? Of a RECURSIVE one?
4. Walk through the robust-mutex protocol. Who owns the mutex when
   `lock()` returns `EOWNERDEAD`?
5. Tell the Mars Pathfinder story in four sentences. Which task was L, M, H?
6. Inheritance vs ceiling: when does each raise the owner's priority?
   Which one costs syscalls even without contention?
7. Name the four Coffman conditions, and say which one lock ordering breaks
   and which one try-lock-and-back-off breaks.
8. Why is comparing pointers with `<` to order locks technically undefined
   in C, and what do you do instead?
9. Why did TSan need the "warm-up" transfer in `07` to report the bug?
10. What is the difference between deadlock, livelock and starvation?
11. When is lock striping better than one lock per bucket?
12. Why does `std::scoped_lock(a, b)` not deadlock against
    `std::scoped_lock(b, a)`?
