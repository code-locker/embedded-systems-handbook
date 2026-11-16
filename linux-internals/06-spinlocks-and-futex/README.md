# 06 — Spinlocks and futex

> Every lock has to answer one question: **what does a thread do while it
> waits?** A spinlock keeps the CPU and loops. A sleeping lock asks the
> kernel to park the thread until the lock is free. The **futex** is the
> Linux system call that makes the sleeping kind almost as cheap as the
> spinning kind when nobody is waiting. Every `pthread_mutex_t`,
> `std::mutex`, condition variable and semaphore in glibc is built on it.

---

## 1. Why this exists

Lesson [03](../03-mutexes-and-deadlock/README.md) used `pthread_mutex_t`
as a black box. This lesson opens the box. Two costs compete:

| Waiting strategy | Cost per wait | Good when |
|---|---|---|
| **Spin** (loop on an atomic) | burns a core for the whole wait; hand-over ~50–200 ns | the owner is *running on another core* and will release within ~1 µs |
| **Sleep** (futex wait) | two context switches, ~2–10 µs; the core is free for others meanwhile | the wait may be long, or the owner may not be running |

The kernel spins all the time (`spin_lock()` in interrupt handlers, where
sleeping is not allowed). User space almost never should, and you need to
understand why before choosing. You also need to know how a mutex can be
*one atomic instruction* when uncontended, since that is why "just use a
mutex" is good advice.

On the BeagleBone AI-64 this matters twice: there are only **two** A72
cores (a spinning waiter takes half the machine away), and the A72 is
**weakly ordered**, so a hand-written lock without the right
acquire/release ordering *will* break there even if it "works" on x86.

---

## 2. Mental model

**Spinlock = waiting at a locked toilet door, trying the handle again and
again.** Fine if the person inside is just washing their hands. Silly if
they are reading a newspaper, and absurd if the only way they can get out
is for you to leave the corridor (one core!).

**Futex = a waiting room with a buzzer.** You try the door once (atomic
compare-and-swap, no kernel). If it is locked, you write "someone is
waiting" on the door sign and sit in the waiting room (`FUTEX_WAIT`). The
person leaving checks the sign: if nobody wrote on it, they just leave
(no syscall); if someone did, they press the buzzer (`FUTEX_WAKE`).

```
   lock word in YOUR memory (user space)          kernel
   +---------------------------+
   | int state = 0 / 1 / 2     | ----addr----> hash bucket -> wait queue
   +---------------------------+                             [B] -> [C]
     ^  atomic CAS / xchg        FUTEX_WAIT(addr, 2): sleep if *addr == 2
     |  (fast path, no kernel)   FUTEX_WAKE(addr, 1): wake one sleeper
   threads A, B, C
```

The kernel never knows "the lock is held". It only keeps a **queue of
sleepers per address**. All the lock logic lives in user space.

---

## 3. How it works

### 3.1 Spinlock vs sleeping lock  (`c/01_pthread_spinlock.c`)

```
   SPINLOCK                              MUTEX (sleeping lock)
   core0  A: [lock]--cs--[unlock]        core0  A: [lock]--cs--[unlock]+wake(B)
   core1  B:   spin spin spin [lock]     core1  B:  futex_wait -> sleep ... ctx
                              ^ ~50 ns                 switch back in, ~2-10 us
```

Spinning wins only if **all three** hold:

1. The critical section is very short (tens of instructions, no syscalls,
   no allocation, no I/O).
2. There is more than one core. On one core the owner cannot run while
   you spin, so every cycle you spin is a cycle it cannot use to finish.
3. The owner is not preempted while holding the lock.

Point 3 is the **user-space preemption problem**. The kernel's
`spin_lock()` disables preemption (and `spin_lock_irqsave()` also disables
local interrupts), so the owner runs until it unlocks. A user-space thread
*cannot* disable preemption. If the scheduler takes the CPU away from the
owner in the middle of its critical section, every waiter spins for a full
time slice (milliseconds) waiting for a thread that is not even running.
The same happens with virtual CPUs ("lock-holder preemption" in VMs).

`pthread_spinlock_t` API:

```c
pthread_spinlock_t s;
pthread_spin_init(&s, PTHREAD_PROCESS_PRIVATE);  /* or _SHARED for shm  */
pthread_spin_lock(&s);      /* spins until it gets it                   */
pthread_spin_trylock(&s);   /* 0 or EBUSY, never waits                  */
pthread_spin_unlock(&s);
pthread_spin_destroy(&s);
```

It is not recursive: locking it twice from the same thread spins forever.
It has no owner, no priority inheritance and no deadlock detection.

### 3.2 Building a spinlock: TAS  (`c/02_tas_ttas_backoff.c`)

A spinlock is one atomic word and two memory orderings:

```c
lock:    while (atomic_flag_test_and_set_explicit(&f, memory_order_acquire)) ;
unlock:  atomic_flag_clear_explicit(&f, memory_order_release);
```

- **acquire** on lock: no load or store of the critical section can move
  *above* the lock. On ARMv8 the exchange becomes `LDAXR/STXR` (or
  `SWPA` with LSE atomics, which the A72 does not have: it is ARMv8.0).
- **release** on unlock: every write in the critical section is visible
  to the next owner *before* it sees the lock free (`STLR` on ARMv8).

```
   owner A:  [writes data] ---- release store(lock=0)
                                        |  synchronises-with
   next B:                   acquire xchg(lock) sees 0 ---- [reads data]
```

`atomic_flag` is the only C11/C++11 type guaranteed lock-free, which is
why it is the textbook building block.

### 3.3 TTAS and MESI: why spinning on a load is better

Cache coherence (MESI) in one table. Each core holds each 64-byte line in
one of four states:

| State | Meaning | To write it... |
|---|---|---|
| **M**odified | only my copy, dirty | just write |
| **E**xclusive | only my copy, clean | just write (goes to M) |
| **S**hared | others may have copies | invalidate all other copies first (bus traffic) |
| **I**nvalid | no valid copy | fetch it, and invalidate others to write |

A TAS waiter performs a *write* (`xchg`) on every spin, so the lock's line
bounces between all waiting cores, and the owner's unlock store has to
fight its way through that traffic:

```
   TAS, 3 waiters                         TTAS, 3 waiters
   core1 xchg  -> line M on core1         core1 load -> S   \
   core2 xchg  -> steal, M on core2       core2 load -> S    > spin in own cache,
   core3 xchg  -> steal, M on core3       core3 load -> S   /  zero bus traffic
   core1 xchg  -> steal ...               owner unlocks: ONE invalidation,
   (line ping-pongs every iteration)      then one xchg race
```

**Test-and-test-and-set** spins on a plain (relaxed) load and only tries
the exchange when the lock *looks* free. The program counts atomic RMW
attempts per acquisition: TAS is typically 3–8, TTAS 1.5–2, with backoff
close to 1.

### 3.4 Backoff and the CPU relax hint

After losing the exchange race, wait `delay` iterations and double
`delay` (up to a cap). Waiters spread out in time and fewer of them
stampede at each unlock (the same idea as Ethernet's exponential backoff).
Inside the wait, tell the core you are spinning:

```c
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();                       /* x86 PAUSE           */
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");   /* ARM YIELD hint       */
#else
    sched_yield();                                /* portable fallback   */
#endif
}
```

`PAUSE` lowers power, avoids a pipeline flush when the loop exits and
gives the hyper-thread sibling the core. `YIELD` on ARM is a hint to the
core (and to a hypervisor). After a long spin, a user-space lock should
call `sched_yield()`: the owner is probably preempted.

### 3.5 Ticket lock: fairness  (`c/03_ticket_lock.c`, `cpp/02_ticket_lock.cpp`)

TAS/TTAS are unfair: the thread that just unlocked still has the line in
its cache and often wins again. A **ticket lock** is the bakery
"take a number" machine:

```
   next  = 5   ticket dispenser          lock():   my = fetch_add(&next, 1)
   owner = 3   "now serving"                       while (owner != my) spin
   waiters hold 4 (and 3 is inside)      unlock(): owner = owner + 1  (release)
```

Strict FIFO, no starvation. The C program proves FIFO deterministically
(A takes its ticket before B, so A is served first) and measures the
longest run of same-thread acquisitions (1–3 with tickets; hundreds with
TTAS). Linux used ticket spinlocks on x86 (2.6.25–4.1) and ARM (until
4.18): `arch_spinlock_t` was `{ u16 owner; u16 next; }` in one word, which
is what `cpp/02` imitates with 16-bit counters that wrap.

Two costs: all waiters spin on the same `owner` line (N cache misses per
hand-over), and fairness is a liability when a waiter is preempted. If the
thread with the *next* ticket is not running, nobody can proceed, even if
the lock is free. The benchmark's one-core scenario shows this.

### 3.6 MCS and qspinlock  (`c/04_mcs_lock.c`)

MCS (Mellor-Crummey and Scott, 1991) queues waiters in a linked list.
Each waiter spins on a flag **in its own node**, and the unlocker writes
only its successor's flag: O(1) cache traffic per hand-over, however many
cores.

```
   lock->tail -------------------------------------------+
                                                         v
   [ A: owner ] --next--> [ B: locked=1 ] --next--> [ C: locked=1 ]
                            spins on B.locked         spins on C.locked

   lock(me):   me.next=NULL; me.locked=1; prev = xchg(&tail, me)
               if prev: prev->next = me; while (me.locked) spin
   unlock(me): if !me.next:
                   if cas(&tail, me -> NULL): return     # truly alone
                   while !me.next: spin                  # joiner mid-link
               me.next->locked = 0                       # direct hand-over
```

The subtle window: between `xchg(&tail)` and `prev->next = me`, the new
waiter is in the queue but not linked yet. That is why unlock waits for
`next` when its CAS fails.

Linux's **qspinlock** (`kernel/locking/qspinlock.c`, the spinlock on x86
since 4.2 and arm64 since 4.18) squeezes MCS into a 4-byte word: a locked
byte, a pending bit for the first waiter, and a tail encoded as
(CPU number, nesting level). MCS nodes are per-CPU: 4 per CPU, one each
for task, softirq, hardirq and NMI context.

### 3.7 Futex  (`c/05_futex_basics.c`)

```c
long futex_wait(atomic_int *a, int expected, const struct timespec *rel) {
    return syscall(SYS_futex, a, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, rel, NULL, 0);
}
long futex_wake(atomic_int *a, int n) {
    return syscall(SYS_futex, a, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, NULL, NULL, 0);
}
```

- `FUTEX_WAIT(addr, val)`: atomically *in the kernel*, check `*addr == val`;
  if not, return `EAGAIN` at once; if so, sleep on the queue for `addr`.
  It may also return `ETIMEDOUT`, `EINTR`, or spuriously. **Always
  re-check the word in a loop.**
- `FUTEX_WAKE(addr, n)`: wake up to `n` sleepers (`INT_MAX` = all), returns
  how many it woke.
- `FUTEX_PRIVATE_FLAG`: only threads of this process use the word, so the
  kernel can hash (mm, address) instead of looking up the backing page.
  Leave it out for a futex in shared memory between processes (lesson 11).

The `expected` argument closes the lost-wakeup race:

```
   waiter                              waker
   v = flag;         // 0
                                       flag = 1;
                                       futex_wake(&flag)   // nobody asleep yet
   sleep(&flag)      // forever!       <- with plain "sleep"
   futex_wait(&flag, 0)  -> EAGAIN     <- futex re-checks flag==0 under its lock
```

Other operations you will see in glibc and the kernel: `FUTEX_WAIT_BITSET`
(absolute timeouts, used by condition variables), `FUTEX_REQUEUE` /
`FUTEX_CMP_REQUEUE` (move sleepers from a condvar to a mutex without
waking them all), `FUTEX_LOCK_PI` / `FUTEX_UNLOCK_PI` (priority
inheritance, lesson 03), and Linux 5.16's `futex_waitv` (wait on several
futexes, for Wine/Proton).

### 3.8 A futex mutex from scratch  (`c/06_futex_mutex.c`, `cpp/03_futex_mutex.cpp`)

Ulrich Drepper's *Futexes Are Tricky*, "mutex 3". The word has three
states: **0** free, **1** locked and nobody waiting, **2** locked and maybe
someone waiting.

```c
void lock(m) {
    int c = 0;
    if (cmpxchg(&m->state, &c, 1))            return;  /* fast path      */
    if (c != 2) c = xchg(&m->state, 2);                 /* announce waiter*/
    while (c != 0) {
        futex_wait(&m->state, 2);                       /* sleep if still 2 */
        c = xchg(&m->state, 2);
    }
}
void unlock(m) {
    if (fetch_sub(&m->state, 1) != 1) {   /* it was 2: there may be sleepers */
        m->state = 0;
        futex_wake(&m->state, 1);
    }
}
```

```
   A: cmpxchg 0->1 ok            state 1   ........ critical section ........
   B:    cmpxchg fails (c=1) -> xchg -> 2 (state 2) -> futex_wait(2): sleeps
   A: unlock: fetch_sub returned 2 -> state = 0 -> futex_wake(1)
   B:    wakes -> xchg(2) returns 0 -> B owns it (state 2, pessimistic)
   B: unlock: fetch_sub returned 2 -> state 0, futex_wake (nobody: wasted, harmless)
```

Why the third state? A 0/1 lock must call `futex_wake` on *every* unlock
(it cannot know whether anyone sleeps): a syscall each time, about 100x
slower than an atomic. Why does a woken waiter set 2, not 1? It cannot
know whether other sleepers remain; setting 1 could leave them asleep
forever. The program asserts that 100,000 uncontended lock/unlock pairs
make **zero** syscalls.

This is what glibc does: `lll_lock` in `sysdeps/nptl/lowlevellock.h` is
this algorithm, and `pthread_mutex_lock` for a normal mutex is a thin
layer on top. So `std::mutex` on Linux is, roughly, this program.

**ThreadSanitizer and futexes:** TSan cannot see inside the `futex`
syscall, so it cannot know that `FUTEX_WAKE` happens-before a waiter's
return. This lock needs no such knowledge: ownership always passes
through an acquire RMW on the word that reads the value written by a
release in `unlock()`, so TSan sees the happens-before edge and stays
quiet. If you write a futex lock that hands ownership *only* through the
syscall, TSan reports false races; the fix is to make the hand-over
visible through atomics, not to suppress the report.

### 3.9 Adaptive mutexes

`PTHREAD_MUTEX_ADAPTIVE_NP` (glibc extension) spins a bounded number of
times on contention before sleeping. The limit adapts per lock to recent
history and is capped by the tunable `glibc.pthread.mutex_spin_count`
(default 100). `cpp/03_futex_mutex.cpp` does the same with a fixed 100
spins. This combines the strengths of both: no context switch for very
short waits, no wasted time slice for long ones. The kernel's `struct mutex`
does this too ("optimistic spinning": spin *while the owner is running on
another CPU*, which only the kernel can know).

```c
pthread_mutexattr_t a;
pthread_mutexattr_init(&a);
pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ADAPTIVE_NP);
pthread_mutex_init(&m, &a);
```

### 3.10 Benchmark: when does spinning win?  (`c/07_lock_benchmark.c`)

Six locks, three scenarios, two columns: **ns per lock+unlock** (wall
clock) and **cores busy** (CPU time ÷ wall time, from `getrusage`).

```
   A. short CS, one thread per core   spinlocks often fastest
   B. long CS (~µs)                   spinlocks maybe fast in wall time, but
                                      "cores busy" ~= threads: CPU stolen
   C. 2 threads pinned to ONE core    spinning cannot help; ticket lock often
                                      10-50x slower (the next ticket holder
                                      must be scheduled first)
```

Numbers depend on the machine, load and sanitizer. For meaningful numbers:
`make run P=linux-internals/06-spinlocks-and-futex/c/07_lock_benchmark.c SAN=0 OPT=1`.
Run it on the BeagleBone AI-64 too (2 cores: scenario A uses 2 threads).

---

## 4. Complexity table

| Lock | Uncontended lock+unlock | Hand-over under contention | Cache traffic per hand-over | Fair? | Memory |
|---|---|---|---|---|---|
| TAS (`atomic_flag`) | 1 RMW + 1 store | spin | O(waiters) per *spin* | no | 1 byte |
| TTAS (+ backoff) | 1 RMW + 1 store | spin | O(waiters) per unlock | no | 1 byte |
| Ticket | 1 RMW + 1 store | spin | O(waiters) per unlock | FIFO | 2 counters |
| MCS / qspinlock | 1 xchg + 1 CAS | spin on own node | O(1) | FIFO | 1 pointer + node per waiter |
| `pthread_spinlock_t` | 1 RMW + 1 store | spin | like TTAS | no | 4 bytes |
| Futex mutex (3-state) | 1 CAS + 1 RMW, **0 syscalls** | sleep: 2 syscalls + 2 context switches | O(1) | no (barging) | 4 bytes |
| `pthread_mutex_t` normal | same as futex mutex | sleep | O(1) | no | 40 bytes (x86-64), 48 (aarch64) |
| Adaptive mutex | same | spin ≤ ~100, then sleep | | no | same |

"Barging": an unlocking thread or a newcomer can grab the lock before the
woken waiter gets to run. Unfair, but it keeps throughput high.

---

## 5. C vs C++

| Concept | C | C++17 |
|---|---|---|
| Ready-made spinlock | `pthread_spinlock_t` | none in the standard (Boost has `boost::detail::spinlock`); write one |
| Atomic flag | `atomic_flag`, `atomic_flag_test_and_set_explicit` | `std::atomic_flag::test_and_set(order)`; C++20 adds `test()`, `wait()`, `notify_one()` |
| Atomic bool / counter | `atomic_bool`, `atomic_exchange_explicit` | `std::atomic<bool>::exchange(v, order)` |
| RAII lock | no (write `lock` / `unlock` by hand, or `goto out`) | `std::lock_guard`, `std::unique_lock`, `std::scoped_lock` work with **any** type that has `lock()`/`unlock()` (BasicLockable), `+ try_lock()` (Lockable) |
| Wait on a custom lock | not possible with `pthread_cond_t` | `std::condition_variable_any` |
| Futex | `syscall(SYS_futex, ...)` | same; C++20 `std::atomic<T>::wait/notify_*` are futex-based in libstdc++ |
| Sleeping mutex | `pthread_mutex_t` | `std::mutex` (wraps `pthread_mutex_t`) |

`cpp/01_spinlock_lockable.cpp` shows the payoff of the Lockable concept:
`std::scoped_lock(a.lock, b.lock)` gives deadlock-free two-lock transfers
with a hand-written spinlock, because `scoped_lock` uses `try_lock` to back
off. That only works because the class provides `try_lock()`.

---

## 6. Where it is used in the real world

- **Linux kernel.** `spinlock_t` is everywhere in drivers: protecting
  register access and data shared with an interrupt handler.
  `spin_lock_irqsave()` disables local interrupts so the ISR on *this* CPU
  cannot deadlock against the code it interrupted; `raw_spinlock_t` stays a
  real spinlock even on PREEMPT_RT, where `spinlock_t` becomes a sleeping
  rt-mutex. The full interrupt-context story is in
  [linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md).
  The arm64 spinlock is the qspinlock (MCS) since 4.18; before that it was
  a ticket lock using `LDAXR`/`STXR` and `WFE`/`SEV` to sleep the core
  while waiting.
- **glibc.** `pthread_mutex_lock`, `pthread_cond_wait`, `sem_wait`,
  `pthread_rwlock_*`, `pthread_barrier_wait` and `pthread_once` all end in
  `futex()` syscalls on their slow path. `pthread_spin_lock` is a TTAS
  loop on one int.
- **C++ standard library.** `std::mutex` = `pthread_mutex_t`; libstdc++'s
  C++20 `std::atomic::wait` = futex; `std::shared_ptr`'s atomic overloads
  use a small pool of spinlocks/mutexes.
- **GStreamer / GLib.** `GMutex` on Linux is a futex mutex built exactly
  like section 3.8 (`glib/gthread-posix.c`, `g_mutex_lock_slowpath`).
  `g_bit_lock` packs a lock into one bit of an int, with futex wait/wake.
  GStreamer uses `GST_OBJECT_LOCK` (a `GMutex`) on every element, see
  [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md).
- **Real-time audio/video** (JACK, PipeWire): the real-time thread must
  never sleep on a lock that a normal thread holds (priority inversion),
  so they use lock-free rings like
  [dsa/07 §3.8](../../dsa/07-stacks-and-queues/README.md) and
  `trylock`-or-skip, never a plain spinlock.
- **Embedded/RTOS.** FreeRTOS on one core has no spinlocks at all: a
  critical section is `taskENTER_CRITICAL()` (mask interrupts). The SMP
  FreeRTOS port (and Zephyr's `k_spinlock`) add real spinlocks between
  cores. On the TDA4VM, the A72 and the R5F cores coordinate through the
  **hardware spinlock** module (`drivers/hwspinlock/omap_hwspinlock.c`,
  `hwspin_lock_timeout()`), since they do not share coherent caches.
- **Databases and runtimes.** PostgreSQL's `s_lock` (TAS + backoff +
  `pg_usleep`), the JVM's thin locks, Go's `sync.Mutex` (spin then park).

---

## 7. Common mistakes and pitfalls

1. **Check-then-set in two steps** (`while (lock) ; lock = 1;`): both
   threads see 0 and both enter. Needs *one* atomic RMW. See
   `c/08_naive_spinlock_bug.c`.
2. **`volatile int` as a lock.** `volatile` stops the compiler caching the
   value but provides no atomicity and no ordering between cores. On the
   A72 the data written in the critical section may become visible *after*
   the unlock.
3. **Relaxed ordering on lock/unlock.** The counter test may still pass on
   x86 (strong ordering). On ARM it fails. Lock = acquire, unlock = release.
4. **Spinning in user space on a single core**, or with more spinning
   threads than cores. Pure loss; use a mutex.
5. **Long or blocking work inside a spinlock**: `printf`, `malloc`, file
   I/O, another lock that sleeps. The waiters burn CPU the whole time.
6. **Locking a spinlock twice** in the same thread: spins forever (not
   recursive, no owner).
7. **Fair spinlocks with preemption**: a preempted ticket-holder blocks
   everyone behind it. Yield after a while, or do not spin in user space.
8. **`futex_wait` without a loop.** It returns on `EAGAIN`, `EINTR`,
   timeouts and spuriously. Only the value of the word is the truth.
9. **A 2-state futex lock that skips the wake** ("I set it to 0, if
   someone is waiting they will notice"): they will not, they are asleep.
   This is the lost-wakeup bug from *Futexes Are Tricky*.
10. **Mixing `FUTEX_PRIVATE_FLAG` and shared memory**: a process-shared
    futex must *not* use the private flag, or wakes never reach the other
    process.
11. **False sharing**: two hot locks (or a lock and unrelated hot data) in
    one 64-byte line slow each other down. `alignas(64)`.
12. **Trusting x86 testing.** Run TSan:
    `make run P=linux-internals/06-spinlocks-and-futex/c/02_tas_ttas_backoff.c SAN=tsan`,
    then run the program on the board.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_pthread_spinlock.c](c/01_pthread_spinlock.c) | `pthread_spin_init/lock/trylock/unlock`, EBUSY, counter under contention | `make run P=linux-internals/06-spinlocks-and-futex/c/01_pthread_spinlock.c` |
| [c/02_tas_ttas_backoff.c](c/02_tas_ttas_backoff.c) | TAS with `atomic_flag`, TTAS, TTAS + exponential backoff, `cpu_relax()`; counts RMW attempts per lock | `make run P=linux-internals/06-spinlocks-and-futex/c/02_tas_ttas_backoff.c` |
| [c/03_ticket_lock.c](c/03_ticket_lock.c) | Ticket lock, deterministic FIFO test, fairness measurement, counter wrap-around | `make run P=linux-internals/06-spinlocks-and-futex/c/03_ticket_lock.c` |
| [c/04_mcs_lock.c](c/04_mcs_lock.c) | MCS queue lock (the idea behind the kernel's qspinlock), mutual exclusion verified | `make run P=linux-internals/06-spinlocks-and-futex/c/04_mcs_lock.c` |
| [c/05_futex_basics.c](c/05_futex_basics.c) | Raw `SYS_futex`: EAGAIN on mismatch, ETIMEDOUT, wake counts, a one-shot event | `make run P=linux-internals/06-spinlocks-and-futex/c/05_futex_basics.c` |
| [c/06_futex_mutex.c](c/06_futex_mutex.c) | Drepper's 3-state futex mutex; 0 syscalls uncontended; counter under contention | `make run P=linux-internals/06-spinlocks-and-futex/c/06_futex_mutex.c` |
| [c/07_lock_benchmark.c](c/07_lock_benchmark.c) | mutex vs adaptive mutex vs spinlocks vs futex mutex: short CS, long CS, one core | `make run P=linux-internals/06-spinlocks-and-futex/c/07_lock_benchmark.c SAN=0 OPT=1` |
| [c/08_naive_spinlock_bug.c](c/08_naive_spinlock_bug.c) | **Bug:** plain-int check-then-set "lock"; TSan reports the data race | `make run P=linux-internals/06-spinlocks-and-futex/c/08_naive_spinlock_bug.c SAN=tsan` |
| [cpp/01_spinlock_lockable.cpp](cpp/01_spinlock_lockable.cpp) | Lockable `Spinlock` class with `lock_guard`, `unique_lock`, `scoped_lock`, `condition_variable_any` | `make run P=linux-internals/06-spinlocks-and-futex/cpp/01_spinlock_lockable.cpp` |
| [cpp/02_ticket_lock.cpp](cpp/02_ticket_lock.cpp) | Ticket lock with `std::atomic<uint16_t>`, FIFO order proven, 16-bit wrap | `make run P=linux-internals/06-spinlocks-and-futex/cpp/02_ticket_lock.cpp` |
| [cpp/03_futex_mutex.cpp](cpp/03_futex_mutex.cpp) | Adaptive futex mutex class, used in an MPMC work queue with `condition_variable_any` | `make run P=linux-internals/06-spinlocks-and-futex/cpp/03_futex_mutex.cpp` |

Every program (except the `_bug`) is also clean under ThreadSanitizer:

```bash
make run P=linux-internals/06-spinlocks-and-futex/c/04_mcs_lock.c SAN=tsan
make test-tsan        # all linux-internals programs
```

---

## 9. Exercises

**Easy**
1. In `c/02`, change the unlock to `memory_order_relaxed`. Does the test
   still pass on x86? Cross-compile it for the board
   (`aarch64-linux-gnu-gcc -O2 -pthread`) and run it there in a loop.
2. Add a `trylock` to the TAS and TTAS locks in `c/02`.
3. Run `c/07` with `time` and compare `user+sys` with `real` for each
   scenario. Which locks cost CPU time while waiting?

**Medium**
4. Give the TTAS lock in `c/02` a spin limit: after 1000 failed spins,
   sleep on a futex instead (you have built an adaptive lock; compare with
   `cpp/03`).
5. Add `fmutex_timedlock(m, ms)` to `c/06` using the timeout argument of
   `FUTEX_WAIT`. Be careful: the remaining time shrinks after each wake.
6. Make the futex mutex in `c/06` process-shared: put it in a
   `mmap(MAP_SHARED | MAP_ANONYMOUS)` region, `fork()`, drop
   `FUTEX_PRIVATE_FLAG`, and count in both processes (preview of lesson 11).
7. Build a counting semaphore on a futex (`sem_post` = fetch_add + wake,
   `sem_wait` = CAS down or futex_wait on 0). Compare with lesson
   [05](../05-semaphores/README.md).

**Hard**
8. Implement the **CLH lock** (each thread spins on its *predecessor's*
   node) and compare it with MCS.
9. Write a 2-state futex mutex that does *not* always call `futex_wake`
   and find, with a stress test, the interleaving that loses a wakeup.
   Explain it with a timeline. (Then read "Futexes Are Tricky" §5.)
10. Read `kernel/locking/qspinlock.c` and draw how the 32-bit word encodes
    locked byte, pending bit, tail CPU and tail index.

---

## 10. Remember forever

```
SPIN only if: tiny CS + multi-core + owner can't be preempted (kernel). User space: mutex.
lock = ACQUIRE RMW, unlock = RELEASE store.  volatile is NOT a lock.
TAS writes every spin (line ping-pong); TTAS spins on a LOAD (MESI Shared), then xchg.
backoff: double the wait; cpu_relax = PAUSE (x86) / YIELD (ARM); sched_yield after long.
ticket: my=fetch_add(next); while(owner!=my); unlock: owner++  -> FIFO, shared line.
MCS: queue of nodes, spin on OWN node, O(1) traffic -> kernel qspinlock.
futex: WAIT(addr,val) sleeps only if *addr==val (no lost wakeup); WAKE(addr,n). Loop!
3-state mutex: 0 free, 1 locked, 2 contended. unlock: if fetch_sub!=1 {=0; wake(1)}
glibc pthread_mutex / std::mutex / GMutex = that algorithm. ADAPTIVE_NP = spin, then sleep.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name the three conditions under which spinning beats sleeping. Which
   one can user space never guarantee, and why can the kernel?
2. Why is a spinlock in user space on a single-core machine always worse
   than a mutex?
3. Which memory orderings do lock and unlock need, and what goes wrong on
   the Cortex-A72 if unlock is relaxed?
4. Explain TTAS with the MESI states. Why does it reduce bus traffic?
5. What does the CPU relax hint do on x86 and on ARM?
6. How does a ticket lock guarantee FIFO? What is its scalability problem,
   and how does MCS fix it?
7. In MCS unlock, why must the unlocker sometimes wait for `me->next`
   even though its CAS on `tail` failed?
8. What does the kernel check atomically in `FUTEX_WAIT`, and which race
   does that prevent?
9. Walk through the 3-state futex mutex with two threads. Why does a woken
   waiter set the state to 2 instead of 1?
10. How many syscalls does an uncontended `pthread_mutex_lock` +
    `unlock` make? How do you know?
11. What is `PTHREAD_MUTEX_ADAPTIVE_NP` and when does it help?
12. Why does the kernel need `spin_lock_irqsave()` and not just
    `spin_lock()` when data is shared with an interrupt handler?
