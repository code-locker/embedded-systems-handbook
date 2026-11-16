# 08 — Barriers, once and thread pools

> Locks protect data. This lesson is about **coordinating threads**:
> making N threads meet before the next phase (barrier), running an
> initialisation exactly once no matter how many threads race to it (once,
> magic statics, double-checked locking), giving each thread its own copy
> of a global (TLS), and keeping a fixed team of threads busy with a queue
> of small jobs (thread pool, parallel for). It ends with the one way to
> stop a thread that you should almost never use: cancellation.

---

## 1. Why this exists

Real programs on the BeagleBone AI-64 run the same patterns again and
again:

| Problem | Tool |
|---|---|
| Image processing in tiles: all tiles of frame k done before frame k+1 starts | barrier |
| "Open the device / load the model on first use", called from any thread | `pthread_once`, `std::call_once`, function-local `static` |
| Per-thread scratch buffers, per-thread statistics, `errno` | thread-local storage |
| Thousands of small jobs (decode a packet, resize a tile) | thread pool |
| Split one big loop over the 2 A72 cores | parallel for |
| Stop a worker | stop flag (cooperative), *not* `pthread_cancel` |

Creating a thread costs tens of microseconds and its stack 8 MB of
virtual memory; doing that per job is wasteful, and unbounded thread
creation thrashes a two-core machine. A pool pays the cost once.

---

## 2. Mental model

- **Barrier = a tour guide counting the group at each attraction.** Nobody
  moves on until everyone has arrived. The *last* one to arrive is told
  "you're last" (`PTHREAD_BARRIER_SERIAL_THREAD`) and can, say, buy the
  next tickets.
- **Once = the first person to arrive unlocks the office**; everyone else
  who arrives while they are fumbling with the keys waits at the door; later
  arrivals walk straight in.
- **Thread pool = a taxi rank.** A fixed number of taxis (workers) wait at
  the rank (condition variable). A passenger (task) joins the queue; the
  first free taxi takes them.

```
  BARRIER (N = 3)                       THREAD POOL
  T0 ---work---|wait.........|---       submit() --> [t5|t4|t3] --> worker 0
  T1 ---work-------|wait.....|---                     queue       --> worker 1
  T2 ---work-----------------|---                  (mutex+condvar)--> worker 2
                   all arrived ^ T2 = SERIAL          results via futures
```

---

## 3. How it works

### 3.1 pthread_barrier_t  (`c/01_pthread_barrier.c`)

```c
pthread_barrier_t b;
pthread_barrier_init(&b, NULL, N);     /* N threads must call wait     */
int rc = pthread_barrier_wait(&b);     /* blocks until N have arrived  */
if (rc == PTHREAD_BARRIER_SERIAL_THREAD)  /* exactly one thread per round */
    do_between_phases();
pthread_barrier_destroy(&b);
```

The program solves 1-D heat diffusion with 4 threads, each owning a slice
of the grid. Step k+1 reads the neighbours' cells from step k, so every
step ends with a barrier. The serial thread swaps the `cur`/`next`
buffers; a *second* barrier makes sure nobody starts the next step before
the swap. The result is bit-identical to a single-threaded run, and the
serial thread is chosen exactly once per round (300 times for 300 steps).

```
   cur:  [ t0 slice ][ t1 slice ][ t2 slice ][ t3 slice ]
                    ^ t1 reads cur[63], which t0 wrote in the previous step
   per step: compute slice -> barrier -> SERIAL swaps -> barrier
```

A barrier is also a full memory synchronisation point: everything written
before `wait` by any thread is visible to every thread after it.

### 3.2 A reusable barrier: why a generation counter  (`c/02_reusable_barrier.c`)

The obvious mutex + condvar barrier:

```c
/* BROKEN when reused */
lock(m);
if (++waiting == N) { waiting = 0; broadcast(cv); }
else while (waiting != 0) wait(cv, m);
unlock(m);
```

```
   round k: T0, T1 sleep. T2 arrives last: waiting = 0, broadcast.
   T2 runs ahead into round k+1: waiting = 1, sleeps.
   T0 wakes: waiting (1) != 0 -> sleeps again   } still in round k,
   T1 wakes: waiting (1) != 0 -> sleeps again   } forever: DEADLOCK
```

"The count was reset" is not a stable fact: by the time a slow waiter
looks, the next round may have changed the count again. The fix is a
**generation** number that only ever increases:

```c
lock(m);
unsigned long my_gen = gen;
if (++waiting == N) { gen++; waiting = 0; broadcast(cv); unlock(m); return true; }
while (gen == my_gen) wait(cv, m);     /* "my round is over" is permanent */
unlock(m);
return false;
```

`c/08_barrier_no_generation_bug.c` is the broken version with a 5-second
watchdog (`pthread_timedjoin_np`). It usually deadlocks in the first
rounds. glibc's own `pthread_barrier_wait` uses the same idea with a
futex: a round counter that waiters compare against.

### 3.3 Once  (`c/03_once_and_dcl.c`, `cpp/01_call_once_magic_statics.cpp`)

```c
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void init(void) { g_codec = codec_create(); }
...
pthread_once(&once, init);   /* first caller runs init; concurrent callers
                                wait for it; later callers return at once */
```

Two guarantees: `init` runs **exactly once**, and every caller that returns
from `pthread_once` **sees everything `init` wrote** (it synchronises).
The fast path is a single acquire load. C11 has the same as `call_once`
and `once_flag` in `<threads.h>` (glibc 2.28+); C++ has `std::call_once`.

### 3.4 Double-checked locking

The pattern: check without the lock (fast), and only if not yet
initialised take the lock and check again.

```
  BROKEN (plain pointer)                 CORRECT (atomic pointer)
  if (p == NULL) {                       q = atomic_load(&p, ACQUIRE);
      lock();                            if (q == NULL) {
      if (p == NULL)                         lock();
          p = create();                      q = atomic_load(&p, relaxed);
      unlock();                              if (q == NULL) {
  }                                              q = create();
  use(p);                                        atomic_store(&p, q, RELEASE);
                                             }
                                             unlock();
                                         }
                                         use(q);
```

Why the plain version is broken: `p = create()` is several stores (the
object's fields, then the pointer). Without release ordering, the compiler
or the CPU (the A72 does this) may make the *pointer* visible before the
*fields*. Another thread passes the first check, skips the lock, and uses a
half-built object. On x86 it "works" almost always, which is why the bug
survived for years in Java and C++ code. The atomic version publishes the
object with a release store and reads it with an acquire load; the
program checks that 8 threads × 1000 calls construct it once and that at
most 8 calls ever take the lock.

### 3.5 C++ magic statics

```cpp
const Calibration &calibration() {
    static const Calibration instance;   // thread-safe since C++11
    return instance;
}
```

The compiler emits a guard byte and calls `__cxa_guard_acquire` /
`__cxa_guard_release` (libstdc++, implemented with a futex): exactly
double-checked locking, generated for you. If the constructor throws, the
static is not initialised and the next caller tries again. This is the
simplest correct lazy singleton in C++; prefer it over hand-written DCL.
(`-fno-threadsafe-statics` turns the guard off; some embedded builds use it
to save code size. Do not.)

### 3.6 Thread-local storage  (`c/06_tls_keys.c`)

| | `_Thread_local` / `thread_local` / `__thread` | `pthread_key_create` |
|---|---|---|
| Speed | an offset from the thread pointer (`TPIDR_EL0` on ARMv8, `%fs` on x86-64) | a function call + table lookup |
| Per-thread destructor | C: no; C++ `thread_local` objects: yes | yes, `destructor(value)` at thread exit |
| Dynamic number of keys | no (fixed at compile time) | yes (up to `PTHREAD_KEYS_MAX`, 1024) |

Rules the program verifies: each thread sees only its own value; the key's
destructor runs once per exiting thread whose value is non-NULL; it does
**not** run for the main thread when `main` returns, and
`pthread_key_delete` does not call destructors either. The key itself is
created with `pthread_once`. glibc uses exactly this for `errno`, the
`strerror` / `dlerror` buffers and per-thread malloc arenas.

### 3.7 Thread pool  (`c/04_thread_pool.c`, `cpp/02_thread_pool.cpp`)

```
   submit():  lock; if stopping -> refuse; push task; signal(has_work); unlock
   worker:    lock
              loop: while queue empty and not stopping: wait(has_work)
                    if queue empty and stopping: break       <- drain first
                    pop task; active++; unlock
                    run task                                  <- no lock held
                    lock; active--; if active==0 and queue empty: broadcast(all_idle)
   wait_all:  lock; while queue not empty or active > 0: wait(all_idle)
   destroy:   lock; stopping = true; broadcast(has_work); unlock; join all
```

Design decisions worth remembering:

1. **Run the task without the lock**, or the pool executes one task at a
   time.
2. **Graceful shutdown drains the queue**: workers exit only when
   `stopping` *and* the queue is empty. (An "abort" shutdown would free
   the remaining tasks instead.) Submits after shutdown are refused.
3. **`wait_all` needs `active`**, not just "queue empty": the last task
   may still be running.
4. **Broadcast on shutdown**, signal on submit (one task, one worker).
5. **Ownership**: the C pool frees each `Task` node after running it; the
   task's argument belongs to the submitter, who may read results only
   after `pool_wait()` (which gives the happens-before).

The C++ pool adds `submit(f, args...) -> std::future<R>` via
`std::packaged_task`: results and *exceptions* travel back to the caller
(`future.get()` rethrows), so a throwing task neither kills its worker
nor gets lost. The destructor is the shutdown, so the pool cannot outlive
its scope with threads still running (RAII).

Sizing: CPU-bound work → number of cores (2 on the TDA4VM A72 cluster);
I/O-bound work → more threads, or better, an event loop (lesson 13).

### 3.8 Parallel for  (`c/05_parallel_for.c`)

Split an index range, compute partial results with **no shared writes**,
combine after `join`.

```
   static:   [ T0: 0..n/4 )[ T1 ... )[ T2 ... )[ T3 ... n)   zero overhead,
                                                             imbalance if items differ
   dynamic:  next_chunk = fetch_add(&counter, CHUNK)        self-balancing,
             T0: c0 c4 c5 ...  T1: c1 c6 ...                one atomic per chunk
```

Each partial sum lives on its own 64-byte cache line (`_Alignas(64)`);
eight `uint64_t` side by side would share a line and every core's `+=`
would invalidate it in the others (false sharing: correct but slow, see
lesson [02](../02-race-conditions-and-atomics/README.md)). Accumulate in a
local variable, store once. This is OpenMP's `schedule(static)` vs
`schedule(dynamic, CHUNK)`.

### 3.9 Cancellation  (`c/07_cancellation.c`)

`pthread_cancel(t)` requests termination. With the default *deferred*
type, `t` dies at its next **cancellation point** (`pthread_cond_wait`,
`read`, `write`, `nanosleep`, `sleep`, `pthread_join`, `pthread_testcancel`,
and many more). The dangers:

```
   worker: lock(m) ... read(fd) <- CANCELLED here ... unlock(m) never runs
   main:   lock(m)   -> blocks forever
```

1. A thread cancelled while holding a mutex leaves it locked forever.
2. Memory it allocated leaks; invariants it was half-way through updating
   stay broken.
3. C++: in glibc, cancellation is a forced unwind (`abi::__forced_unwind`),
   so destructors run, but a `catch (...)` that swallows it aborts the
   program, and `noexcept` functions terminate.

Defences, all shown in the program:

- `pthread_cleanup_push(unlock, &m)` / `pthread_cleanup_pop(1)`: handlers
  run on cancellation, in reverse order. When cancelled inside
  `pthread_cond_wait`, the mutex is **re-acquired first**, so the handler
  must unlock it.
- `pthread_setcancelstate(PTHREAD_CANCEL_DISABLE)` around a multi-step
  update, then `pthread_testcancel()` at a safe place.
- **Best: do not cancel at all.** Set an atomic stop flag (and wake the
  thread: a condvar broadcast, an `eventfd` write, closing a socket), and
  let it return normally. Never use `PTHREAD_CANCEL_ASYNCHRONOUS` except
  around pure computation.

Sanitizer note: with AddressSanitizer, cancelling a C thread inside
`pthread_cleanup_push` confuses ASan's stack bookkeeping (glibc unwinds
with an internal `longjmp` ASan does not see), so the two functions that
get cancelled are marked `no_sanitize_address` under ASan. TSan checks
them normally.

---

## 4. Complexity table

| Operation | Time | Notes |
|---|---|---|
| `pthread_barrier_wait` | O(1) per thread, last arriver wakes N−1 | one futex wake-all per round |
| Reusable barrier (mutex+condvar) | O(1) per thread + broadcast | N context switches per round |
| `pthread_once` / `call_once` / magic static, after init | O(1): one acquire load | the first call costs the init |
| DCL fast path | one acquire load | `LDAR` on ARMv8 |
| `pthread_getspecific` | O(1), function call | `_Thread_local`: one load via thread pointer |
| Pool `submit` | O(1) + lock + maybe one wake | linked list / deque push |
| Pool task dispatch | O(1) | contention on one queue lock: use per-worker queues + work stealing at high rates |
| Parallel for, static | O(n / T) per thread | plus thread start/join, or pool dispatch |
| Parallel for, dynamic | O(n / T) + O(n / CHUNK) atomics | CHUNK trades balance vs. overhead |

---

## 5. C vs C++

| Concept | C (POSIX / C11) | C++17 | C++20 and later |
|---|---|---|---|
| Barrier | `pthread_barrier_t` | hand-written (`cpp/03`) | `std::barrier<Completion>` |
| One-shot countdown | hand-written | hand-written `Latch` (`cpp/03`) | `std::latch` |
| Once | `pthread_once`, C11 `call_once` | `std::call_once` + `std::once_flag`; function-local `static` | same |
| Lazy singleton | `pthread_once` or atomic DCL | magic static | same |
| TLS | `_Thread_local`, `pthread_key_create` | `thread_local` (with constructors/destructors) | same |
| Thread pool | hand-written | hand-written; `std::async` (no pool guarantee) | C++26 `std::execution` (senders/receivers) |
| Results from tasks | output fields + `pool_wait` | `std::future` / `std::packaged_task` / `std::promise` | `std::jthread` + `std::stop_token` for cooperative stop |
| Cancellation | `pthread_cancel` + cleanup handlers | stop flag + condvar | `std::jthread::request_stop()`, `std::stop_callback` |

Note on `std::async(std::launch::async, f)`: it starts a new thread per
call in libstdc++ (no pool), and the returned future's destructor
**blocks** until the task finishes, so discarding it makes the call
synchronous. Prefer an explicit pool.

---

## 6. Where it is used in the real world

- **Linux kernel.** **Workqueues** (`queue_work()`, `alloc_workqueue()`,
  the `kworker/*` threads) are the kernel's thread pools; drivers defer
  work from interrupt handlers to them. `DEFINE_PER_CPU` / `this_cpu_*`
  are the kernel's TLS. `DO_ONCE()` and `static_key` handle run-once
  initialisation; completions (`wait_for_completion`) are latches.
  See [linux-device-drivers/01-kernel-synchronization](../../linux-device-drivers/01-kernel-synchronization/README.md).
- **glibc.** `pthread_barrier_wait` and `pthread_once` are futex based;
  `errno` is `__thread`; `__cxa_guard_acquire` (libstdc++) for magic
  statics.
- **GStreamer.** `GstTaskPool` (the default one wraps a `GThreadPool`)
  provides the threads behind every `GstTask` (each `queue` element's
  streaming thread, see
  [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md));
  `GThreadPool` itself is a pool exactly like `c/04`. `g_once_init_enter`
  / `g_once_init_leave` is GLib's double-checked lazy init (used by every
  `G_DEFINE_TYPE` to register a type once).
- **OpenMP** (`#pragma omp parallel for schedule(static|dynamic)`,
  `#pragma omp barrier`, `#pragma omp single`, `threadprivate`): the same
  ideas, generated by the compiler. OpenCV's `parallel_for_` runs on such
  a pool (pthreads, OpenMP or TBB backend).
- **Embedded/RTOS.** FreeRTOS has no barrier but event groups
  (`xEventGroupSync()` is a barrier), and pools are a set of tasks
  blocking on one queue. Zephyr has a system **work queue**
  (`k_work_submit`), the RTOS equivalent of kernel workqueues.

---

## 7. Common mistakes and pitfalls

1. **Barrier count mismatch**: initialising with N and having N−1 or N+1
   threads call `wait` → hang (or one thread races into the next round).
2. **Reusable barrier without a generation counter** → deadlock
   (`c/08_barrier_no_generation_bug.c`).
3. **Destroying a barrier while threads are still inside `wait`.**
4. **Plain-pointer double-checked locking**: half-constructed objects on
   ARM. Use an atomic with acquire/release, or just `pthread_once` / a
   magic static.
5. **`std::call_once` with a throwing callable**: the standard says the
   next caller retries, but on Linux older libstdc++ versions could hang
   in that case (GCC bug 66146). Do not rely on throwing out of
   `call_once` unless you have tested your toolchain.
6. **Running the task while holding the pool lock** → no parallelism.
7. **`wait_all` that checks only "queue empty"** → returns while the last
   task is still running.
8. **Shutdown with `signal` instead of `broadcast`** → some workers sleep
   forever and `join` hangs.
9. **Tasks that wait for other tasks in the same pool** (e.g. a task
   calling `future.get()` on a sub-task) → with all workers waiting,
   nobody runs the sub-tasks: deadlock. Size the pool or restructure.
10. **False sharing of per-thread results** in a parallel for.
11. **TLS destructor expectations**: not run for `main`, not run by
    `pthread_key_delete`, not run on `_exit`/`exit`.
12. **`pthread_cancel` with a mutex held** and no cleanup handler →
    mutex locked forever. Prefer a stop flag.
13. **Discarding a `std::async` future** → the call becomes synchronous.
14. Always re-run concurrency code under TSan:
    `make run P=linux-internals/08-barriers-once-and-thread-pools/c/04_thread_pool.c SAN=tsan`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_pthread_barrier.c](c/01_pthread_barrier.c) | Phased heat-diffusion solver with `pthread_barrier_wait`, SERIAL thread swaps buffers; bit-identical to serial | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/01_pthread_barrier.c` |
| [c/02_reusable_barrier.c](c/02_reusable_barrier.c) | Mutex + condvar barrier with a generation counter, 2000 rounds verified | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/02_reusable_barrier.c` |
| [c/03_once_and_dcl.c](c/03_once_and_dcl.c) | `pthread_once` and correct double-checked locking with C11 atomics, init counted | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/03_once_and_dcl.c` |
| [c/04_thread_pool.c](c/04_thread_pool.c) | Fixed-size pool: queue, `pool_wait`, graceful drain-on-shutdown, refused late submits | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/04_thread_pool.c` |
| [c/05_parallel_for.c](c/05_parallel_for.c) | Parallel array sum, static vs dynamic chunks, cache-line-padded partials | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/05_parallel_for.c` |
| [c/06_tls_keys.c](c/06_tls_keys.c) | `pthread_key_create` with destructors, `_Thread_local`, key created via `pthread_once` | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/06_tls_keys.c` |
| [c/07_cancellation.c](c/07_cancellation.c) | `pthread_cancel` in `cond_wait`, cleanup handlers unlock + free, cancel-disable, stop flag | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/07_cancellation.c` |
| [c/08_barrier_no_generation_bug.c](c/08_barrier_no_generation_bug.c) | **Bug:** barrier without generation counter; watchdog reports the deadlock | `make run P=linux-internals/08-barriers-once-and-thread-pools/c/08_barrier_no_generation_bug.c` |
| [cpp/01_call_once_magic_statics.cpp](cpp/01_call_once_magic_statics.cpp) | Magic static, `std::call_once` per object, DCL with `std::atomic` | `make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/01_call_once_magic_statics.cpp` |
| [cpp/02_thread_pool.cpp](cpp/02_thread_pool.cpp) | Pool with `std::function`, `packaged_task` futures, exceptions to caller, RAII drain, `parallel_for` | `make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/02_thread_pool.cpp` |
| [cpp/03_barrier_latch.cpp](cpp/03_barrier_latch.cpp) | C++17 `Latch` and `Barrier` with completion step (C++20 `std::latch`/`std::barrier` shape) | `make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/03_barrier_latch.cpp` |

Every program except the `_bug` also passes under ThreadSanitizer, e.g.
`make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/02_thread_pool.cpp SAN=tsan`,
or `make test-tsan` for the whole track.

---

## 9. Exercises

**Easy**
1. Rewrite `c/01` with **one** barrier per step: each thread computes
   which buffer is `cur` from the step parity, so no swap is needed.
2. Add a `pool_pending()` function to `c/04` that returns queued + active
   tasks. Why is the value stale as soon as it is returned?
3. Make `c/05` deliberately false-share (`uint64_t partial[8]`, `+=` in the
   loop) and time both versions with `make run ... SAN=0 OPT=1`.

**Medium**
4. Add a bounded queue to `c/04`: `pool_submit` blocks when 64 tasks are
   queued (a second condvar `not_full`, as in
   [dsa/19 c/03](../../dsa/19-system-design-structures/README.md)).
5. Add `pthread_barrier_t`-style SERIAL return and a timeout
   (`barrier_timedwait`) to `c/02`. What should happen to the other
   waiters when one times out? (POSIX barriers have no timeout for a
   reason.)
6. Give the C++ pool a `wait_idle()` like `pool_wait()` in C, and a
   `submit_bulk(first, last, f)` that queues many tasks under one lock.

**Hard**
7. **Work stealing:** one deque per worker; a worker pushes/pops at the
   back of its own deque and steals from the front of others when empty.
   Compare throughput with the single-queue pool for 1 µs tasks.
8. Implement `std::jthread`-style cooperative cancellation for the C pool:
   a `stop_token` that long tasks poll, plus a way to wake a task blocked
   in `read()` (hint: `eventfd` + `poll`, lesson 13).
9. Implement a **sense-reversing** barrier with only atomics (no mutex):
   each thread flips a local sense; the last arriver resets the count and
   flips the global sense. Verify under TSan.

---

## 10. Remember forever

```
barrier : N meet; last arriver = SERIAL; full memory sync. Reusable needs a GENERATION,
          waiters wait for gen to change, never for count to reset.
once    : pthread_once / std::call_once / function-local static (magic static, C++11).
DCL     : q=load(p,ACQUIRE); if(!q){lock; q=load(p); if(!q){q=new; store(p,q,RELEASE)} unlock}
TLS     : _Thread_local = fast; pthread_key = destructor at thread exit (not for main).
pool    : queue+mutex+cv; run task WITHOUT lock; wait_all needs active count;
          shutdown: stopping=1, BROADCAST, workers drain queue, join.  C++: packaged_task->future.
parfor  : partials per thread on own cache line; combine after join; static vs dynamic chunks.
cancel  : dies at cancellation points holding locks -> cleanup_push(unlock); prefer a stop flag.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What does `PTHREAD_BARRIER_SERIAL_THREAD` tell you, and what is it for?
2. Why does `c/01` need two barriers per step? How can it be done with one?
3. Draw the interleaving that deadlocks a reusable barrier without a
   generation counter. Why does a generation number fix it?
4. What two guarantees does `pthread_once` give?
5. Why is double-checked locking with a plain pointer broken on ARM, and
   which orderings make it correct?
6. What code does the compiler generate for a function-local `static`?
   What happens if its constructor throws?
7. When do `pthread_key_create` destructors run, and when do they not?
8. In a thread pool, why must the task run without the lock? Why does
   `wait_all` need an `active` counter?
9. How does the C++ pool deliver a task's exception to the caller?
10. Static vs dynamic scheduling in a parallel for: when does each win?
    What is false sharing and how does `c/05` avoid it?
11. Why is `pthread_cancel` dangerous with mutexes? What happens to the
    mutex when a thread is cancelled inside `pthread_cond_wait`?
12. Map these to Linux/GStreamer/RTOS equivalents: thread pool, latch,
    per-thread data, barrier.
