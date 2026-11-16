# 03 — Kernel Synchronization

> Inside the kernel *everything* is concurrent: other CPUs, preemption,
> hard interrupts, softirqs, timers, workqueues and every process doing a
> system call into your driver. This lesson is the whole kernel locking
> toolbox — atomics, barriers, spinlocks (and their `_bh`/`_irqsave`/`raw_`
> flavours), mutexes, semaphores, rw-semaphores, completions, wait queues,
> seqlocks, RCU and per-CPU data — each one mapped to the user-space version
> you already know from the [linux-internals](../../linux-internals/) track,
> and each one proved by a self-testing kernel module.

---

## 1. Why this exists

In user space you choose to have threads. In a driver you do not get to
choose: the same `struct my_dev` is touched by

- two processes calling `read()` at the same time on two CPUs,
- the device's **interrupt handler**, which can fire *in the middle* of your
  `read()` on the same CPU,
- a **timer** or **tasklet** (softirq), a **workqueue** item, a **kthread**,
- and, on a preemptible kernel, the scheduler can switch tasks between any
  two instructions of your code.

On the BeagleBone AI-64 there are two Cortex-A72 cores, and the board's
kernel (`6.18.x-k3`) is built with **`CONFIG_PREEMPT_RT=y`**, the most
preemptible configuration Linux has. Locking bugs that never show on a
single-core, non-preemptible test box show up there.

The user-space rules from [02-race-conditions-and-atomics](../../linux-internals/02-race-conditions-and-atomics/)
and [03-mutexes-and-deadlock](../../linux-internals/03-mutexes-and-deadlock/)
still hold, plus **one new rule that drives every choice in this lesson**:

> **Some kernel code must never sleep.** Interrupt handlers, softirqs and
> any code holding a spinlock run in *atomic context*. There you may only
> use primitives that spin or are lock-free. Everything else may sleep.

---

## 2. Mental model

**One CPU is a desk with three in-trays of rising urgency.** You (process
context) work on a document. A colleague (softirq) may put something on top
of your document and you must deal with it *right now*; the boss (hardirq)
may interrupt both of you. Whoever is on top of the pile runs to completion
before the one below continues. If the boss needs the stapler you are
holding, and you cannot move until the boss leaves… nobody ever moves.
That is the deadlock that `spin_lock_irqsave()` exists to prevent.

```
 ONE CPU, time flows to the right; higher rows interrupt lower rows

 hardirq    |                 [ IRQ handler ]                 never sleeps
 softirq    |      [ timer cb ]               [ NET_RX ]      never sleeps
 process    |=====>          =====>        =====>   ====>     may sleep
 (task)       read() syscall, kthread, workqueue item          (unless it holds
                                                                a spinlock)
 TWO CPUs: everything above happens on both at the same time.

                       may sleep?   preemptible?   typical lock
 process context        yes          yes*          mutex, rw_semaphore
 holding a spinlock     NO           no            -
 softirq/tasklet/timer  NO           no            spin_lock (+ _bh in process)
 hardirq                NO           no            spin_lock (+ _irqsave elsewhere)
 RCU read section       NO**         see 3.6       rcu_read_lock()
   *  depends on CONFIG_PREEMPT*;  ** except SRCU
```

The sentence to remember: **pick the lock by *who else* touches the data —
if any of them may not sleep, nobody may sleep while holding it, and every
less urgent context must block the more urgent one while holding it.**

---

## 3. How it works

### 3.1 Sources of concurrency and the contexts you run in

| Source | What it means for your data | Disable it locally with |
|---|---|---|
| **SMP** | another CPU runs the same code *truly* in parallel | nothing — only a lock or atomic helps |
| **Preemption** (`CONFIG_PREEMPT`, `PREEMPT_DYNAMIC`, `PREEMPT_RT`) | another task can run on *this* CPU between two of your instructions | `preempt_disable()` / spinlock |
| **Hard interrupts** | a handler runs on top of you on this CPU | `local_irq_save()` / `spin_lock_irqsave()` |
| **Softirqs** (timers, tasklets, NET_RX/TX, block) | run on IRQ exit or in `ksoftirqd`, on top of process context | `local_bh_disable()` / `spin_lock_bh()` |
| **kthreads, workqueues** | ordinary process context, may run on any CPU | (they are tasks: use any lock) |
| **User context** | every syscall into your driver (`read`, `ioctl`, `mmap`…) from any number of processes | (tasks) |

**Process context** = running on behalf of a task (`current` is meaningful):
syscalls, kthreads, workqueues, `module_init`. You *may* sleep.
**Atomic context** = hardirq, softirq, NMI, or anything with preemption or
interrupts disabled (e.g. while holding a spinlock). You may **not** sleep:
no `mutex_lock`, `msleep`, `kmalloc(GFP_KERNEL)`, `copy_to_user`,
`wait_event`, `down`. If you sleep there, the scheduler switches away from
code that cannot be switched away from — "BUG: scheduling while atomic" or
a silent deadlock.

Helpers to ask "where am I?" (`<linux/preempt.h>`):

| Call | True when |
|---|---|
| `in_task()` | plain process context (not in any interrupt) |
| `in_hardirq()` | in a hard IRQ handler |
| `in_serving_softirq()` | running a softirq handler |
| `in_softirq()` | serving a softirq **or** BHs disabled (`spin_lock_bh` held) |
| `in_interrupt()` | any of hardirq/softirq/NMI **or** BH disabled (old, vague; avoid) |
| `in_atomic()` | preempt count non-zero — unreliable without `CONFIG_PREEMPT_COUNT` |
| `might_sleep()` | not a test: an *assertion* — with `CONFIG_DEBUG_ATOMIC_SLEEP` it prints a BUG when called in atomic context. Every sleeping primitive calls it. Put it in your own functions that may sleep. |

**Do not branch on these** to decide whether to take a mutex or a
spinlock ("`if (in_interrupt()) spin_lock…`"): it is fragile and Linus
has rejected it many times. Design the code so that each path knows its
context.

`ksync_spin` prints what the timer callback saw: `in_serving_softirq=1
in_task=0`, and the irq_work handler: `in_hardirq=1`.

**PREEMPT_RT in one paragraph** (the board's kernel): hard IRQ handlers
become threads (`irq/NN-name`), most softirqs run in threads, and
`spinlock_t`, `rwlock_t` and `local_lock_t` become **sleeping locks built on
`rt_mutex`** with priority inheritance. So on RT you *can* be preempted
while holding a `spinlock_t`, but you still must not call something that
sleeps "for real" (like `msleep`) under one by design. Only `raw_spinlock_t`
still disables preemption and spins — it is what the scheduler, the irqchip
code and handlers that run in real hard-IRQ context must use. The API you
write is the same; the semantics change, which is why correct `_bh`/`_irq`
usage and lockdep-clean code matter.

### 3.2 Atomics, bitops and barriers  (`kernel/ksync_spin.c`, tests 1–5)

`counter++` is load, add, store. Two CPUs can interleave those steps and one
update is lost — `ksync_spin` shows it with a plain `int` (it only prints
the number; on a 2-CPU board you typically lose a large fraction).

```
  CPU0: ldr 41 ........ add 42 .. str 42
  CPU1:       ldr 41 .. add 42 ......... str 42      -> 2 increments, +1 result
```

**`atomic_t` / `atomic64_t`** (`<linux/atomic.h>`) make the read-modify-write
indivisible:

| Family | Examples | Ordering |
|---|---|---|
| no return value | `atomic_inc`, `atomic_add`, `atomic_and`, `atomic_set` | **relaxed** — no barrier at all |
| return value | `atomic_inc_return`, `atomic_fetch_add`, `atomic_xchg`, `atomic_cmpxchg`, `atomic_dec_and_test` | **fully ordered** (as if `smp_mb()` before and after) |
| explicit | `atomic_inc_return_relaxed`, `_acquire`, `_release`, `atomic_try_cmpxchg` | as named |

Gotcha: `atomic_inc(&ready); … ` does **not** publish earlier writes. If
you need ordering with a void op, use `smp_mb__before_atomic()` /
`smp_mb__after_atomic()`.

**`refcount_t`** (`<linux/refcount.h>`) is an `atomic_t` for reference
counts that *saturates* instead of wrapping and WARNs on increment-from-zero
and underflow — turning use-after-free bugs into a warning. Pattern:
`refcount_inc` per user, `if (refcount_dec_and_test(&o->ref)) kfree(o);`
Test 3 runs N threads doing get/put on one object, checks the count is back
to 1, and the last put frees it. (`struct kref` wraps this with a release
callback.)

**Bit operations** (`<linux/bitops.h>`): `set_bit`, `clear_bit`,
`change_bit` (atomic, relaxed), `test_and_set_bit`, `test_and_clear_bit`
(atomic, fully ordered, return the old bit), `__set_bit` (non-atomic — only
when you own the word), `test_and_set_bit_lock`/`clear_bit_unlock`
(acquire/release: a one-bit lock). Test 2 has every thread try to claim all
4096 slots of a bitmap; `test_and_set_bit()` guarantees each slot is won
exactly once — that is how drivers hand out IDs, channels and minors.

**`READ_ONCE()` / `WRITE_ONCE()`**: force exactly one load/store of that
size, no tearing, no merging, no re-reading. They are *compiler*-level only
(volatile access), they add **no CPU barrier**. Use them for any variable
accessed concurrently without a lock (flags, statistics), and to tell KCSAN
"this race is intended".

**Barriers** order memory accesses between CPUs (see
[08-barriers-once-and-thread-pools](../../linux-internals/08-barriers-once-and-thread-pools/)):

| Kernel | Meaning | arm64 (A72) | x86-64 |
|---|---|---|---|
| `barrier()` | compiler only | nothing | nothing |
| `smp_mb()` | full barrier | `dmb ish` | `lock; addl $0,-4(%rsp)` |
| `smp_rmb()` | loads before loads | `dmb ishld` | compiler only (TSO) |
| `smp_wmb()` | stores before stores | `dmb ishst` | compiler only (TSO) |
| `smp_store_release(p, v)` | earlier accesses before this store | `stlr` | `mov` |
| `smp_load_acquire(p)` | this load before later accesses | `ldar` | `mov` |
| `atomic_inc(v)` | relaxed RMW | `ldxr/add/stxr` loop, or `stadd` (LSE) | `lock incl` |
| `atomic_inc_return(v)` | fully ordered RMW | `ldxr/stlxr`+`dmb ish`, or `ldaddal` | `lock xadd` |
| `mb()/rmb()/wmb()` | also orders device (MMIO/DMA) memory | `dsb sy/ld/st` | `mfence/lfence/sfence` |
| `dma_wmb()/dma_rmb()` | order CPU writes to coherent DMA memory | `dmb oshst/oshld` | compiler only |

Test 4 is the canonical message-passing pattern: the producer writes four
payload words, then `smp_store_release(&msg_seq, seq)`; the consumer
`smp_load_acquire(&msg_seq)` and then reads the payload. 10 000 messages,
zero stale words. Replace the release/acquire with `WRITE_ONCE`/`READ_ONCE`
and on ARM (not x86!) the consumer may see the new `seq` with old payload.

**What the A72 really executes.** Cortex-A72 is ARMv8.0: it has no LSE
atomics (`stadd`, `ldadd`, `cas` came with ARMv8.1). The cross-built
`ksync_spin.o` contains *both* — the kernel's "alternatives" patch the LSE
instructions in at boot only on CPUs that have them; on the TDA4VM the
out-of-line `ldxr`/`stxr` retry loops run:

```
$ aarch64-linux-gnu-objdump -d ksync_spin.o | grep -E 'stadd|ldxr|stxr|stlr|ldar|dmb'
   stadd w23, [x0]            <- LSE fast path (patched out on the A72)
   ldxr  x3, [x0]             <- LL/SC: load-exclusive
   stxr  w4, x3, [x0]         <- store-exclusive, fails if anyone wrote
   stlr / ldar                <- smp_store_release / smp_load_acquire
```

### 3.3 Spinlocks  (`kernel/ksync_spin.c`, tests 1, 6, 7)

A spinlock is a word that a waiter **busy-waits** on. While held, the
holder runs with **preemption disabled** (non-RT) and must not sleep; the
critical section must be short (microseconds). Why spin at all? Because
in hardirq/softirq context you cannot sleep, and for a few hundred
nanoseconds of work spinning is cheaper than two context switches.

```c
static DEFINE_SPINLOCK(lock);          /* or spin_lock_init(&dev->lock) */
spin_lock(&lock);      ... spin_unlock(&lock);
spin_lock_bh(&lock);   ... spin_unlock_bh(&lock);            /* + softirqs off */
spin_lock_irq(&lock);  ... spin_unlock_irq(&lock);           /* + IRQs off, ON after */
spin_lock_irqsave(&lock, flags); ... spin_unlock_irqrestore(&lock, flags);
spin_trylock(&lock)    /* 1 = got it */
```

**Which variant?** Decide by the *most urgent* context that also takes the
lock, and use the *matching* variant in the less urgent contexts:

| Data shared between… | In process context use | In the other context use |
|---|---|---|
| process ↔ process only (several CPUs) | `spin_lock` (or better: a mutex) | — |
| process ↔ softirq / tasklet / timer_list | `spin_lock_bh` | `spin_lock` |
| process ↔ hardirq handler | `spin_lock_irqsave` (`_irq` if IRQs are surely on) | `spin_lock` (IRQs already off in the handler)* |
| softirq ↔ same softirq type on other CPUs | — | `spin_lock` (a softirq never interrupts itself on one CPU) |
| softirq ↔ hardirq | — | softirq: `spin_lock_irqsave`; hardirq: `spin_lock` |
| hardirq ↔ hardirq (two IRQ lines) | — | `spin_lock_irqsave` in both is the safe default |
| any of the above on PREEMPT_RT, from *real* hard-IRQ context (`IRQF_NO_THREAD`, irq_work hard, hrtimer hard, irqchip) | `raw_spin_lock_irqsave` | `raw_spin_lock` |

\* On PREEMPT_RT a normal (threaded) IRQ handler runs in a thread, and a
`spinlock_t` with `spin_lock_irqsave()` does not even disable interrupts —
which is fine because the "handler" is now a task. That is why the same
driver source works on both kernels.

**Why `_irqsave`? The single-CPU self-deadlock:**

```
  CPU1, process context                      CPU1, IRQ handler
  ---------------------                      -----------------
  spin_lock(&dev->lock);   <- holds it
  dev->count++ ...
        ======== IRQ arrives on CPU1 =======>
                                             spin_lock(&dev->lock);
                                               spins... forever: the holder
                                               is the code underneath us and
                                               cannot run until we return
```

With `spin_lock_irqsave()`, local interrupts are off while the lock is held,
so the IRQ waits until `spin_unlock_irqrestore()`. Another CPU's IRQ handler
can still take the lock — it just spins briefly. `_irqsave` saves the old
IRQ state in `flags` and restores it, so it is correct whether IRQs were on
or off; `_irq` unconditionally re-enables, which is a bug if the caller had
them off. `flags` is a local `unsigned long`, never shared.

The same picture one level down is why **`_bh`** exists: a timer callback
runs in softirq context "on the way out" of an interrupt, on top of whatever
process context was running. Test 6 shares `{a, b}` between N kthreads
(`spin_lock_bh`) and a self-re-arming `timer_list` (`spin_lock`), checks
`b == 2a` on every access, and counts the timer's updates.
`spin_lock_irqsave` would also be correct there, but it blocks *hard*
interrupts too — more latency for nothing. Test 7 shares data with an
`irq_work` that runs in real hard-IRQ context, so it uses
`raw_spin_lock_irqsave` + `raw_spinlock_t` (correct on PREEMPT_RT too).

**Spinlock variants and history:**

- `raw_spinlock_t` — always a real spinning lock, also on RT. Use only for
  low-level code that must run in true atomic context; keep sections tiny.
- `rwlock_t` (`read_lock`, `write_lock_irqsave`…) — many readers or one
  writer, readers can starve the writer, and the shared reader count bounces
  between caches. Mostly replaced by RCU; avoid in new code.
- **Implementation history:** a plain test-and-set word (unfair: the CPU
  that just released often wins again) → **ticket spinlocks** (2.6.25, x86;
  "take a number" FIFO fairness, but all waiters spin on the same cache line)
  → **qspinlock** (x86 4.2, arm64 4.18 via `ARCH_USE_QUEUED_SPINLOCKS`): a
  4-byte word, the first waiter spins on it, further waiters queue in
  per-CPU **MCS nodes** and each spins on *its own* cache line. On arm64 the
  fast path is one `cmpxchg_acquire`, unlock is a `stlrb` (store-release).
- Debug: `CONFIG_DEBUG_SPINLOCK` catches use of uninitialised locks,
  unlock by the wrong CPU, and "spinlock recursion".

**Per-CPU data — the lock you do not need** (test 5). If each CPU has its
own copy, no other CPU touches it; you only need to stop *this* CPU from
switching tasks (or taking an interrupt that uses it) halfway through:

```c
DEFINE_PER_CPU(unsigned long, hits);
this_cpu_inc(hits);                    /* one instruction or irq-safe; no lock */
p = get_cpu_ptr(&pair); p->a++; p->b += 2; put_cpu_ptr(&pair);  /* preempt off */
local_irq_save(flags); ... local_irq_restore(flags);           /* + vs this CPU's IRQs */
local_bh_disable();    ... local_bh_enable();                   /* + vs softirqs */
for_each_possible_cpu(cpu) sum += per_cpu(hits, cpu);           /* slow reader */
```

`preempt_disable()` only protects against *this* CPU; it is not a lock.
On RT prefer `local_lock_t` (`local_lock(&pcpu->lock)`): it compiles to
`preempt_disable()` on normal kernels and to a per-CPU sleeping lock on RT,
and lockdep understands it. Real users: SNMP/network statistics, the slab
allocator's per-CPU caches, `percpu_counter`, `percpu_ref`.

### 3.4 Sleeping locks  (`kernel/ksync_sleep.c`, tests 1–3)

**`struct mutex`** (`<linux/mutex.h>`) — the default lock for process
context. One owner; only the owner unlocks; may sleep while held.

```c
static DEFINE_MUTEX(m);                 /* or mutex_init(&dev->lock) */
mutex_lock(&m);                         /* uninterruptible sleep (D state) */
if (mutex_lock_interruptible(&m)) return -ERESTARTSYS;  /* signals cancel */
if (mutex_lock_killable(&m))      return -EINTR;        /* only SIGKILL cancels */
if (mutex_trylock(&m)) { ... }          /* 1 = got it (opposite of down_trylock!) */
mutex_unlock(&m);
guard(mutex)(&m);                       /* 6.5+: scope-based unlock (cleanup.h) */
```

Inside: `owner` holds the owning `task_struct *` plus flag bits. Fast path
is one `cmpxchg_acquire` on `owner`. On contention the waiter first does
**optimistic spinning** (`CONFIG_MUTEX_SPIN_ON_OWNER`): if the owner is
*running* on another CPU it will probably unlock soon, so spin a little
(on an MCS queue, like qspinlock) instead of sleeping. If the owner is not
running, the waiter goes to sleep on `wait_list`. Rules (checked by
`CONFIG_DEBUG_MUTEXES`): no use in IRQ/softirq context, no recursive
locking, unlock by the owner only, no freeing a locked mutex. In a driver's
`read()`/`ioctl()` use `mutex_lock_interruptible()` so Ctrl-C works.
Test 1 builds a list with N kthreads under a mutex, using `kfree()` inside
the lock and `lockdep_assert_held()` + `__must_hold()` in the helper.

**`struct semaphore`** (`<linux/semaphore.h>`) — a counter plus a wait
list. `sema_init(&s, N)`, `down(&s)`, `down_interruptible()`,
`down_killable()`, `down_timeout()`, `down_trylock()` (**returns 0 on
success!**), `up(&s)` (may be called by any task, even from IRQ). It has no
owner, so no optimistic spinning, no priority inheritance, no lockdep owner
checks. Today it is mostly legacy (console_sem); use a mutex for exclusion
and a completion for signalling. Test 2 limits N threads to `sem_limit`
inside and records the maximum seen.

**`struct rw_semaphore`** (`<linux/rwsem.h>`) — sleeping reader/writer
lock: `down_read`, `up_read`, `down_write`, `up_write`,
`down_read_interruptible`, `down_write_killable`, `downgrade_write`. Writers
are not starved (new readers queue behind a waiting writer). Famous user:
`mm->mmap_lock` (page faults take it for read, `mmap()` for write). Test 3:
one writer that sleeps between updating `cfg_a` and `cfg_b`, readers that
sleep inside — readers never see a torn pair, several readers are inside
together, never alongside the writer.

**`struct rt_mutex`** and **priority inheritance** — if a low-priority task
holds a lock that a high-priority task needs, a medium-priority task can
keep the low one from running: **priority inversion** (the Mars Pathfinder
bug). An `rt_mutex` temporarily boosts the owner to the highest waiter's
priority. It backs user-space PI futexes (`PTHREAD_PRIO_INHERIT`,
`FUTEX_LOCK_PI`), the I2C core's `bus_lock`, and on PREEMPT_RT every
`spinlock_t`, `rwlock_t` and `mutex`.

### 3.5 Waiting: completions and wait queues  (`ksync_sleep.c` tests 4–5, `ksync_chardev.c`)

These are not locks; they are "sleep until a condition becomes true" — the
kernel's condition variables
([04-condition-variables](../../linux-internals/04-condition-variables/)).

**Completion** (`<linux/completion.h>`) — a one-shot (or counted) event:

```c
DECLARE_COMPLETION(done);          /* or init_completion(&dev->done) */
wait_for_completion(&done);                    /* D state, no timeout */
wait_for_completion_timeout(&done, HZ);        /* 0 = timed out */
wait_for_completion_interruptible(&done);      /* -ERESTARTSYS on signal */
complete(&done);       /* wake one waiter (counts if nobody waits yet) */
complete_all(&done);   /* wake everyone, stays "done" until reinit_completion() */
```

Every test in this lesson uses them: the thread runner opens a "start
gate" with `complete_all()` and waits for N `complete()` calls on "done".
Test 4 is the classic driver pattern — start an operation, then
`wait_for_completion_timeout()` until the IRQ handler / worker calls
`complete()`. It also shows `kthread_complete_and_exit()`: a thread that
signals and exits in one call, so it never returns into module code that
`rmmod` may already have freed.

**Wait queue** (`<linux/wait.h>`) — a list of sleeping tasks plus a lock.
You give the condition; the macros do the classic safe loop:

```c
wait_queue_head_t wq;  init_waitqueue_head(&wq);
/* sleeper */                                   /* waker */
ret = wait_event_interruptible(wq, cond);       make cond true (under the lock);
                                                wake_up_interruptible(&wq);
  expands to roughly:
  for (;;) {
      prepare_to_wait(&wq, &w, TASK_INTERRUPTIBLE);  /* on list + state first */
      if (cond) break;                               /* then test */
      if (signal_pending(current)) { ret = -ERESTARTSYS; break; }
      schedule();
  }
  finish_wait(&wq, &w);
```

Setting the task state *before* testing the condition is what prevents the
lost wake-up (the waker's `wake_up` either sees us on the list or we see
the condition true). Variants: `wait_event`, `_timeout` (returns 0 on
timeout), `_killable`, `_interruptible_timeout`, `_lock_irq` (condition
checked with a spinlock held), and `_exclusive` versions that wake only one
waiter (avoid the thundering herd). Test 5 is a bounded buffer with two wait
queues, `not_full` and `not_empty`, between producer and consumer kthreads,
with poison pills to stop the consumers — exactly the pthread
`mutex + 2 condvars` design, in kernel form.

**How a driver's blocking `read()` and `poll()` work** (`ksync_chardev.c`):

```
  read(fd)                                       IRQ / timer / write()
    lock; n = kfifo_out(); unlock                  lock; kfifo_in(); unlock
    if n == 0:                                     wake_up_interruptible(&wq)
       O_NONBLOCK? return -EAGAIN                        |
       wait_event_interruptible(wq, len > 0) <-----------+
       (signal -> -ERESTARTSYS -> EINTR or restart)
    copy_to_user() AFTER unlocking (it may sleep)

  poll(fd) / select / epoll_wait
    driver .poll = poll_wait(file, &wq, pt);   <- just registers on the SAME wq
                   return EPOLLIN if data, EPOLLOUT if room   (never sleeps)
    the VFS sleeps, and calls .poll again after a wake_up on wq
```

The FIFO is shared with a timer (softirq), so the lock is a spinlock taken
with `spin_lock_bh()` in `read/write/poll`. In a real UART-style driver the
producer is the hard IRQ handler and the process side uses
`spin_lock_irqsave()`. The user-space side of this story (`poll`, `epoll`,
`eventfd`) is [13-signals-eventfd-and-epoll](../../linux-internals/13-signals-eventfd-and-epoll/);
`user/ksync_poll.c` checks EAGAIN, a 100 ms poll timeout, POLLIN after a
write, a `read()` that really sleeps ~200 ms until another thread writes,
a full FIFO (write EAGAIN, no POLLOUT), and `epoll`.

### 3.6 Read-mostly data: seqlock and RCU  (`kernel/ksync_readmostly.c`)

**Seqlock / seqcount** — readers take no lock; they detect that they raced
with a writer and retry
([07-rwlocks-seqlocks-and-rcu](../../linux-internals/07-rwlocks-seqlocks-and-rcu/)):

```
 seq  0         1 (odd)                 2
 W    |  write_seqlock: seq++  update  write_sequnlock: seq++
 R1     s=read_seqbegin()=0 ... copy ... read_seqretry(s)? 0==2? no -> retry
 R2                                       s=2 ... copy ... retry? no -> use copy
```

```c
static DEFINE_SEQLOCK(sl);
do {                                          write_seqlock(&sl);   /* _irqsave/_bh too */
        seq = read_seqbegin(&sl);             x = …; y = …;
        a = x; b = y;            /* copy */   write_sequnlock(&sl);
} while (read_seqretry(&sl, seq));
```

`seqlock_t` = `seqcount_t` + a spinlock serialising writers. A bare
`seqcount_t` needs the writers serialised by something else;
`seqcount_spinlock_t`/`seqcount_mutex_t`/`seqcount_raw_spinlock_t` record
*which* lock so lockdep can check it. Readers never block writers (writers
can starve readers). Readers must only **copy** — never dereference a
pointer read inside the loop (it may point to freed memory) — and a
reader must never run on the CPU that interrupted a writer of the same
seqlock (it would spin forever), hence `write_seqlock_irqsave()` when an IRQ
handler reads. Real users: `jiffies_64` on 32-bit CPUs (`jiffies_seq` in
`kernel/time/jiffies.c`), timekeeping (`tk_core.seq`, which is how
`ktime_get()` and the vDSO `clock_gettime()` read time without a lock),
`rename_lock` in the dcache. The test: one writer updates a fake clock
`{sec, nsec, check}` iters times; readers verify `check == sec*1000+nsec`
on every copy and count retries.

**RCU (Read-Copy-Update)** — readers take no lock, write no shared memory
and never retry; writers **copy**, **update** the copy, **publish** it with
one pointer store, and free the old version only after a **grace period**:
when every CPU has passed through a point where it cannot be inside a read
section that started before the publish.

```
 cur --> [v1]
 writer: new = kmalloc(); *new = *v1 + change;     (readers still see v1)
         rcu_assign_pointer(cur, new)              cur --> [v2]   (release store)
         synchronize_rcu()  ......................  waits until every reader that
         kfree(v1)                                  might hold v1 has left
 reader: rcu_read_lock(); p = rcu_dereference(cur); use *p; rcu_read_unlock();
          |<--------- may not sleep, may see v1 OR v2, never freed memory ----->|
```

| API | Who | What |
|---|---|---|
| `rcu_read_lock()` / `rcu_read_unlock()` | reader | mark the read section (nearly free: preempt count or a per-task counter) |
| `rcu_dereference(p)` | reader | load the pointer with dependency ordering + sparse/lockdep checks |
| `rcu_assign_pointer(p, v)` | writer | publish: release store, so the fields are visible before the pointer |
| `rcu_replace_pointer(p, v, cond)` | writer | publish and return the old pointer |
| `rcu_dereference_protected(p, lockdep_is_held(&m))` | writer | read the pointer under the update lock (no barrier needed) |
| `synchronize_rcu()` | writer | sleep until a grace period has passed (ms) |
| `call_rcu(&p->rcu, cb)` | writer, any context | run `cb` after a grace period (softirq or `rcuc` thread) |
| `kfree_rcu(p, rcu)` | writer, any context | free after a grace period, no callback to write |
| `rcu_barrier()` | module exit | wait until all queued `call_rcu` callbacks have run — **required before rmmod if you used call_rcu** |
| `list_add_rcu`, `list_del_rcu`, `list_for_each_entry_rcu`, `hlist_*_rcu` | both | RCU-safe linked lists (writers still need a lock among themselves) |
| `srcu_read_lock()`… | reader that must sleep | Sleepable RCU, per-domain |

Writers still exclude each other with a normal lock (here a mutex); RCU only
removes readers from the picture. The test rotates the three ways to free
the old copy (`synchronize_rcu`+`kfree`, `call_rcu` with a callback that
poisons then frees, `kfree_rcu`), poisons every freed copy, and has readers
check magic, checksum and that versions never go backwards. Teardown:
unpublish, `synchronize_rcu()`, `kfree`, `rcu_barrier()`.

Real users: the network stack (`dev->ifalias`, routing tables, `txq->qdisc`
in `net/core/dev.c`, packet RX under `rcu_read_lock`), the dcache path
walk, module list, PID lookup, `struct file` tables, netfilter rules. With
`CONFIG_PREEMPT_RCU` (always on PREEMPT_RT) readers can be preempted, but
still must not block.

### 3.7 Debugging tools: lockdep, KCSAN, sparse  (`kernel/ksync_bug.c`)

| Tool | Kconfig | Catches | Message to grep in dmesg |
|---|---|---|---|
| **lockdep** | `CONFIG_PROVE_LOCKING` (selects `DEBUG_LOCK_ALLOC`, `DEBUG_SPINLOCK`, `DEBUG_MUTEXES`…) | lock-order inversions (ABBA, even if they never deadlocked), IRQ-safe vs IRQ-unsafe use, recursive locking, held locks at exit | `possible circular locking dependency detected`, `inconsistent lock state`, `possible recursive locking detected` |
| atomic sleep check | `CONFIG_DEBUG_ATOMIC_SLEEP` | `might_sleep()` called in atomic context | `BUG: sleeping function called from invalid context` |
| spinlock debug | `CONFIG_DEBUG_SPINLOCK` | uninitialised lock, wrong-CPU unlock, recursion, lockup | `BUG: spinlock …` |
| RCU checks | `CONFIG_PROVE_RCU` (with lockdep) | `rcu_dereference` outside a read section / without the lock | `suspicious RCU usage` |
| lock statistics | `CONFIG_LOCK_STAT` | contention per lock class | `/proc/lock_stat` |
| **KCSAN** | `CONFIG_KCSAN` (x86-64, arm64; gcc ≥ 11 or clang ≥ 11) | data races: concurrent plain accesses where one is a write | `BUG: KCSAN: data-race in …` |
| hang detectors | `CONFIG_SOFTLOCKUP_DETECTOR`, `CONFIG_DETECT_HUNG_TASK` | a CPU spinning forever, a task in D state > 120 s | `soft lockup`, `blocked for more than` |
| **sparse** | none: `make C=1` (or `C=2`) | `__acquires`/`__releases` imbalance, `__rcu` and `__user` pointer misuse | `context imbalance`, `different address spaces` |

How lockdep thinks: every lock belongs to a **class** (all locks initialised
at the same place / with the same key). It records each "held A while taking
B" edge and each context a class is used in (hardirq, softirq, with IRQs
on/off), then reports any *possible* cycle or unsafe combination the moment
both halves have been seen — even if the timing never lined up. After the
first report it switches itself off ("turning off the locking correctness
validator"), so fix the first splat first. A splat shows the two (or more)
lock chains with stack traces and a "Possible unsafe locking scenario" table
drawn as CPU0/CPU1 columns.

`ksync_bug.ko` (built only with `BUGS=1`) triggers one bug per load:

| `bug=` | Bug | Needs | Expected splat |
|---|---|---|---|
| 1 | `mutex_lock()` inside `raw_spin_lock()` | `CONFIG_DEBUG_ATOMIC_SLEEP` | `BUG: sleeping function called from invalid context at kernel/locking/mutex.c` … `in_atomic(): 1` |
| 2 | ABBA: A→B, then B→A (sequentially — no real hang) | `CONFIG_PROVE_LOCKING` | `WARNING: possible circular locking dependency detected` |
| 3 | `spin_lock()` in process context + same lock in a timer | `CONFIG_PROVE_LOCKING` (non-RT kernel) | `WARNING: inconsistent lock state` `{SOFTIRQ-ON-W} -> {IN-SOFTIRQ-W} usage` |

Without the options nothing is printed. That is the point: the bug ships.

**Annotations** that document and check locking rules:

```c
struct my_dev {
        struct mutex lock;      /* protects: list, count. Order: lock -> port->lock */
        struct list_head list;
        int count;
};
static void add_locked(struct my_dev *d) __must_hold(&d->lock)   /* sparse */
{
        lockdep_assert_held(&d->lock);                            /* runtime */
        ...
}
static void my_lock(struct my_dev *d) __acquires(&d->lock);
static void my_unlock(struct my_dev *d) __releases(&d->lock);
struct cfg __rcu *cur;          /* sparse: only via rcu_dereference/assign */
```

Write the **lock order** in a comment next to the locks (the kernel does
this at the top of `mm/rmap.c` and `mm/filemap.c`) and take locks in that
order everywhere. Use `mutex_lock_nested()` / `spin_lock_nested()` with a
subclass when you must hold two locks of the same class in a fixed order
(e.g. parent then child).

### 3.8 Which primitive do I use?

| Situation | Use | Not |
|---|---|---|
| a counter / flag updated from many CPUs | `atomic_t`, `atomic64_t`, `set_bit` | a lock around `++` |
| object lifetime shared by several users | `refcount_t` / `kref` | bare `atomic_t` |
| per-CPU statistics | `DEFINE_PER_CPU` + `this_cpu_inc` (or `u64_stats_sync`) | an atomic shared by all CPUs |
| publish data to another CPU, one writer | `smp_store_release` / `smp_load_acquire` | `volatile` |
| process context only, may sleep inside | `mutex` (`_interruptible` in syscalls) | spinlock, semaphore |
| process context, many readers, rare writer, may sleep | `rw_semaphore` | `rwlock_t` |
| shared with a softirq / tasklet / timer_list | `spinlock_t`: `spin_lock_bh` in process, `spin_lock` in softirq | mutex |
| shared with a hard IRQ handler | `spinlock_t`: `spin_lock_irqsave` outside, `spin_lock` in the handler | `_bh` only |
| shared with true hard-IRQ code on PREEMPT_RT | `raw_spinlock_t` | `spinlock_t` |
| allow at most N users of a resource | `semaphore` (or a counter + wait queue) | mutex |
| wait for one event ("done", "fw loaded", "DMA finished") | `completion` | a flag + `msleep` loop |
| wait for an arbitrary condition, blocking `read()`, `poll()` | wait queue: `wait_event_interruptible` + `wake_up` | busy-polling |
| small data, very frequent reads, readers can retry | `seqlock_t` / `seqcount_*_t` | rwlock |
| pointer to read-mostly data / list, readers must be fast | RCU | rwlock, rw_semaphore |
| priority inversion matters (RT tasks share a lock) | `rt_mutex` | semaphore |
| data owned by a single CPU / single thread | nothing — keep it that way | any lock |

---

## 4. Complexity table

Rough costs on an uncontended path; contended costs depend on cache-line
transfers between cores (~50–100 ns on the A72 cluster, more across a NUMA
fabric).

| Primitive | Uncontended | Contended waiter | Holder may sleep | Usable in IRQ/softirq | Why |
|---|---|---|---|---|---|
| `READ_ONCE`/`WRITE_ONCE` | 1 load/store | — | — | yes | compiler-only |
| `atomic_inc` (relaxed) | 1 RMW (~10–20 ns LL/SC) | retries the RMW | — | yes | exclusive monitor / cache line ownership |
| `atomic_inc_return` | RMW + barrier | retries | — | yes | full ordering costs a `dmb` |
| `this_cpu_inc` | ~1 instruction | none | — | yes | no sharing at all |
| `spin_lock` | 1 acquire-cmpxchg + 1 release store | spins (CPU burned) | **no** | yes | must stay atomic |
| `spin_lock_bh` / `_irqsave` | + mask/unmask softirqs / IRQs | spins | no | yes | adds latency for others |
| `mutex_lock` | 1 cmpxchg | spins briefly if owner runs, then sleeps (2 context switches) | yes | **no** | sleeps |
| `down` (semaphore) | raw spinlock + counter | sleeps | yes | `up()` only | no owner, no spinning |
| `down_read` (rwsem) | 1 atomic add | sleeps | yes | no | reader count is shared (cache bouncing) |
| `wait_for_completion` | spinlock + check | sleeps | — | `complete()` only | |
| `read_seqbegin/retry` | 2 loads + `smp_rmb` | retries while a writer is active | readers: no | yes (with `_irqsave` writers) | no writes by readers |
| `rcu_read_lock` | ~0 (preempt count / task counter) | never waits | no | yes | writer pays: `synchronize_rcu` = ms |
| `synchronize_rcu` | — | sleeps one grace period (ms) | yes | **no** | waits for all CPUs |

Space: `spinlock_t` 4 bytes (non-debug), `struct mutex` ~32 bytes,
`struct semaphore`/`rw_semaphore`/`completion`/`wait_queue_head_t` ~24–40
bytes, `rcu_head` 16 bytes per object. Lockdep adds tens of bytes per lock.

---

## 5. C vs C++ — and kernel vs user space

The kernel is C (plus Rust bindings), so all module code here is C. The
mapping to what you write in user space — the
[linux-internals](../../linux-internals/) track — is direct:

| User space (C) | C++ | Kernel | Notes |
|---|---|---|---|
| `pthread_mutex_t` | `std::mutex`, `std::lock_guard` | `struct mutex`, `guard(mutex)` | both sleep; both built on a futex-like fast path ([03](../../linux-internals/03-mutexes-and-deadlock/)) |
| `sem_t` | `std::counting_semaphore` (C++20) | `struct semaphore` | kernel `down_trylock` returns 0 on success ([05](../../linux-internals/05-semaphores/)) |
| `pthread_spinlock_t` | (`std::atomic_flag` loop) | `spinlock_t`, `raw_spinlock_t` | **`_bh`/`_irq`/`_irqsave` have no user-space analogue** — threads have no interrupts (the closest is blocking signals with `pthread_sigmask` around signal-shared data) ([06](../../linux-internals/06-spinlocks-and-futex/)) |
| `pthread_rwlock_t` | `std::shared_mutex` | `rw_semaphore` (sleeping), `rwlock_t` (spinning) | ([07](../../linux-internals/07-rwlocks-seqlocks-and-rcu/)) |
| `pthread_cond_t` + mutex | `std::condition_variable` | wait queue (`wait_event`/`wake_up`) | kernel macros embed the `while (!cond) wait` loop ([04](../../linux-internals/04-condition-variables/)) |
| `pthread_barrier_t`, a flag + condvar | `std::latch`, `std::promise/future` | `completion` | |
| `futex(2)` | (inside `std::mutex`) | the *kernel side* that makes user-space locks sleep; PI futexes use `rt_mutex` | ([06](../../linux-internals/06-spinlocks-and-futex/)) |
| `PTHREAD_PRIO_INHERIT` mutex | — | `rt_mutex` | priority inheritance |
| liburcu (`urcu`) | (C++26 `std::rcu`) | RCU | same API names: `rcu_read_lock`, `synchronize_rcu`, `call_rcu` |
| hand-written seqlock with atomics | — | `seqlock_t`, `seqcount_t` | |
| C11 `atomic_int`, `atomic_fetch_add` | `std::atomic<int>` | `atomic_t`, `atomic_fetch_add` | kernel default for void ops is *relaxed*, C11 default is *seq_cst* ([02](../../linux-internals/02-race-conditions-and-atomics/)) |
| `memory_order_release/acquire` | same | `smp_store_release` / `smp_load_acquire` | ([08](../../linux-internals/08-barriers-once-and-thread-pools/)) |
| `atomic_thread_fence(seq_cst)` | same | `smp_mb()` | |
| `volatile sig_atomic_t` / relaxed atomic load | `std::atomic::load(relaxed)` | `READ_ONCE` / `WRITE_ONCE` | |
| `pthread_once` | `std::call_once` | `DO_ONCE()`, `static_key`, or init in `module_init` | |
| `__thread` / `thread_local` | `thread_local` | `DEFINE_PER_CPU` (per *CPU*, not per thread) | |
| `eventfd` + `poll` | — | wait queue + `.poll` = `poll_wait()` | ([13](../../linux-internals/13-signals-eventfd-and-epoll/)) |
| ThreadSanitizer | same | KCSAN (data races), lockdep (lock order) | |

The kernel's C has two things user-space C lacks: the **`cleanup.h`
guards** (`guard(spinlock_irqsave)(&lock);`, `scoped_guard(mutex, &m) {…}`)
which give C++-style RAII unlocking (6.5+), and the **Linux-Kernel Memory
Model** (`tools/memory-model/`), which defines exactly what the barriers
above guarantee and can be checked with `herd7` litmus tests.

---

## 6. Where it is used in the real world

On the BeagleBone AI-64's own drivers (paths are in the board kernel tree):

- **UART** — `drivers/tty/serial/8250/8250_omap.c` (the TDA4VM UARTs are
  `"ti,j721e-uart", "ti,am654-uart"`): the IRQ handler and the TTY write
  path share `port->lock`; process context takes it with
  `uart_port_lock_irqsave()` (a wrapper around `spin_lock_irqsave`). The
  console write path uses it too, which is why `printk` from an IRQ handler
  does not deadlock.
- **I2C** — every `struct i2c_adapter` has `struct rt_mutex bus_lock`
  (`include/linux/i2c.h`): transfers sleep, and a real-time audio/sensor
  thread must not be blocked forever by a low-priority one holding the bus
  → priority inheritance. `i2c-omap.c` drives the TDA4VM controllers.
- **GPIO** — `drivers/gpio/gpio-davinci.c` (TDA4VM GPIO) protects its
  registers with a spinlock taken with `spin_lock_irqsave` because the
  irqchip callbacks run in interrupt context.
- **Networking** — the netdev and routing structures are RCU-protected:
  `rcu_dereference(dev->ifalias)`, `rcu_dereference(txq->qdisc)` in
  `net/core/dev.c`; per-CPU stats with `u64_stats_sync` (a seqcount);
  NAPI polling in NET_RX softirq with `spin_lock_bh` in process context.
- **V4L2 / camera** (the AI-64's CSI capture): `struct vb2_queue` has
  `struct mutex *lock` that serialises the `VIDIOC_*` ioctls, and the
  buffer-done path from the DMA IRQ uses a driver spinlock with irqsave plus
  a wait queue so `poll()`/`DQBUF` wake up.
- **Timekeeping** — `jiffies_seq` (`seqcount_raw_spinlock_t`) and
  `tk_core.seq`; `clock_gettime()` in the vDSO uses the same seqcount idea
  in *user space*.
- **Memory management** — `mm->mmap_lock` (rw_semaphore), page-table
  spinlocks, per-CPU page lists with `local_lock`.
- **Block / filesystems** — `inode->i_rwsem`, `sb->s_umount`,
  RCU path walk in the dcache, `rename_lock` seqlock.
- **Driver core** — `device_lock()` (a mutex) around probe/remove,
  `kref` for object lifetime, completions for firmware loading.

---

## 7. Common mistakes and pitfalls

1. **Sleeping while holding a spinlock** — `mutex_lock`, `kmalloc(GFP_KERNEL)`,
   `copy_to_user`, `msleep` under `spin_lock`. Use `GFP_ATOMIC`, copy to a
   bounce buffer and do user copies after unlocking (as `ksync_chardev` does).
2. **Plain `spin_lock` on data that an IRQ/softirq also locks** → single-CPU
   self-deadlock. Use `_irqsave` / `_bh` in the less urgent context.
3. **`spin_lock_irq` / `spin_unlock_irq` in code that may be called with
   IRQs already off** — the unlock turns them on behind the caller's back.
   Use `_irqsave`.
4. **Using a mutex or `wait_event` in a timer, tasklet or IRQ handler.**
   Defer the sleeping part to a workqueue or threaded IRQ.
5. **ABBA lock ordering** — document the order, run with lockdep.
6. **Assuming `atomic_inc()` orders memory.** Void atomics are relaxed.
7. **`volatile` instead of `READ_ONCE`/barriers** — it neither makes RMW
   atomic nor orders memory across CPUs.
8. **Testing only on x86.** TSO hides missing `smp_wmb`/release; the
   A72 does not. Test on the board.
9. **`down_trylock()` success is 0** while `mutex_trylock()`/`spin_trylock()`
   success is 1.
10. **Lost wake-up**: checking a flag, then calling `schedule()`, instead of
    setting the task state first. Use `wait_event*` and completions.
11. **`if` instead of `while` around a wait** — another reader may take the
    data first (`ksync_read` loops).
12. **Dereferencing a pointer read inside a seqlock retry loop**, or
    **sleeping inside `rcu_read_lock()`**, or **using `rcu_dereference` result
    after `rcu_read_unlock()`**.
13. **Forgetting `rcu_barrier()` in module exit** after `call_rcu()` — the
    callback runs after the module's code is gone.
14. **kthread lifetime**: calling `kthread_stop()` on a thread that already
    returned (use-after-free), or a thread that never checks
    `kthread_should_stop()` (rmmod hangs). Park until stop, or use
    `kthread_complete_and_exit()` for fire-and-forget threads.
15. **Self-re-arming timer + `timer_delete_sync()`** can race; use
    `timer_shutdown_sync()` on teardown.
16. **`raw_spinlock_t` everywhere "to be safe"** — on RT it destroys latency.
    Use it only for code that really runs in hard-IRQ context.
17. **Per-CPU access with preemption enabled** — `p = this_cpu_ptr(); p->a++;
    p->b++;` can migrate between the two lines. Use `get_cpu_ptr()` or
    `local_lock`.
18. **`copy_to_user()` under a mutex that your `.mmap` or `.fault` handler
    also takes** — the copy can page-fault, the fault path takes
    `mmap_lock` and then your mutex, `mmap()` takes them the other way round:
    lockdep reports a circular dependency. Copy outside such locks.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile for all modules; `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `BUGS=1`, `W=1`, `clean` | `make drivers` |
| [kernel/ksync_spin.c](kernel/ksync_spin.c) | plain int (lost updates) vs `atomic_t`/`atomic64_t` vs spinlock with timings; `test_and_set_bit` slot claiming; `refcount_t`; release/acquire message passing; per-CPU + `get_cpu_ptr`; `spin_lock_bh` vs a timer; `raw_spin_lock_irqsave` vs a hard-IRQ `irq_work` | `insmod ksync_spin.ko threads=4 iters=100000` |
| [kernel/ksync_sleep.c](kernel/ksync_sleep.c) | mutex-protected list (`lockdep_assert_held`, `__must_hold`); semaphore limiting concurrency (max inside, `down_trylock` convention); `rw_semaphore` torn-read check; completion with timeout and `kthread_complete_and_exit`; wait-queue bounded buffer with `wait_event_timeout` | `insmod ksync_sleep.ko threads=4 sem_limit=2` |
| [kernel/ksync_readmostly.c](kernel/ksync_readmostly.c) | `seqlock_t` writer + lock-free readers with retry counting; RCU pointer swap with `rcu_replace_pointer`, `synchronize_rcu`, `call_rcu`, `kfree_rcu`, poisoning, `rcu_barrier` | `insmod ksync_readmostly.ko threads=4` |
| [kernel/ksync_chardev.c](kernel/ksync_chardev.c) | misc device `/dev/ksync`: kfifo + spinlock (`_bh`), blocking `read()`/`write()` with a wait queue, `O_NONBLOCK`, `.poll`, optional timer producer (`tick_ms=`) | `insmod ksync_chardev.ko [tick_ms=200]` |
| [kernel/ksync_bug.c](kernel/ksync_bug.c) | **intentionally broken** (only with `BUGS=1`): mutex under spinlock, ABBA order, softirq-unsafe lock | `make -C …/kernel BUGS=1`, `insmod ksync_bug.ko bug=1` |
| [user/ksync_poll.c](user/ksync_poll.c) | user-space test for `/dev/ksync`: EAGAIN, poll timeout/POLLIN/POLLOUT, a `read()` that sleeps until another thread writes, full FIFO, epoll, timer data | `make drivers`, then `sudo ./build/linux-device-drivers/03-kernel-synchronization/user/ksync_poll` |

**Build** (from the repo root):

```sh
make drivers                                   # host: modules + user/ programs
make -C linux-device-drivers/03-kernel-synchronization/kernel W=1          # extra warnings
make -C linux-device-drivers/03-kernel-synchronization/kernel BUGS=1       # + ksync_bug.ko
make drivers KDIR=<board kernel tree> ARCH=arm64 CROSS_COMPILE=<its toolchain prefix>   # board
                                               # (see ../README.md#building for the exact paths)
make drivers-clean                             # remove *.ko, *.o, .*.cmd …
```

All modules are **build-tested only** on the host (Ubuntu 6.8, zero warnings
with and without `W=1`, checkpatch-clean apart from the file-name-in-header
note that this repo's conventions require) and cross-built against the
board's `6.18.x-arm64-k3` PREEMPT_RT tree with its own GCC 15 toolchain.
Loading them is **your** step, on the board or in a VM — see
[../README.md](../README.md#safety).

**Run** (board or VM):

```sh
sudo insmod ksync_spin.ko threads=4 iters=100000
sudo dmesg | grep ksync_spin
sudo rmmod ksync_spin
```

Expected output (numbers vary from run to run and machine to machine):

```
ksync_spin: start: 4 threads x 100000 iterations, 2 online CPUs
ksync_spin: INFO plain int       231877 of 400000, lost 168123 updates (expected on SMP, not a failure) 4 ms
ksync_spin: PASS atomic_t       400000 of 400000, 11 ms
ksync_spin: PASS atomic64_t     800000 of 800000
ksync_spin: PASS spinlock       400000 of 400000, 25 ms
ksync_spin: PASS bitops         4096 slots claimed exactly once each by 4 threads
ksync_spin: PASS refcount_t     4 threads x 100000 get/put, count back to 1, last put freed the object
ksync_spin: PASS release/acq    10000 messages, 0 stale payload words
ksync_spin: PASS per-CPU        sum over 2 CPUs used = 400000, no lock taken
ksync_spin: PASS spin_lock_bh   a=400052 (52 by timer, in_serving_softirq=1 in_task=0), b==2a
ksync_spin: PASS irqsave        a=406250 (6250 from the hard-IRQ handler, in_hardirq=1), b==2a held
ksync_spin: done: all checks passed

ksync_sleep: start: 4 threads x 100000 iterations
ksync_sleep: PASS mutex          4 threads added 2000 items each, removed odd ones: 4000 left, list intact
ksync_sleep: PASS semaphore      limit 2, max seen inside 2, down_trylock() on empty = 1 (1 = busy)
ksync_sleep: PASS rw_semaphore   1 writer + 3 readers, max 3 readers inside together, 0 torn reads
ksync_sleep: PASS completion     woken by complete() after ~20 ms (timeout was 2000 ms)
ksync_sleep: PASS waitqueue      2 producers -> 2 consumers through 8 slots, sum 800020000 == 800020000
ksync_sleep: done: all checks passed

ksync_readmostly: start: 1 writer + 3 readers
ksync_readmostly: PASS seqlock        100000 writes, 2190311 lock-free reads, 1873 retries, 0 torn copies
ksync_readmostly: PASS RCU            48211 lock-free reads, 200 swaps (synchronize_rcu 67, call_rcu 66, kfree_rcu 67), 0 stale/poisoned
ksync_readmostly: done: all checks passed
```

Any `FAIL` line, a `done: N check(s) FAILED`, or any kernel `BUG:`/`WARNING:`
splat means something is wrong. The modules always return 0 from init so
you can `rmmod` them even after a FAIL.

```sh
sudo insmod ksync_chardev.ko
sudo ./ksync_poll        # PASS nonblock / poll / blocking / full / epoll
sudo rmmod ksync_chardev
sudo insmod ksync_chardev.ko tick_ms=200 && sudo ./ksync_poll   # PASS ticks
sudo cat /dev/ksync      # watch the timer lines arrive; Ctrl-C = -ERESTARTSYS
```

---

## 9. Exercises

**Easy**
1. Load `ksync_spin` with `threads=2` and `threads=16`. How does the number
   of lost plain-int updates change? Why does it drop to (almost) zero when
   you boot with `maxcpus=1`?
2. Time the three counters on the board and on your PC. Why is the spinlock
   version slower than `atomic_inc`, and why is the gap bigger on the A72
   (hint: LL/SC vs `lock xadd`)?
3. In `ksync_sleep`, set `sem_limit=1`. What primitive have you built? What
   does it lack compared with a mutex?

**Medium**
4. Change `bh_work()` in `ksync_spin.c` to use plain `spin_lock()`. Boot a
   kernel with `CONFIG_PROVE_LOCKING=y` (or use `ksync_bug.ko bug=3`), load
   it and read the "inconsistent lock state" splat line by line.
5. Replace the release/acquire pair in `msg_work()` with
   `WRITE_ONCE`/`READ_ONCE`. Run it many times on the board with
   `threads=2`. Did it fail? Why might it pass anyway (hint: `ldxr`/`stxr`,
   cache timing), and why is it still wrong per the LKMM? Write the
   message-passing litmus test for `herd7`.
6. Add an `ioctl` to `ksync_chardev` that returns the number of bytes queued.
   Which lock does it need, and which variant?
7. Convert `item_lock` in `ksync_sleep.c` to use `guard(mutex)` /
   `scoped_guard` from `<linux/cleanup.h>` (kernel ≥ 6.5).

**Hard**
8. Turn the RCU pointer into an RCU *list*: writers `list_add_rcu`/
   `list_del_rcu` under the mutex and free with `kfree_rcu`, readers walk it
   with `list_for_each_entry_rcu` and check every node.
9. Make `ksync_chardev` use a real interrupt on the board: a GPIO button
   (`gpiod_to_irq` + `request_threaded_irq`) as the producer. Which lock
   variant is now needed, and what changes on PREEMPT_RT?
10. Write a per-CPU counter with `local_lock_t` and show with `ftrace`
    (`preemptirq` events) that on a non-RT kernel it compiles to
    `preempt_disable()`.
11. Build a kernel for a QEMU arm64 VM with `CONFIG_KCSAN=y`, load
    `ksync_spin`, and find the KCSAN report for `plain_counter`. Then wrap
    the access in `data_race()` and explain why that silences it and why
    you would never do that for a real counter.

---

## 10. Remember forever

```
CONTEXT decides: hardirq/softirq/spinlock held = ATOMIC = never sleep.
Shared with... softirq/timer -> spin_lock_bh   | hardirq -> spin_lock_irqsave
               only processes -> mutex (_interruptible in syscalls)
               true hard IRQ on PREEMPT_RT -> raw_spinlock_t
atomic_inc() is RELAXED; *_return/cmpxchg/xchg are FULLY ORDERED.
publish: smp_store_release(&flag)  ...  smp_load_acquire(&flag)  (arm64: stlr/ldar)
wait: completion for one event; wait_event_*(wq, cond) + wake_up for conditions;
      .poll = poll_wait(file, &wq, pt) + return mask (never sleeps)
read-mostly: seqlock = readers retry & only copy; RCU = readers free, writer
      publishes copy, frees after grace period (synchronize_rcu / call_rcu / kfree_rcu)
down_trylock()==0 means SUCCESS.  rcu_barrier() before rmmod after call_rcu.
Turn on PROVE_LOCKING + DEBUG_ATOMIC_SLEEP + DEBUG_SPINLOCK while developing.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name six sources of concurrency in the kernel. Which of them exist on a
   single-CPU, non-preemptible kernel?
2. What is "atomic context"? List four things you may not do in it and why.
3. Draw the single-CPU deadlock that `spin_lock_irqsave()` prevents. Why is
   a second CPU's IRQ handler not a problem?
4. Data is shared between a `read()` syscall and a `timer_list` callback.
   Which lock and which variant in each place? What if it were an `hrtimer`
   in hard mode?
5. Why does `spin_lock_irq()`/`spin_unlock_irq()` break if the caller already
   had interrupts disabled?
6. What changes for `spinlock_t` under `PREEMPT_RT`, and when do you need
   `raw_spinlock_t`?
7. Which atomic operations imply a full memory barrier and which do not?
   What does `atomic_inc` compile to on a Cortex-A72 and on a Cortex-A76?
8. Explain release/acquire in one sentence each and write the message-passing
   pattern with kernel APIs.
9. Mutex vs semaphore vs rw_semaphore: three differences each. Why can a
   mutex spin before sleeping?
10. What is priority inversion and which kernel lock solves it? Name one
    driver subsystem that uses it.
11. Why must the task state be set *before* testing the condition in a wait
    loop? What does `wait_event_interruptible` return on a signal and what
    does user space see?
12. How do a driver's blocking `read()` and its `.poll` share one wait queue?
    Why must `.poll` never sleep?
13. Seqlock vs RCU: when is each better? Why may a seqlock reader not follow
    a pointer?
14. What exactly does `synchronize_rcu()` wait for? Why is `rcu_barrier()`
    needed in module exit after `call_rcu()`?
15. What does lockdep report, and how can it find a deadlock that never
    happened? Name the three Kconfig options you enable for lock debugging.
16. Map `pthread_mutex_t`, `sem_t`, `pthread_cond_t`, `pthread_rwlock_t`,
    C11 `atomic_fetch_add` and liburcu to their kernel equivalents.
