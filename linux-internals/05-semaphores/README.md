# 05 — Semaphores

> A **semaphore** is a counter that never goes below zero, plus a queue of
> threads waiting for it to become positive. Dijkstra invented it in 1965.
> It is the oldest synchronisation primitive still in daily use. It is the
> right tool for **counting resources** and **signalling events**, including
> from a signal handler or an interrupt. It is the *wrong* tool for plain
> mutual exclusion.

---

## 1. Why this exists

Two problems come up in every embedded system, and a mutex solves neither:

1. **"At most N at a time."** There are 3 DMA channels, 4 hardware
   encoder instances, 8 free buffers in a pool. Thread 9 must wait until
   one is returned.
2. **"Tell another thread something happened."** The capture thread
   finished a frame. The UART interrupt received a byte. The child process
   is ready. The waiter may arrive *before or after* the event, and either
   way it must not miss it.

A mutex has exactly one unit and an *owner* (only the locker may unlock).
A condition variable has no memory. A semaphore has **N units, no owner,
and remembers every post** in its count.

| You need... | Use |
|---|---|
| protect shared data | mutex ([03](../03-mutexes-and-deadlock/README.md)) |
| wait for an arbitrary predicate | condition variable ([04](../04-condition-variables/README.md)) |
| limit concurrency to N / count free slots | **counting semaphore** |
| signal "event happened" (remembered), even from a signal handler | **semaphore** (or eventfd, [13](../13-signals-eventfd-and-epoll/README.md)) |
| the same between processes | named POSIX semaphore, or `sem_t` in shared memory with `pshared = 1` |

---

## 2. Mental model

**A car park with a counter display at the entrance.** The display shows
free spaces (the value). A car arriving when the display says 0 waits at
the barrier (`sem_wait` sleeps). A car leaving increments the display and
the barrier lets one waiting car in (`sem_post`). Nobody checks *which*
car leaves: any car can make room, which is exactly why the semaphore has
no owner.

```
            sem_wait (P)                         sem_post (V)
   value > 0 ?  yes: value--, go on        value++ ; if someone sleeps, wake one
                no : sleep in the queue
   +-------------------------------------------+
   |  value: 2        waiters: [ ]             |   resource pool, 2 free
   |  value: 0        waiters: [T3 T4]         |   2 in use, 2 waiting
   |  post -> value 0 waiters: [T4]  T3 runs   |   the post was "consumed" by T3
   +-------------------------------------------+
   invariant: value >= 0;  value = (initial + posts - completed waits)
```

P and V come from Dutch: **P** = *proberen* (to try) or *passeren* (to
pass), **V** = *verhogen* (to raise). FreeRTOS calls them *take* and
*give*. The Linux kernel calls them `down()` and `up()`.

---

## 3. How it works

### 3.1 The POSIX API (`c/01_sem_basics.c`)

```c
#include <semaphore.h>
sem_t s;
sem_init(&s, /*pshared=*/0, /*value=*/2);   /* 0: threads of this process */
sem_wait(&s);          /* P: -1 or sleep.  -1/EINTR if a signal handler ran */
sem_trywait(&s);       /* -1/EAGAIN instead of sleeping */
sem_timedwait(&s, &abs);          /* abs deadline on CLOCK_REALTIME -> ETIMEDOUT */
sem_clockwait(&s, CLOCK_MONOTONIC, &abs);   /* glibc >= 2.30 */
sem_post(&s);          /* V: +1, wake one; async-signal-safe */
sem_getvalue(&s, &v);  /* snapshot only: stale as soon as it returns */
sem_destroy(&s);       /* nobody may be waiting */
```

Unlike `pthread_*` functions, semaphore functions return **−1 and set
`errno`**. Always loop on `EINTR` around `sem_wait`. On Linux, glibc's
`sem_t` is a 64-bit word (value + waiter count) and a **futex**: an
uncontended wait or post is one atomic instruction, with no syscall.

### 3.2 Counting vs binary

- **Counting**: any non-negative value. Counts resources, items, events.
- **Binary**: only 0 or 1. POSIX has no separate type. A `sem_t` used as
  binary is just a counting semaphore you *promise* never to post twice, and
  `c/01` shows how one stray post breaks that promise (value 2: two threads
  inside). C++20 `std::binary_semaphore` and FreeRTOS
  `xSemaphoreCreateBinary` do cap the value at 1.

### 3.3 Semaphore vs mutex

| | Mutex | Semaphore |
|---|---|---|
| Owner | yes: only the locker unlocks (ERRORCHECK enforces) | **none**: any thread may post |
| Count | 1 | 0…N |
| Remembers a "give" with no taker | n/a | **yes**, in the count |
| Priority inheritance | yes (`PTHREAD_PRIO_INHERIT`) | **impossible**: with no owner there is nobody to boost |
| Post from a signal handler / ISR | **no** | **yes** (`sem_post` is async-signal-safe) |
| Recursive locking | optional | meaningless (it would deadlock or over-count) |
| Typical bug | forgetting to unlock | posting twice, or posting from the wrong place |

Using a binary semaphore as a lock *works*. But you lose ownership
checks and priority inheritance (on an RTOS this causes the Pathfinder
inversion, [03 §3.6](../03-mutexes-and-deadlock/README.md)). You also lose
the guarantee that only the locker unlocks. **Mutex for exclusion,
semaphore for counting and signalling.**

### 3.4 Resource pool: limit concurrency to N (`c/02_resource_pool.c`)

```
 sem free = 3          take: P(free); lock(list); pick a free channel; unlock(list)
 list [ch0 ch1 ch2]    give: lock(list); mark channel free; unlock(list); V(free)
```

The semaphore answers **"may I?"**: one is guaranteed free when `P`
returns. A small mutex-protected free-list answers **"which one?"**. Order
matters. Take from the list *after* `P`, and put back *before* `V`, so the
woken thread is sure to find a slot. The program records the maximum number
of threads inside at once with an atomic CAS loop and checks it is ≤ 3.

### 3.5 Producer/consumer with two semaphores (`c/03_producer_consumer.c`)

Dijkstra's classic:

```
 empty_slots = N   full_slots = 0   mutex m (indices only)

 producer:  P(empty_slots); lock(m); buf[tail++] = x; unlock(m); V(full_slots)
 consumer:  P(full_slots);  lock(m); x = buf[head++]; unlock(m); V(empty_slots)

 invariant: empty_slots + full_slots + items in flight == N
```

Always **P before lock**. If you lock first and then sleep in `P`, the
other side can never get the lock to `V` you, and both sides deadlock.
Compared with the condition-variable queue
([04 `c/04`](../04-condition-variables/README.md)): no predicate loop and
no lost wake-ups (posts are counted). But there is no natural `close()`, so
consumers stop on **poison pills**, one per consumer, queued after the real
items. Lock-free SPSC alternative:
[dsa/07 §3.8](../../dsa/07-stacks-and-queues/README.md). Mutex+condvar ring:
[dsa/19 `c/03`](../../dsa/19-system-design-structures/README.md).

### 3.6 Ordering, and posting from a signal handler (`c/01` part 2, `c/04_sem_signal_handler.c`)

A semaphore initialised to **0** means "not yet". `A: work; post` /
`B: wait; continue` makes B run after A, whichever starts first. `post` is a
release and `wait` an acquire, so A's writes are visible to B.

`sem_post` is in the **async-signal-safe** list (`man 7 signal-safety`).
`pthread_mutex_lock`, `pthread_cond_signal`, `printf` and `malloc` are not.
The handler may have interrupted the very thread that holds the lock. So:

```
 signal handler (keep it tiny)          worker thread
 ---------------------------            -------------
 flag/counter (atomic) ++               while (sem_wait(&ev) == -1 && errno == EINTR) ;
 sem_post(&ev)  ---------------------->  handle the event: printf, malloc, locks... all OK
```

This is the user-space version of an interrupt's **top half / bottom
half**, and of FreeRTOS `xSemaphoreGiveFromISR()` waking a task. Standard
signals do not queue: two pending `SIGUSR1`s merge into one. The program
waits for an acknowledgement before raising the next. Lesson
[13](../13-signals-eventfd-and-epoll/README.md) shows `signalfd`/`eventfd`,
the modern alternative.

### 3.7 Named semaphores across processes (`c/05_named_sem_fork.c`)

```c
sem_t *s = sem_open("/esh_lock_1234", O_CREAT | O_EXCL, 0600, 1);  /* create */
sem_t *s = sem_open("/esh_lock_1234", 0);                          /* open existing */
sem_close(s);                       /* this process is done with it */
sem_unlink("/esh_lock_1234");       /* remove the NAME; object dies at last close */
```

On Linux the object is the file `/dev/shm/sem.esh_lock_1234`, a small
mmap'ed page holding a `sem_t` with `pshared = 1`. Lifetime works like a
file: it survives the creator unless unlinked. Use a **unique name** (the
program puts the PID in it), `O_EXCL` to avoid silently reusing a stale
one with an unknown value, and an `atexit` handler that unlinks on failure.

The alternative for related processes: put a `sem_t` in
`mmap(MAP_SHARED)` memory (or `shm_open`,
[11](../11-ipc-shared-memory/README.md)) and call
`sem_init(s, 1, value)`. **Both** parts are needed. `c/08_sem_pshared_fork_bug.c`
shows what happens if either is missing.

### 3.8 System V semaphores (`c/06_sysv_semaphore.c`)

```c
int id = semget(IPC_PRIVATE /* or ftok(path, 'x') */, nsems, IPC_CREAT | 0600);
semctl(id, i, SETVAL, (union semun){ .val = 1 });    /* caller defines semun! */
struct sembuf ops[2] = { {0, -1, SEM_UNDO}, {1, -2, SEM_UNDO} };
semop(id, ops, 2);                 /* ALL of these, atomically, or block */
semctl(id, 0, IPC_RMID);           /* else it lives until reboot (ipcs -s) */
```

What System V still does better: **atomic operations on a set** (take
the lock and two buffers in one step, all or nothing, with no
hold-and-wait), and **`SEM_UNDO`** (the kernel reverses your changes when
your process dies, even by `SIGKILL`). The program shows a child that dies
holding the semaphore. With `SEM_UNDO` the value is restored. Without it,
the unit is lost forever. The downsides: every operation is a syscall, the
API is clumsy, and objects leak if not removed.

### 3.9 Building one yourself (`c/07_sem_from_mutex_condvar.c`, `cpp/01_semaphore_class.cpp`)

A semaphore is "a count + wait until count > 0", a predicate on shared
state, so lesson 04 builds it directly:

```
 wait: lock; while (count == 0) cond_wait; count--; unlock
 post: lock; count++; cond_signal; unlock
```

C++17 has no semaphore. C++20 adds `std::counting_semaphore<LeastMax>` and
`std::binary_semaphore` (`acquire`, `try_acquire`, `try_acquire_for`,
`release(n)`). `cpp/01` gives the same interface on mutex+cv, a
`Permit` RAII guard (exceptions cannot leak a pool slot), and a binary
semaphore that *throws* on a second `release()`.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `sem_wait` / `sem_post` uncontended (POSIX) | one atomic RMW, ~20 ns, no syscall | futex fast path in user space |
| `sem_wait` that must sleep | futex wait syscall + context switch | kernel queue |
| `sem_post` with waiters | futex wake syscall | wakes one |
| `sem_open` / `sem_unlink` | file create/unlink in `/dev/shm` + mmap | named objects are files |
| `semop` (System V) | **always a syscall**, ~0.3–1 µs | kernel object, undo bookkeeping |
| `semop` on k semaphores | O(k), atomic | all-or-nothing check |
| mutex+cv semaphore | lock + unlock + (signal) | 2–3 atomics uncontended |
| Memory | `sem_t`: 32 B (glibc, 64-bit) | value + waiters word + padding |

---

## 5. C vs C++

| Concept | C | C++17 | C++20 |
|---|---|---|---|
| Counting semaphore | `sem_t` (POSIX) | write one: `CountingSemaphore<Max>` on mutex+cv (`cpp/01`) | `std::counting_semaphore<Max>` |
| Binary | `sem_t` used as 0/1 (not enforced) | `CountingSemaphore<1>` (enforced, throws) | `std::binary_semaphore` (over-release is UB) |
| P / V | `sem_wait` / `sem_post` | `acquire()` / `release(n)` | same |
| Try / timed | `sem_trywait` / `sem_timedwait` (REALTIME), `sem_clockwait` | `try_acquire` / `try_acquire_for` (steady_clock) | same |
| Scope guard | — (`cleanup` attribute) | `Permit` RAII class | write one (none in std) |
| Between processes | named `sem_open`, or `sem_t` in shared memory with pshared=1; System V | use the C API | same |
| From a signal handler | `sem_post` only | **not** the mutex+cv class (not async-signal-safe) | `std::` semaphores are not guaranteed signal-safe |

---

## 6. Where it is used in the real world

- **Linux kernel.** `struct semaphore` (`include/linux/semaphore.h`:
  `sema_init`, `down`, `down_interruptible`, `down_timeout`, `up`) is a
  counting semaphore with a spinlock and a wait list. New code is told to
  use `struct mutex` for exclusion and `struct completion` for "wait for an
  event". Semaphores remain for true counting (e.g. limiting outstanding
  requests in some drivers). The kernel's user-space side implements System
  V semaphores in `ipc/sem.c`. See
  [linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md).
- **glibc.** `nptl/sem_wait.c`, `sem_post.c`, `sem_open.c`. Named
  semaphores live in `/dev/shm`. `sem_post` stays async-signal-safe because
  it is lock-free.
- **GStreamer / multimedia.** Buffer pools have a fixed number of buffers.
  `gst_buffer_pool_acquire_buffer` blocks when all are in use. That is a
  counting semaphore (implemented with a `GstPoll`/cond internally). V4L2
  capture has the same shape with `VIDIOC_DQBUF`/`QBUF` on a fixed set of
  DMA buffers. See [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md).
- **Databases and servers.** Connection pools and "max N concurrent
  requests" limiters. PostgreSQL used System V semaphores for backend
  wake-ups for decades (now POSIX by default on Linux). Apache httpd
  used `SEM_UNDO` locks so a crashed worker cannot block the accept mutex.
- **Embedded / RTOS: FreeRTOS mapping.**

  | FreeRTOS | POSIX / C++ equivalent | Notes |
  |---|---|---|
  | `xSemaphoreCreateBinary()` | `sem_init(&s, 0, 0)` used as 0/1; C++20 `binary_semaphore` | created **empty** (must be given first); for ISR → task signalling |
  | `xSemaphoreCreateCounting(max, init)` | `sem_init(&s, 0, init)`; `counting_semaphore<max>` | resource pools, event counting |
  | `xSemaphoreCreateMutex()` | `pthread_mutex_t` with `PTHREAD_PRIO_INHERIT` | **has priority inheritance**, has an owner, must not be used from an ISR |
  | `xSemaphoreCreateRecursiveMutex()` | `PTHREAD_MUTEX_RECURSIVE` | `xSemaphoreTakeRecursive` |
  | `xSemaphoreTake(s, ticks)` | `sem_timedwait` / `try_acquire_for` | `portMAX_DELAY` = wait forever |
  | `xSemaphoreGive(s)` | `sem_post` / `release()` | |
  | `xSemaphoreGiveFromISR(s, &woken)` | `sem_post` from a signal handler (`c/04`) | then `portYIELD_FROM_ISR(woken)` |
  | `ulTaskNotifyTake` / `xTaskNotifyGive` | eventfd ([13](../13-signals-eventfd-and-epoll/README.md)) | lighter, per-task "binary/counting semaphore" |

  Zephyr: `k_sem_init/take/give` (counting, ISR-safe give). On the TDA4VM
  the R5F cores running FreeRTOS use exactly these, while Linux on the A72
  uses POSIX semaphores.

---

## 7. Common mistakes and pitfalls

1. **Using a semaphore as a mutex.** No owner, no priority inheritance,
   and a stray `post` lets two threads in. Use a mutex.
2. **Not looping on `EINTR`** around `sem_wait`/`sem_timedwait`. A signal
   handler makes it return −1 early, and your code proceeds without the unit.
3. **Locking a mutex, then sleeping in `sem_wait`** (P inside the lock).
   The poster needs that mutex: deadlock. Always P first, then lock.
4. **`sem_t` in ordinary memory across `fork()`.** The child gets a copy;
   posts never reach the parent. **`pshared = 0` in shared memory** is also
   broken: the private futex wake goes to the wrong queue. Both hang, see
   `c/08_sem_pshared_fork_bug.c`. Under `strace` bug B often *disappears*:
   a Heisenbug.
5. **Named semaphore leaks.** Forgetting `sem_unlink` leaves
   `/dev/shm/sem.*` behind with whatever value it had. The next run with
   `O_CREAT` (no `O_EXCL`) silently reuses it. Use unique names, `O_EXCL`,
   and an `atexit` cleanup.
6. **System V without `IPC_RMID`**, or without `SEM_UNDO` for lock-like
   use. Check with `ipcs -s` and clean up with `ipcrm -s <id>`.
7. **Trusting `sem_getvalue`.** It is a snapshot for logging and tests,
   never for decisions (`if (value > 0) sem_wait()` is a race).
8. **`sem_timedwait` deadlines on CLOCK_REALTIME** on a board whose clock
   jumps at NTP sync. Use `sem_clockwait(CLOCK_MONOTONIC)` (glibc ≥ 2.30).
9. **Calling anything but `sem_post` (and other async-signal-safe
   functions) in a signal handler.** `printf` in a handler can deadlock on
   stdio's lock.
10. **Destroying a semaphore while threads wait on it**, or `sem_destroy`
    on a named semaphore (use `sem_close`).
11. **TSan and steady-clock waits:** the C++ class uses `system_clock`
    under TSan only, for the GCC 11 interceptor gap explained in
    [04 §5](../04-condition-variables/README.md). Verify with
    `make run P=linux-internals/05-semaphores/cpp/01_semaphore_class.cpp SAN=tsan`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_sem_basics.c](c/01_sem_basics.c) | `sem_init/wait/trywait/timedwait/clockwait/post/getvalue/destroy`; B-waits-for-A ordering; a post before the wait is remembered; binary semaphore as a lock and how a stray post breaks it | `make run P=linux-internals/05-semaphores/c/01_sem_basics.c` |
| [c/02_resource_pool.c](c/02_resource_pool.c) | 8 threads share 3 "DMA channels": semaphore counts, mutex free-list picks; max concurrency checked ≤ 3 | `make run P=linux-internals/05-semaphores/c/02_resource_pool.c` |
| [c/03_producer_consumer.c](c/03_producer_consumer.c) | Dijkstra's bounded buffer: `empty_slots`, `full_slots`, mutex; 3 producers, 2 consumers, poison pills; sums checked | `make run P=linux-internals/05-semaphores/c/03_producer_consumer.c` |
| [c/04_sem_signal_handler.c](c/04_sem_signal_handler.c) | `sem_post` from a `SIGUSR1` handler wakes a worker; `EINTR` retry loop; top half / bottom half | `make run P=linux-internals/05-semaphores/c/04_sem_signal_handler.c` |
| [c/05_named_sem_fork.c](c/05_named_sem_fork.c) | `sem_open` by unique name in 3 forked children, shared-memory counter, timed "done" semaphore, `sem_unlink` lifetime | `make run P=linux-internals/05-semaphores/c/05_named_sem_fork.c` |
| [c/06_sysv_semaphore.c](c/06_sysv_semaphore.c) | `semget/semop/semctl`; `SEM_UNDO` restores a dead child's hold; `IPC_NOWAIT`; atomic all-or-nothing multi-semaphore `semop`; `IPC_RMID` | `make run P=linux-internals/05-semaphores/c/06_sysv_semaphore.c` |
| [c/07_sem_from_mutex_condvar.c](c/07_sem_from_mutex_condvar.c) | counting semaphore from mutex + condvar: wait, trywait, monotonic timedwait, post; pool and ordering tests | `make run P=linux-internals/05-semaphores/c/07_sem_from_mutex_condvar.c` |
| [c/08_sem_pshared_fork_bug.c](c/08_sem_pshared_fork_bug.c) | **BUG**: `sem_t` copied by `fork()`, and `pshared=0` in shared memory: parent never woken (1 s timeouts, exits 1); the fix for contrast | `make run P=linux-internals/05-semaphores/c/08_sem_pshared_fork_bug.c` |
| [cpp/01_semaphore_class.cpp](cpp/01_semaphore_class.cpp) | `CountingSemaphore<Max>` / `BinarySemaphore` with the C++20 interface, `Permit` RAII guard with exceptions, `try_acquire_for`, `release(n)` | `make run P=linux-internals/05-semaphores/cpp/01_semaphore_class.cpp` |

Every non-bug program also runs clean under ThreadSanitizer:

```bash
make run P=linux-internals/05-semaphores/c/02_resource_pool.c SAN=tsan
```

TSan sees threads, not processes. For the fork-based programs (`c/05`,
`c/06`, `c/08`) it checks each process on its own.

---

## 9. Exercises

**Easy**
1. In `c/02`, change `N_CHANNELS` to 1. What primitive did you just build,
   and what is it missing compared with a mutex?
2. In `c/01`, replace the ordering semaphore with a condition variable and
   *no* flag variable. Start B after A has already signalled. What happens?
3. Run `ls -l /dev/shm` while `c/05` is paused in a debugger at the
   children's loop.

**Medium**
4. Rewrite `c/03` so consumers stop without poison pills: add a `closed`
   flag and post `full_slots` once per consumer in `close()`. Which race
   must you handle?
5. Implement a **rendezvous** (A waits for B and B waits for A) with two
   semaphores. Then a reusable barrier for N threads with semaphores only
   (the "turnstile" pattern).
6. Make `c/04` count events correctly *without* the acknowledgement by
   using a real-time signal (`SIGRTMIN`, sent with `sigqueue`). Why do RT
   signals queue?
7. Add `try_acquire_until(time_point)` to `cpp/01`.

**Hard**
8. **Readers-writers with semaphores** (Courtois et al. 1971): write the
   readers-preference version, show writer starvation, then the
   writers-preference version. Compare with `pthread_rwlock`
   ([07](../07-rwlocks-seqlocks-and-rcu/README.md)).
9. Implement a counting semaphore on a raw **futex** (`syscall(SYS_futex,
   ...)`) with a user-space fast path, the way glibc does. Make `post` safe
   to call from a signal handler.
10. With `SEM_UNDO`, what does the kernel do if the undo would make the
    value negative? Read `ipc/sem.c` (`exit_sem`) and write a test.

---

## 10. Remember forever

```
semaphore = counter >= 0 + wait queue.  P/wait/take/down: -1 or sleep.  V/post/give/up: +1, wake one
NO owner -> anyone posts -> great for signalling & counting, bad as a mutex (no PI, stray post = 2 inside)
a post with nobody waiting is REMEMBERED (count) -- a condvar signal is not
pool: P(free); lock list; pick; unlock  |  lock list; free it; unlock; V(free)
prod/cons: P(empty) lock put unlock V(full)  /  P(full) lock get unlock V(empty)   -- P BEFORE lock
sem_post is async-signal-safe: handler posts, thread does the work (ISR give -> task take)
sem_wait: loop on EINTR.  errno-style (-1), not pthread-style (return code)
between processes: named sem_open("/unique") + sem_unlink, or sem_t in MAP_SHARED with pshared=1 (BOTH!)
SysV: semop on a set is atomic; SEM_UNDO frees a dead holder's units; always IPC_RMID
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What do P and V do, exactly, including the case where a thread is
   waiting?
2. Give three differences between a mutex and a binary semaphore. Why
   can a semaphore not have priority inheritance?
3. Why does a semaphore-based "B waits for A" work even if A posts before B
   starts, while a bare condition variable signal does not?
4. In the resource pool, why is the free-list protected by a separate
   mutex? Why take from it *after* `sem_wait`?
5. In the producer/consumer, what goes wrong if the consumer locks the
   mutex *before* `P(full_slots)`?
6. Which functions may you call in a signal handler, and why is `sem_post`
   one of them but `pthread_cond_signal` not?
7. What file does `sem_open("/x", O_CREAT, 0600, 1)` create on Linux? What
   do `sem_close` and `sem_unlink` each do?
8. Explain both bugs in `c/08`. Why does bug B often vanish under `strace`?
9. What does `SEM_UNDO` do, and what problem of robust mutexes
   ([03](../03-mutexes-and-deadlock/README.md)) does it solve
   differently?
10. Why is a System V `semop` on two semaphores safer than two POSIX
    `sem_wait` calls in a row?
11. Map `xSemaphoreCreateBinary`, `xSemaphoreCreateCounting`,
    `xSemaphoreCreateMutex` and `xSemaphoreGiveFromISR` to POSIX.
12. What does the Linux kernel recommend instead of `struct semaphore`
    for (a) mutual exclusion and (b) waiting for an event?
