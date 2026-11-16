# 04 — Condition Variables

> A mutex lets you change shared state safely. A **condition variable**
> lets you **sleep until that state is the way you need it**: "until the
> queue is not empty", "until the frame is ready", "until shutdown". It is
> the standard way to wait without burning CPU, and the place where some of
> the hardest concurrency bugs come from (lost wake-ups).

---

## 1. Why this exists

A consumer thread needs the next item from a queue. Without a condition
variable it has three bad options:

| Approach | Problem |
|---|---|
| `while (empty) {}` (spin) | burns a whole A72 core and the battery, and starves the producer on a single CPU |
| `while (empty) usleep(1000);` (poll) | adds up to 1 ms latency per item, and still wakes 1000×/s for nothing |
| sleep on a semaphore | fine for *counting* items, but cannot express "wait until `x > 5 && !paused`" |

A condition variable turns "wait until *any predicate* on shared data is
true" into: sleep in the kernel, get woken by whoever changes the data,
re-check the predicate. No CPU is used while waiting, latency is one
wake-up, and the predicate can be anything you can compute under a mutex.

---

## 2. Mental model

**A doctor's waiting room with a receptionist.** The mutex is the
reception desk: one person at a time looks at the appointment book (the
shared state). If your appointment is not ready, you do not stand at the
desk (that would hold the lock). You also do not keep walking back to ask
(that would be polling). You sit in the waiting room (the condition
variable) and the receptionist calls you. When you are called you walk back
to the desk *and check the book again*, because someone else may have been
called too, or you may have misheard (a spurious wake-up).

```
              +----------------------- mutex m --------------------+
              |   shared state:  queue, count, ready, closed ...    |
              +-----------------------------------------------------+
                  ^ lock / check / change / unlock         ^
                  |                                        |
   waiter:  lock(m); while (!pred) wait(cv, m); ...; unlock(m)
                                    |      ^
                         sleep on cv|      | woken by signal/broadcast,
                    (m released     v      | m re-acquired before return
                     atomically)   [ cv: queue of sleeping threads ]
                                           ^
   signaller: lock(m); change state; signal(cv) or broadcast(cv); unlock(m)
```

**A condition variable has no memory.** It is not a flag and not a
counter. A signal with no thread waiting is lost. The *state* (your
variables) is what remembers, which is why you always check the predicate,
never "whether I was signalled".

---

## 3. How it works

### 3.1 The canonical wait loop (`c/01_condvar_basics.c`)

```c
pthread_mutex_lock(&m);
while (!predicate)                 /* WHILE, never IF */
    pthread_cond_wait(&cv, &m);    /* atomically: unlock m + sleep; relock on wake */
/* predicate is true AND we hold m: use the state */
pthread_mutex_unlock(&m);

/* the other side */
pthread_mutex_lock(&m);
make predicate true;               /* ALWAYS under the same mutex */
pthread_cond_signal(&cv);          /* or broadcast; may be after unlock */
pthread_mutex_unlock(&m);
```

### 3.2 Why `while` and not `if`: spurious and stolen wake-ups

`pthread_cond_wait` may return when the predicate is false:

1. **Spurious wake-ups.** POSIX explicitly allows them: futex wake-ups
   after signals, implementation races on multi-core. They are rare, but
   allowed, so you must handle them.
2. **Stolen wake-ups.** You were signalled because an item arrived, but
   between the signal and your re-acquiring the mutex another consumer
   (perhaps one that was not even waiting) took it.
3. **Broadcasts for other predicates.** One cv shared by waiters with
   different conditions wakes everyone.

The `while` loop handles all three by re-checking. The C++ `wait(lk, pred)`
overload is exactly this loop.

### 3.3 Why the mutex goes into `wait()`

`wait` must release the mutex **and** start sleeping as one atomic step:

```
 if it were two steps:                      what pthread_cond_wait does:
  waiter        signaller                    waiter               signaller
  lock; pred? no                             lock; pred? no
  unlock                                     [queue on cv + unlock]   (atomic)
           lock; pred=1; signal; unlock                             lock; pred=1
  sleep  ... forever (signal was lost)       ~~sleep~~ <----------- signal; unlock
                                             wake, relock, pred? yes
```

Because the waiter is already queued on the cv when the mutex is released,
a signaller who takes the mutex afterwards always finds it. On return,
`wait` re-acquires the mutex, so the re-check happens under the lock. (In
glibc since 2.25 a cv is a pair of futex-based groups with sequence numbers.
The kernel's `futex` syscall is what makes "check value and sleep" atomic,
see [06](../06-spinlocks-and-futex/README.md).)

### 3.4 Lost wake-ups (`c/02_lost_wakeup_bug.c`)

The bug class that "hangs once a week on the board". Two forms:

```
 A: check the predicate WITHOUT the lock     B: change the state WITHOUT the lock
    waiter          signaller                   waiter              signaller
    if (!ready)                                 lock; ready? 0
                    lock; ready=1;                                  ready = 1   (no lock!)
                    signal (nobody waits)                           signal      (nobody waits yet)
                    unlock                      wait(cv, m) ... forever
    lock; wait ... forever
```

The fingerprint of a hang: the thread is in `pthread_cond_wait` (gdb
`thread apply all bt`), yet the predicate variable is already true. The
`_bug` program makes the gap 200 ms wide so it fails every time, with a 2 s
safety timeout so it ends. TSan also reports the unlocked read as a data
race.

**Rule: state changes and predicate checks under the mutex, always. Wait in
a `while` loop, always.**

### 3.5 signal vs broadcast (`c/03_signal_vs_broadcast.c`)

| | `pthread_cond_signal` / `notify_one` | `pthread_cond_broadcast` / `notify_all` |
|---|---|---|
| Wakes | at least one waiter | all waiters |
| Use when | any single waiter can handle the change (one item added) | the change matters to every waiter (shutdown, latch opened, waiters with different predicates on one cv) |
| Risk | if the wrong kind of waiter gets it → stalls | thundering herd: N wake-ups, N−1 futile |

```
 broadcast, 1 item, 4 waiters:  W0 takes it.  W1, W2, W3 wake, queue on the mutex,
                                find nothing, sleep again. 3 futile context switches.
```

The program measures exactly N−1 futile wake-ups for a broadcast and
(normally) 0 for signal. Signal is not always enough: if consumers and
producers share **one** cv, a `signal` meant for a consumer can wake a
producer, which goes back to sleep, and the wake-up is gone. That is why
the bounded buffer below has **two** cvs.

Signal **while holding** the mutex or **after** unlocking? Both are
correct as long as the *state change* was under the mutex. Signalling after
unlocking avoids "hurry up and wait" (the woken thread immediately blocks on
the mutex you still hold). Signalling inside is simpler to reason about,
and required if the cv itself might be destroyed by the woken thread right
after unlock.

### 3.6 One-shot event and countdown latch

A cv alone forgets; **cv + state** remembers. A latch is a counter plus a
cv: `count_down()` decrements and broadcasts at zero, `wait()` sleeps while
`count > 0`. With count = 1 it is a one-shot **event** (a start gate). Once
open it stays open, so a late arriver passes straight through, which a bare
signal cannot do. C++20 has `std::latch`. In C++17 you write it with a
mutex and a cv (`c/01` part 2).

### 3.7 Bounded buffer with two condition variables (`c/04_mpmc_bounded_queue.c`, `cpp/02_blocking_queue.cpp`)

```
  producers --push--> [ mutex | buf[cap], head, tail, n, closed ] --pop--> consumers
                         not_full: producers sleep here while n == cap
                         not_empty: consumers sleep here while n == 0

  push: while (n == cap && !closed) wait(not_full);  if closed -> false
        store; n++; signal(not_empty)
  pop : while (n == 0 && !closed) wait(not_empty);   if n == 0 -> false (closed+drained)
        take;  n--; signal(not_full)
  close: closed = 1; broadcast(not_full); broadcast(not_empty)
```

[dsa/19 `c/03_ring_buffer_producer_consumer.c`](../../dsa/19-system-design-structures/README.md)
has the same ring with one producer. This lesson's version is **MPMC** and
tests the hard part, **shutdown**: a producer asleep on a full queue must
be woken by `close()` and get `false`; consumers drain what is left and
then see end-of-stream. Without `closed` in the predicate, `close()` would
wake threads that immediately go back to sleep, and `join()` hangs. The
lock-free alternative for exactly one producer and one consumer is in
[dsa/07 §3.8](../../dsa/07-stacks-and-queues/README.md).

### 3.8 Timed waits (`c/05_timed_wait_monotonic.c`)

`pthread_cond_timedwait(&cv, &m, &abs)` takes an **absolute** deadline on
the cv's clock, which is **CLOCK_REALTIME by default**. Fix it once at init:

```c
pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
pthread_cond_init(&cv, &attr);
deadline = now(MONOTONIC) + timeout;          /* compute ONCE, before the loop */
while (!pred && rc != ETIMEDOUT) rc = pthread_cond_timedwait(&cv, &m, &deadline);
ok = pred;                                    /* trust the predicate, not rc */
```

Computing the deadline once means spurious wake-ups do not extend the total
wait. A relative timeout re-armed every loop would. glibc ≥ 2.30 also has
`pthread_cond_clockwait(cv, m, CLOCK_MONOTONIC, &abs)`. C++
`wait_for`/`wait_until(steady_clock)` use the monotonic clock.

### 3.9 Kernel side: wait queues

In the kernel the same idea is a **wait queue**:
`wait_event_interruptible(wq, condition)` is the `while (!cond) sleep` loop,
and `wake_up(&wq)` is the signal. The lost-wake-up problem is solved in the
kernel by setting the task state *before* re-checking the condition
(`prepare_to_wait` → check → `schedule()`). See
[linux-device-drivers/01-kernel-synchronization](../../linux-device-drivers/01-kernel-synchronization/README.md).

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `pthread_cond_signal` with no waiters | ~10–20 ns, no syscall | glibc checks the waiter count in user space |
| `pthread_cond_signal` with waiters | 1 `futex(FUTEX_WAKE)` syscall | wakes one sleeping thread |
| `pthread_cond_broadcast` with N waiters | 1 syscall + N wake-ups + N mutex re-acquisitions | thundering herd: O(N) context switches |
| `pthread_cond_wait` | unlock + futex wait + relock: 2 context switches per wake-up | sleeps in the kernel |
| push/pop on the bounded queue | O(1) + lock; may block | ring indices |
| Memory | `pthread_cond_t`: 48 B | sequence counters, group refs, futex words |

---

## 5. C vs C++

| Concept | C (pthreads) | C++17 |
|---|---|---|
| Type | `pthread_cond_t` + `pthread_mutex_t` | `std::condition_variable` + `std::mutex` |
| Wait loop | `while (!p) pthread_cond_wait(&cv, &m);` | `cv.wait(lk, [&]{ return p; });` |
| Timed | `pthread_cond_timedwait` (abs, cv's clock); `condattr_setclock(MONOTONIC)` | `wait_for(lk, 50ms, pred)` / `wait_until(lk, tp, pred)` → `bool` |
| Wake | `pthread_cond_signal` / `broadcast` | `notify_one()` / `notify_all()` |
| Lock type | any `pthread_mutex_t` | `std::unique_lock<std::mutex>` only; `std::condition_variable_any` for any lockable (e.g. `std::shared_lock`) |
| Latch | mutex + cv + count (`c/01`) | C++20 `std::latch`; C++17: write it |
| Queue | `c/04` struct + functions | `BlockingQueue<T>` template, `std::optional<T>` for end-of-stream, move-only items |

`condition_variable_any` is slower: internally it has its own mutex and a
`condition_variable`, and it locks and unlocks your lock around the wait.

**TSan note (GCC 11 / Ubuntu 22.04):** libstdc++ implements
`steady_clock` waits (`wait_for`, `wait_until(steady_clock)`) with
`pthread_cond_clockwait`, and GCC 11's ThreadSanitizer does not intercept
it. When another thread wakes you, TSan reports false "double lock" and
data-race warnings. The C++ programs here use `steady_clock` normally and
switch a `WaitClock` alias to `system_clock` only under TSan. Waits that
only time out are unaffected.

---

## 6. Where it is used in the real world

- **Linux kernel.** Wait queues (`wait_event*`, `wake_up*`) are the kernel's
  condition variables; `struct completion` (`wait_for_completion`,
  `complete`) is the one-shot event / latch. Every blocking `read()` on a
  driver sleeps in one. Kernel side:
  [linux-device-drivers/01-kernel-synchronization](../../linux-device-drivers/01-kernel-synchronization/README.md).
- **glibc.** `nptl/pthread_cond_wait.c`, the 2017 rewrite (by Torvald
  Riegel) that fixed an ordering bug in the old algorithm: waiters are
  split into two groups with 64-bit sequence counters. It is a good,
  long comment to read.
- **GStreamer.** `GstQueue` (the `queue` element) is a bounded buffer with
  `GMutex` + two `GCond`s (`item_add`, `item_del`), and "flushing" is its
  `close()`. `GstBus` and `GAsyncQueue` are blocking queues. See
  [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md).
- **Thread pools and executors.** Job queue = bounded buffer, shutdown =
  close + broadcast ([08](../08-barriers-once-and-thread-pools/README.md)).
- **Embedded/RTOS.** FreeRTOS has no condition variable. You use
  queues, event groups (`xEventGroupWaitBits`, wait for a predicate on bits)
  or task notifications. Zephyr has `k_condvar`. A semaphore can count
  items, but cannot wait for an arbitrary predicate.
- **Databases, web servers, loggers.** Async log writers (spdlog's async
  logger, the `glog` writer thread) drain a bounded queue fed by many
  threads. Connection pools wait for "a connection is free".

---

## 7. Common mistakes and pitfalls

1. **`if` instead of `while`.** It breaks on spurious, stolen and broadcast
   wake-ups.
2. **Predicate checked or changed without the mutex.** This causes lost
   wake-ups and is also a data race. TSan finds the race:
   `make run P=linux-internals/04-condition-variables/c/02_lost_wakeup_bug.c SAN=tsan`.
3. **Treating a signal as an event that is remembered.** It is not. Put
   the fact in a variable.
4. **One cv for two different predicates with `signal`.** The wrong waiter
   wakes and the right one never hears it. Use two cvs, or broadcast.
5. **Shutdown that does not wake sleepers.** `closed` must be part of every
   wait predicate, and `close()` must **broadcast** on every cv.
6. **REALTIME deadlines.** Use `pthread_condattr_setclock(CLOCK_MONOTONIC)`
   or `steady_clock`. Compute the deadline once, outside the loop.
7. **Relative timeout in a loop** (`wait_for(100ms)` inside your own
   `while`): each spurious wake-up restarts the 100 ms. Use the predicate
   overload or an absolute deadline.
8. **Waiting with a recursive mutex locked twice.** `wait` releases one
   level, so the signaller can never get the lock. That is a deadlock.
9. **Destroying a cv while threads are still waiting on it**, or a cv on
   the stack of a function that returns while others still use it.
10. **Holding the lock while doing the slow work** after the wait. Copy or
    move the item out, unlock, then process it.
11. **Thundering herd by habit.** `notify_all` "to be safe" on every push
    costs N context switches per item under load.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_condvar_basics.c](c/01_condvar_basics.c) | worker → main handoff with the `while` loop; why the mutex goes into wait (diagram); latch and one-shot start gate | `make run P=linux-internals/04-condition-variables/c/01_condvar_basics.c` |
| [c/02_lost_wakeup_bug.c](c/02_lost_wakeup_bug.c) | **BUG**: predicate checked outside the lock → signal lost, waiter times out with `ready == 1`; TSan reports the race | `make run P=linux-internals/04-condition-variables/c/02_lost_wakeup_bug.c` |
| [c/03_signal_vs_broadcast.c](c/03_signal_vs_broadcast.c) | counts wake-ups and futile wake-ups: signal (0 futile) vs broadcast (N−1, thundering herd); shutdown needs broadcast | `make run P=linux-internals/04-condition-variables/c/03_signal_vs_broadcast.c` |
| [c/04_mpmc_bounded_queue.c](c/04_mpmc_bounded_queue.c) | 3 producers, 3 consumers, 8-slot queue, two cvs, `close()`; every item exactly once; close wakes a blocked producer | `make run P=linux-internals/04-condition-variables/c/04_mpmc_bounded_queue.c` |
| [c/05_timed_wait_monotonic.c](c/05_timed_wait_monotonic.c) | `condattr_setclock(CLOCK_MONOTONIC)`, deadline computed once, timeout never early, `pthread_cond_clockwait` (guarded) | `make run P=linux-internals/04-condition-variables/c/05_timed_wait_monotonic.c` |
| [cpp/01_condition_variable.cpp](cpp/01_condition_variable.cpp) | `wait(lk, pred)`, `wait_for`, `wait_until`, `notify_all` generation barrier, `condition_variable_any` with a custom lock | `make run P=linux-internals/04-condition-variables/cpp/01_condition_variable.cpp` |
| [cpp/02_blocking_queue.cpp](cpp/02_blocking_queue.cpp) | `BlockingQueue<T>` template: `push`, `pop` → `optional`, `try_pop_for`, `close`; `unique_ptr` items | `make run P=linux-internals/04-condition-variables/cpp/02_blocking_queue.cpp` |

Run any of them under ThreadSanitizer by adding `SAN=tsan`:

```bash
make run P=linux-internals/04-condition-variables/c/04_mpmc_bounded_queue.c SAN=tsan
TSAN_OPTIONS=halt_on_error=1 make run P=linux-internals/04-condition-variables/c/02_lost_wakeup_bug.c SAN=tsan   # reports the race
```

---

## 9. Exercises

**Easy**
1. In `c/01`, replace `while` with `if`. Does it still pass? Why does
   that prove nothing?
2. Add `size_t q_size(Queue *)` to `c/04`. Why must it lock even though it
   only reads?
3. Change `c/03` phase 1 to use `broadcast` and compare the futile counts.

**Medium**
4. Rewrite `c/04` with **one** cv used for both directions and `signal`.
   Make producers and consumers unbalanced (1 producer, 4 consumers,
   capacity 1) and find the stall. Then fix it with broadcast and measure the
   extra wake-ups.
5. Add `q_push_timeout(q, x, ms)` to `c/04` using a monotonic cv.
6. Implement a reusable **barrier** (N threads, many rounds) with a
   generation counter, like `cpp/01` part 3. Why is the generation needed?
   (Compare with `pthread_barrier_t` in
   [08](../08-barriers-once-and-thread-pools/README.md).)

**Hard**
7. Add `pop_batch(out, max)` to `BlockingQueue` that takes up to `max`
   items in one lock acquisition. Measure the throughput gain with
   `OPT=1 SAN=0`.
8. Build a **priority** blocking queue (`std::priority_queue` inside) with
   `close()`. Then a **leaky** one that drops the oldest item when full,
   like GStreamer `queue leaky=downstream`.
9. Read glibc's `nptl/pthread_cond_wait.c` header comment and explain in
   five sentences why a waiter that arrives *after* a signal must not be
   able to consume it.

---

## 10. Remember forever

```
cv = a place to SLEEP until a predicate on mutex-protected state is true; it has NO memory
lock(m); while (!pred) wait(cv, m); use; unlock(m)       <- never `if`
change state UNDER m, then signal/broadcast (inside or right after unlock)
wait() = atomic {unlock m + sleep}, relocks before returning -> no lost wake-up
spurious + stolen wake-ups exist -> the while loop handles both
signal: one waiter can use it.  broadcast: shutdown / latch / mixed predicates (herd: N-1 futile)
bounded buffer: 2 cvs (not_full, not_empty) + closed flag in BOTH predicates + broadcast on close
timeouts: condattr_setclock(MONOTONIC) / steady_clock, absolute deadline computed ONCE
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why does a condition variable need a mutex at all? What would go wrong
   with a cv that had none?
2. Draw the lost-wake-up interleaving for "check without the lock". Then
   draw why `pthread_cond_wait`'s atomic unlock-and-sleep prevents it.
3. Give three reasons a `wait()` can return while the predicate is false.
4. When is `signal` wrong and `broadcast` required? When is `broadcast`
   wasteful?
5. Why does the bounded buffer use two cvs? What breaks with one cv and
   `signal`?
6. Why must `closed` be part of every wait predicate in the queue?
7. What is wrong with `pthread_cond_timedwait` with a default cv on a board
   that gets NTP time 30 s after boot?
8. Why compute the deadline before the loop instead of using a relative
   timeout each time?
9. What is a latch, and why can it not be built from a cv alone?
10. What does `wait_event_interruptible(wq, cond)` in the kernel
    correspond to in pthreads?
11. Why can a woken thread immediately block again if you signal while
    holding the mutex? Is that a correctness problem?
