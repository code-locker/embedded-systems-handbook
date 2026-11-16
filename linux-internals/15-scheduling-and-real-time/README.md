# 15 — Scheduling and Real-Time

> The **scheduler** decides which thread runs on each CPU, and for how long.
> Most of the time it is invisible. Then a motor-control loop jitters, an
> audio stream crackles or a camera pipeline drops frames, and you have to
> know how it decides: scheduling classes, nice weights, real-time
> priorities, deadlines, preemption models, clocks and timers. This lesson
> covers all of them and ends with the recipe for a real-time thread on the
> BeagleBone AI-64's PREEMPT_RT kernel.

---

## 1. Why this exists

A TDA4VM has two Cortex-A72 cores and, on a busy system, hundreds of
runnable threads: your capture and control threads, GStreamer streaming
threads, kernel workers, IRQ threads, `systemd`, `sshd`. Something has to
share two CPUs between them. That something has two opposite goals:

| Goal | Wants | Example |
|---|---|---|
| **Throughput and fairness** | every thread gets its share, few context switches | a compile job, an encoder, a web server |
| **Latency and determinism** | *this* thread runs within X µs of its event, every time | a 1 kHz control loop, a CAN heartbeat, audio |

Linux serves the first with the **fair class** (CFS, replaced by EEVDF in
6.6) and the second with the **real-time classes** (`SCHED_FIFO`,
`SCHED_RR`, `SCHED_DEADLINE`) plus the **PREEMPT_RT** kernel, which the
board runs. The time side matters as much: which clock you read, how you
sleep, and how you measure lateness decide whether a "periodic" task is
periodic at all.

| You need... | Use |
|---|---|
| a background job that should not disturb anything | `nice 10`, `SCHED_BATCH` or `SCHED_IDLE` (§3.4) |
| a thread that must run before all normal work | `SCHED_FIFO` priority 1–99 (§3.4) |
| a guaranteed CPU budget every period | `SCHED_DEADLINE` (§3.5) |
| a periodic loop that does not drift | `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)` (§3.9) |
| a timeout or interval measurement | `CLOCK_MONOTONIC`, never `CLOCK_REALTIME` (§3.8) |
| a timer that wakes an event loop | `timerfd` ([13](../13-signals-eventfd-and-epoll/README.md)) or `timer_create` (§3.10) |
| bounded worst-case latency | PREEMPT_RT + the recipe in §3.12 |

---

## 2. Mental model

**The scheduler is an airport check-in hall with a strict hierarchy of
desks.** There is one hall per CPU (a **runqueue**). Inside it are several
queues, checked in a fixed order: first the airline staff (`stop`), then
passengers with a booked deadline (`deadline`), then first class in strict
rank order 99 down to 1 (`rt`), then everyone else (`fair`), and last the
cleaner who only works when the hall is empty (`idle`). A higher queue
always wins. Inside the fair queue, nobody has rank: each passenger has a
"time already served" counter (**vruntime**), the one who has been served
least goes next, and heavier tickets (lower nice) make your counter run
slower, so you get served more.

```
  per-CPU runqueue (struct rq), picked top to bottom every time:

  +---------------------------------------------------------------+
  | stop      | migration/N, stop_machine        (always wins)     |
  | deadline  | EDF: earliest absolute deadline  SCHED_DEADLINE    |
  | rt        | 100 FIFO lists, bitmap -> O(1)   SCHED_FIFO / RR   |
  |           |   prio 99 [T1]                                     |
  |           |   prio 80 [T2]->[T3]   (RR: rotate on slice end)   |
  |           |   prio  1 [ ]                                      |
  | fair      | EEVDF red-black tree by virtual deadline           |
  |           |   SCHED_OTHER / SCHED_BATCH / SCHED_IDLE(weight 3) |
  |           |        (vd=12)                                     |
  |           |        /     \                                     |
  |           |    (vd=7)   (vd=20)   leftmost eligible runs next  |
  | (ext)     | sched_ext BPF schedulers (6.12+, if loaded)        |
  | idle      | the idle task: WFI on the A72                      |
  +---------------------------------------------------------------+
```

Remember this sentence: **a runnable real-time thread always beats every
fair thread on its CPU; within the fair class, nice changes the *share*,
not the order.**

---

## 3. How it works

### 3.1 Scheduling classes and runqueues (`c/01_sched_policies.c`)

Each CPU has a `struct rq` with one sub-runqueue per **scheduling class**.
A class is a table of function pointers (`enqueue_task`, `pick_next_task`,
`task_tick`, `check_preempt_curr`...). `__schedule()` asks each class in
priority order for a task and runs the first one it gets.

| Class | Policies | Picks by | Typical users |
|---|---|---|---|
| `stop` | (kernel only) | — | CPU hotplug, `stop_machine`, migration |
| `dl` | `SCHED_DEADLINE` | earliest absolute deadline (EDF) | periodic RT jobs with a known budget |
| `rt` | `SCHED_FIFO`, `SCHED_RR` | highest static priority 1–99 | control loops, IRQ threads on RT, audio |
| `fair` | `SCHED_OTHER`, `SCHED_BATCH`, `SCHED_IDLE` | EEVDF virtual deadline (6.6+), CFS vruntime before | almost everything |
| `ext` | `SCHED_EXT` | a BPF program (6.12+) | experiments, datacenter policies |
| `idle` | (kernel only) | — | the per-CPU idle loop |

**When does the scheduler run?** When the current thread blocks (sleep,
lock, I/O), on the timer tick if a fair thread used up its slice, when a
higher-priority thread wakes up (**wakeup preemption**), and when a thread
yields. "Run the scheduler" means: set `TIF_NEED_RESCHED`, and at the next
**preemption point** call `schedule()`. Where those preemption points are is
what the preemption model decides (§3.3).

Load balancing moves threads between CPUs' runqueues (periodically, and
when a CPU goes idle). RT tasks are pushed/pulled more eagerly: the kernel
tries to run the N highest-priority RT tasks on N CPUs.

### 3.2 The fair class: vruntime, weights, EEVDF (`c/02_nice_weights.c`)

Each fair thread has a **weight** from its nice value
(`sched_prio_to_weight[]` in `kernel/sched/core.c`): nice 0 = 1024, and each
nice step is ×1.25. Its **vruntime** advances at
`real_runtime × 1024 / weight`, so a heavy (low-nice) thread's clock runs
slowly and it gets more real CPU before it looks "served".

```
 two threads on ONE CPU: nice 0 (weight 1024) and nice 5 (weight 335)

 real CPU time:   A ######################### 75%   B ######## 25%
 vruntime:        A ------------------------> 300 ms * 1024/1024 = 300
                  B ------------------------> 100 ms * 1024/335  = 306   <- level
 share(A) = 1024 / (1024 + 335) = 75.3 %     (c/02 measured 75.2 %)
```

- **CFS** (2.6.23–6.5): always run the thread with the smallest vruntime,
  kept in a red-black tree. Latency was tuned with heuristics
  (`sched_latency_ns`, `sched_min_granularity_ns`, wakeup granularity).
- **EEVDF** (6.6+, the board's 6.18 kernel): each thread has a **lag**
  (how much service it is owed versus a perfectly fair CPU) and a **virtual
  deadline** = eligible time + slice/weight. Among *eligible* threads (lag
  ≥ 0) the one with the earliest virtual deadline runs. Fairness is the
  same; latency becomes a parameter: since 6.12 a thread may ask for a
  shorter slice with `sched_setattr(.sched_runtime = ...)` on a fair
  policy, getting earlier deadlines without getting more CPU.
- **`SCHED_BATCH`**: like OTHER, but never preempts others on wakeup. For
  CPU-bound batch work.
- **`SCHED_IDLE`**: weight 3 (less than nice 19's 15). Runs when nobody
  else wants the CPU.
- **Autogroup** (`kernel.sched_autogroup_enabled`, on in Ubuntu): threads
  are grouped per session (terminal) and *groups* share the CPU first, so
  `nice` only competes inside your session. cgroups `cpu.weight` does the
  same thing explicitly (systemd slices).

`c/02` pins two spinning threads with nice 0 and nice 5 to one CPU for
400 ms and prints the ratio (≈3.05 : 1). It asserts only that the nicer
thread did not get more. It also prints `/proc/<pid>/sched`, where
`se.load.weight` shows 1048576 = 1024 << 10 (the kernel keeps weights in
fixed point).

### 3.3 Preemption models: what PREEMPT_RT changes

*Preemption* = taking the CPU away from a running thread when a more
important one becomes runnable. In user space that can happen at any
instruction (the timer IRQ returns to the scheduler). The question is
**kernel code**: if thread A is inside a syscall when high-priority B wakes
up, when does B get the CPU?

| Model | Kernel code is preempted... | Worst-case latency | Who uses it |
|---|---|---|---|
| `PREEMPT_NONE` | only when it blocks or returns to user space | long (ms) | servers, throughput |
| `PREEMPT_VOLUNTARY` | also at `might_sleep()`/`cond_resched()` points | shorter | old desktop default |
| `PREEMPT` (full) | anywhere, except with spinlocks held / IRQs off / preempt_disable | ~100 µs–ms | low-latency desktops |
| `PREEMPT_DYNAMIC` | one of the three above, chosen at boot (`preempt=`) | — | Ubuntu generic (this host) |
| `PREEMPT_LAZY` (6.13+) | full for RT tasks, at the tick for fair tasks | — | the future default |
| **`PREEMPT_RT`** | **almost everywhere**, even inside most "spinlocks" | **tens of µs** | **the BeagleBone's 6.18-rt kernel** |

What `PREEMPT_RT` (mainline since 6.12) changes:

```
 stock kernel                                PREEMPT_RT
 ------------                                ----------
 hard IRQ handler runs with IRQs off   -->   tiny primary handler; the real
 (any length!)                               handler is a kthread "irq/42-xyz"
                                             at SCHED_FIFO 50: you can set
                                             its priority (chrt) below your loop
 softirqs run on IRQ exit, any length  -->   softirqs run in thread context,
                                             preemptible
 spinlock_t: spins, preemption off     -->   spinlock_t = rt_mutex-based
                                             SLEEPING lock, preemptible;
                                             only raw_spinlock_t still spins
 mutex/rwsem: no PI in the kernel      -->   PRIORITY INHERITANCE everywhere
                                             (spinlock_t, rwlock_t, mutex)
 long preempt-off/IRQ-off regions      -->   removed or bounded
```

Result: when your SCHED_FIFO 80 thread's timer fires, very little in the
kernel can delay it longer than a few tens of µs. That is what makes an RT
loop on the A72 possible. You do not need to change your user code for RT,
but the rules of §3.12 become *worth* following: on a stock kernel the
kernel's own latency hides your mistakes, on RT your mistakes are what is
left. Check which kernel you are on with `uname -v` (look for `PREEMPT_RT`)
or `cat /sys/kernel/realtime` (`1` on RT). `c/01` prints both.

Driver writers see the other side in
[linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md)
and [04-interrupts-timers-and-deferred-work](../../linux-device-drivers/04-interrupts-timers-and-deferred-work/README.md):
on RT a `spinlock_t` section may sleep, and `raw_spinlock_t` must be short.

### 3.4 Policies and priorities (`c/01_sched_policies.c`)

| Policy | Class | Priority | Unprivileged may... |
|---|---|---|---|
| `SCHED_OTHER` (`SCHED_NORMAL`) | fair | nice −20..19 | raise nice; lower it only within `RLIMIT_NICE` |
| `SCHED_BATCH` | fair | nice | switch to it freely |
| `SCHED_IDLE` | fair | — | switch to it, **not back** (needs `RLIMIT_NICE ≥ 20−nice`) |
| `SCHED_FIFO` | rt | 1..99, runs until it blocks/yields or a higher one wakes | only with `RLIMIT_RTPRIO` ≥ prio or `CAP_SYS_NICE` |
| `SCHED_RR` | rt | 1..99, like FIFO plus a time slice (`sched_rr_timeslice_ms`, 100 ms) among equals | same |
| `SCHED_DEADLINE` | dl | runtime/deadline/period | only with `CAP_SYS_NICE` |

```
  kernel "prio" (what ps -o prio / top PR show, lower = more important):
     0 ........ 98 99 | 100 ............ 139
     RT 99 ... RT 1   | nice -20 ... nice 19   (120 = nice 0)
  user API:  sched_priority 1..99 (higher = more important) for FIFO/RR
```

Key facts, all checked by `c/01`:

- **Policy and nice are per thread.** `setpriority(PRIO_PROCESS, tid, n)`
  changes one thread on Linux. `pthread_setschedparam` too. The main thread
  stays unchanged.
- `pthread_*` functions return the error number. `sched_*` functions return
  −1 and set `errno`.
- New threads **inherit** the creator's policy unless you set
  `PTHREAD_EXPLICIT_SCHED` on the attribute. Without it,
  `pthread_attr_setschedpolicy` is silently ignored (`c/08`, `c/09`).
- `SCHED_RESET_ON_FORK` (or-ed into the policy): children start as
  `SCHED_OTHER`, so an RT daemon cannot leak RT into helper processes.
- **Grant RT to a user** (on the board you are usually root anyway):
  `/etc/security/limits.d/rt.conf` with `@realtime - rtprio 95` and
  `@realtime - memlock unlimited`, or `setcap cap_sys_nice+ep ./prog`, or
  systemd's `LimitRTPRIO=` / `CPUSchedulingPolicy=fifo`.

### 3.5 SCHED_DEADLINE (`c/03_sched_deadline.c`)

Instead of a priority you declare a reservation: **runtime** (worst-case
execution time per job), **deadline** (relative to the period start) and
**period**. The kernel runs DL tasks by Earliest Deadline First and polices
each with a Constant Bandwidth Server: a task that exceeds its runtime is
**throttled** until its next period, so it cannot hurt anybody else.

```
 runtime=2 ms, deadline=5 ms, period=10 ms   (bandwidth 20 % of one CPU)

 |<------------------ period 10 ms ------------------>|
 |####..........|..................................... |####...
 0   2 ms     5 ms (job must be done by here)         10 ms
     ^ sched_yield(): "job done, wake me next period"
```

- glibc 2.35 has no wrapper: `syscall(SYS_sched_setattr, tid, &attr, 0)`
  with a locally defined `struct sched_attr` (48 bytes, times in ns).
- Rules: `runtime ≤ deadline ≤ period`. **Admission control**: the sum of
  `runtime/period` of all DL tasks on a root domain must stay under
  `sched_rt_runtime_us/sched_rt_period_us × CPUs`, else `EBUSY`.
- DL tasks cannot `fork()`, and their affinity must cover the whole root
  domain (partition CPUs with cpusets instead of `taskset`).
- `SCHED_FLAG_DL_OVERRUN` sends `SIGXCPU` on an overrun, a built-in
  watchdog.
- Needs `CAP_SYS_NICE`. `RLIMIT_RTPRIO` is not enough. As a normal user,
  `c/03` shows `EINVAL` for bad parameters and `EPERM` for the real request,
  and explains `chrt -d`.

### 3.6 RT throttling

A `SCHED_FIFO` thread in a `while (1);` loop would own its CPU forever,
including the shell you would use to kill it. So by default RT tasks may
use at most **950 ms per 1 s** of each CPU:

```
 /proc/sys/kernel/sched_rt_period_us   = 1000000
 /proc/sys/kernel/sched_rt_runtime_us  =  950000    (-1 = no limit)
```

The last 5 % goes to fair tasks. If an RT thread really is CPU-bound,
you see `sched: RT throttling activated` in `dmesg`, and your "real-time"
thread stops for 50 ms. Treat that message as a bug in the RT design, not as
a limit to raise. (Since 6.12 the mechanism is being replaced by
*deadline servers* that reserve the 5 % for fair tasks; the knobs stay.)

### 3.7 Priority inversion: the recap

Fixed priorities plus a shared lock can make a high-priority thread wait
for a medium one that does not even use the lock. The Mars Pathfinder
story, the timelines and the two protocols (inheritance and ceiling) are in
[03 §3.6](../03-mutexes-and-deadlock/README.md). In short: **every mutex an
RT thread shares with a lower-priority thread is
`PTHREAD_PRIO_INHERIT`**. `c/09` does exactly that for the hand-off to its
logger thread. PREEMPT_RT applies the same rule to the kernel's own locks.

### 3.8 Clocks (`c/04_clocks.c`)

| Clock | Counts | Jumps? | Use for |
|---|---|---|---|
| `CLOCK_REALTIME` | seconds since 1970 (wall time) | **yes**: NTP step, `date -s`, RTC sync | timestamps for humans and logs, nothing else |
| `CLOCK_MONOTONIC` | time since boot, not counting suspend | no; NTP *slews* its rate (≤ 500 ppm) | timeouts, intervals, periodic deadlines |
| `CLOCK_MONOTONIC_RAW` | the raw hardware counter | no, and no slew | comparing with hardware timestamps, measuring clock drift |
| `CLOCK_BOOTTIME` | MONOTONIC + time suspended | no | "how long since X" across suspend (e.g. a battery device) |
| `CLOCK_PROCESS_CPUTIME_ID` | CPU time of all threads | no | profiling, CPU budgets |
| `CLOCK_THREAD_CPUTIME_ID` | CPU time of this thread | no | per-thread budget, "did I actually run?" |

```
 a board without an RTC battery:
   boot: REALTIME = 1970-01-01 ...   NTP sync: REALTIME += 56 years
   MONOTONIC:  0 s ---- 30 s ---- 31 s ---- 32 s ...   (unaffected)
   a REALTIME deadline "now + 5 s" set before the sync expires instantly.
```

- `clock_getres()` returns 1 ns for the hrtimer-backed clocks (on the A72
  the ARM generic timer feeds them; `dmesg | grep arch_timer` shows its
  frequency).
- The first four are read through the **vDSO**: a function the kernel maps
  into every process that reads the hardware counter plus a shared data page.
  No syscall, ~20 ns at `-O2`. The CPU-time clocks are real syscalls (`c/04`
  prints both costs).
- `pthread_getcpuclockid()` and `clock_getcpuclockid(pid)` give you the CPU
  clock of another thread or process.

### 3.9 Sleeping accurately: relative vs absolute (`c/05_periodic_drift.c`, `c/06_relative_sleep_drift_bug.c`)

```
 relative:  |work|--sleep(P)--|work|--sleep(P)--|work|--sleep(P)--|
            each cycle = P + work + latency  -> error ACCUMULATES (N x)

 absolute:  t0        t0+P      t0+2P     t0+3P        next += P;
            |work|....|work|....|work|....|work|       clock_nanosleep(
            each wakeup is late by ONE latency,          MONOTONIC, ABSTIME,
            never by the sum                             &next)
```

The pattern every periodic task should use:

```c
struct timespec next;
clock_gettime(CLOCK_MONOTONIC, &next);
for (;;) {
    timespec_add_ns(&next, period_ns);            /* keep tv_nsec < 1e9! */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
    do_cycle();                                   /* if now > next: overrun */
}
```

- `clock_nanosleep` **returns** the error number, it does not set `errno`.
- With `TIMER_ABSTIME`, an `EINTR` retry sleeps to the same instant. With a
  relative `nanosleep` you must pass the remaining time back in.
- An absolute sleep never wakes early, so `now - next ≥ 0` is a latency
  sample (that is how cyclictest works, §3.11).
- If `now > next` after the work, you **overran**: decide the policy (skip
  missed releases and stay on the grid as in `cpp/01`, or run them back to
  back).

`c/05` runs 100 × 2 ms cycles with 0.3 ms of work both ways: the relative
loop ends ~37 ms late, the absolute loop ~0.06 ms. `c/06` is the bug as it
appears in real code (`usleep(1000)` after the work, "1 kHz") and its
self-check fails: the loop really runs at ~785 Hz.

### 3.10 Timers (`c/07_posix_timers.c`)

| API | Notifies by | Notes |
|---|---|---|
| `timer_create` + `SIGEV_SIGNAL` | a queued (RT) signal with your `sigev_value` | any clock, many timers per process, **overrun count** |
| `timer_create` + `SIGEV_THREAD_ID` | a signal to one specific thread (Linux) | route ticks to your RT thread |
| `timer_create` + `SIGEV_THREAD` | glibc starts a thread and calls your function | convenient, but a new thread per expiry and invisible to TSan: avoid in RT code |
| `timerfd_create` | the fd becomes readable; `read()` returns the expiration count | fits epoll loops ([13 §3.9](../13-signals-eventfd-and-epoll/README.md)) |
| `setitimer`/`alarm` | `SIGALRM` | one per process, legacy |

**Overrun**: while the previous timer signal is still pending, further
expirations are not queued but counted. `timer_getoverrun()` (or
`si_overrun`) tells you how many periods you missed. `c/07` stops collecting
for 55 ms on a 5 ms timer and gets **one** signal with `si_overrun = 10`.
It also arms a one-shot timer (`it_interval = 0`) and a
`CLOCK_THREAD_CPUTIME_ID` timer that does not fire while the thread sleeps,
only after 30 ms of actual CPU.

Collect timer signals **synchronously**: block the signal in all threads,
then `sigtimedwait()` / `sigwaitinfo()` (or `signalfd`). No handler, so no
async-signal-safety rules.

### 3.11 Measuring latency: cyclictest (`c/08_cyclictest_lite.c`)

Wakeup latency = (time the thread runs again) − (time its timer expired).
It is the sum of IRQ latency, time spent with preemption or IRQs disabled,
scheduler and context switch. **Only the maximum matters** for real time:
an average of 10 µs with one 3 ms spike per hour is a 3 ms system.

```
 expected           actual
    |<--- latency --->|
 ---+-----------------+------------------> CLOCK_MONOTONIC
    ^ hrtimer IRQ -> wake thread -> pick_next -> context switch -> run
```

`c/08` does what `cyclictest` does: a thread sleeps to absolute 1 ms
deadlines 500 times, records `now - next`, and prints min/avg/p99/max and a
histogram. It tries `SCHED_FIFO 80` (falls back to `SCHED_OTHER`) and
measures twice:

| run | timer slack | min | avg | max (this host, SCHED_OTHER) |
|---|---|---|---|---|
| 1 | 50 µs (default for SCHED_OTHER) | ~40 µs | ~65 µs | ~200 µs |
| 2 | 1 ns (`prctl(PR_SET_TIMERSLACK, 1)`) | ~5 µs | ~35 µs | ~110 µs |

**Timer slack** lets the kernel delay a fair thread's timer by up to
50 µs to batch wakeups and save power. RT threads have no slack. If you
measure with SCHED_OTHER and see a 50 µs floor, that is policy, not
latency. On the board, the real tool:

```bash
sudo cyclictest -m -S -p 90 -i 1000 -h 400 -D 10m -q > hist.txt   # -m mlockall, -S one thread per CPU
stress-ng --cpu 2 --io 2 --vm 1 --vm-bytes 256M --timeout 10m &   # measure UNDER load
```

### 3.12 The RT application recipe (`c/09_rt_app_recipe.c`)

```
 BEFORE the loop (slow, may fault, may allocate)      IN the loop (fast, bounded)
 ----------------------------------------------       ----------------------------
 1 mallopt(M_TRIM_THRESHOLD,-1), (M_MMAP_MAX,0)       clock_nanosleep(ABSTIME)
 2 mlockall(MCL_CURRENT | MCL_FUTURE)                 compute on preallocated data
 3 prefault: memset the heap pool, touch the stack    PI-mutex hand-off / lock-free ring
 4 SCHED_FIFO prio via attr + EXPLICIT_SCHED          heartbeat++ for the watchdog
 5 pin to an isolated CPU                             NO malloc/free, printf, file I/O,
 6 PI mutexes for anything shared with non-RT            new pages, unbounded syscalls,
 7 a watchdog that notices a stalled loop                locks shared with non-PI code
```

| Step | Why | Without privilege |
|---|---|---|
| 1 `mallopt` | glibc would return freed memory to the kernel (`trim`) and use `mmap` for big blocks, so a later `malloc` faults in fresh pages | works |
| 2 `mlockall` | no page is ever swapped out or lazily faulted in after this | needs `RLIMIT_MEMLOCK` ≥ the process size (`ulimit -l`; several GB on this host, 8 MiB on many systems) |
| 3 prefault | `malloc` only reserves address space; the **first touch** of each page is a page fault (zeroing 4 KiB, µs). Touch everything now | works |
| 4 `SCHED_FIFO` | beats every fair thread and lower RT threads | `EPERM`: limits.conf `rtprio`, `CAP_SYS_NICE`, root |
| 5 pin + isolate | no migration, warm caches, no competition | pinning works; isolation is a boot option |
| 6 PI mutex | bounded priority inversion ([03](../03-mutexes-and-deadlock/README.md)) | works |
| 7 watchdog | an RT bug is a stalled loop; detect it and go to a safe state | works (`/dev/watchdog` needs root) |

**CPU isolation on the TDA4VM (2 × A72):** boot with
`isolcpus=1 nohz_full=1 rcu_nocbs=1 irqaffinity=0`.
`isolcpus` keeps the scheduler from putting other tasks on CPU 1;
`nohz_full` stops the periodic tick there while one task runs;
`rcu_nocbs` moves RCU callbacks off it; `irqaffinity=0` routes IRQs to CPU 0
(check `/proc/interrupts`, move stragglers via `/proc/irq/N/smp_affinity`).
Then pin the RT thread to CPU 1 and leave everything else on CPU 0. With
two cores this is a real trade-off: CPU 0 then carries Linux, GStreamer and
all IRQs alone. Heavy hard real-time work on this SoC belongs on the R5F
cores; the A72 RT thread is for soft/firm real time and supervision.

`c/09` applies every step and prints a table (`ok` / `skip` / `NO` with the
fix). Under ASan/TSan it **skips `mlockall`**: the sanitizers map terabytes
of `MAP_NORESERVE` shadow memory, so `mlockall` fails with `ENOMEM`, or,
with `CAP_IPC_LOCK`, tries to populate all of it.

### 3.13 Tools

| Tool | What for |
|---|---|
| `chrt -f 80 ./prog`, `chrt -p <pid>` | run with / show / change the RT policy (`-r` RR, `-b` batch, `-i` idle, `-d` deadline) |
| `taskset -c 1 ./prog`, `taskset -pc <pid>` | CPU affinity |
| `nice -n 10`, `renice` | nice values |
| `ps -eLo pid,tid,cls,rtprio,ni,pri,psr,comm` | policy (`cls` = TS/FF/RR/B/IDL/DLN), RT prio, nice, CPU of every thread |
| `cyclictest` (rt-tests) | wakeup latency histogram (§3.11) |
| `stress-ng` | generate CPU/IO/memory/IRQ load while measuring |
| `/proc/<pid>/sched`, `/proc/<pid>/status` (`voluntary_ctxt_switches`) | per-task scheduler state |
| `/sys/kernel/debug/sched/debug` (was `/proc/sched_debug` before 5.13) | every runqueue, every task, vruntime |
| `perf sched record -- sleep 5; perf sched latency; perf sched timehist` | who waited how long for a CPU |
| `trace-cmd record -e sched_switch -e sched_wakeup`, `kernelshark` | the timeline of every context switch |
| `osnoise` / `timerlat` tracers (`rtla timerlat top`) | where latency comes from on RT kernels |

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| pick next RT task | O(1) | bitmap of 100 priority lists, find-first-set |
| pick next fair task (EEVDF/CFS) | O(log n) | augmented red-black tree, cached leftmost |
| enqueue/dequeue fair task | O(log n) | rb-tree insert/erase |
| pick next DL task | O(log n) | rb-tree by absolute deadline |
| context switch | ~1–5 µs on the A72 | save/restore registers, switch `mm` (TLB/ASID), cache effects dominate |
| `clock_gettime(MONOTONIC)` | ~20 ns, no syscall | vDSO reads the counter |
| `clock_gettime(THREAD_CPUTIME_ID)` | ~0.3–1 µs | real syscall |
| `clock_nanosleep` wakeup latency | stock: 50 µs–ms; RT + FIFO + isolation: ~10–50 µs | IRQ + scheduler + switch + timer slack (fair only) |
| `timer_create`/`settime` | one syscall, hrtimer O(log n) | hrtimers live in a per-CPU rb-tree (timerqueue) |
| SCHED_DEADLINE admission | O(1) per request | sum of bandwidths per root domain |
| Memory | `struct task_struct` ~ 8–10 KiB; one `sched_entity` / `sched_rt_entity` / `sched_dl_entity` each | every thread can switch class at runtime |

Timer data structures are DSA in practice: the kernel's hrtimer queue is a
red-black tree with a cached minimum (see the timer queue in
[dsa/10-heaps-priority-queues](../../dsa/10-heaps-priority-queues/README.md)),
and the low-resolution `timer_list` wheel is the hierarchical timer wheel of
[dsa/19-system-design-structures](../../dsa/19-system-design-structures/README.md).

---

## 5. C vs C++

| Concept | C (POSIX/Linux) | C++17 |
|---|---|---|
| Monotonic time | `clock_gettime(CLOCK_MONOTONIC)` | `std::chrono::steady_clock` (= CLOCK_MONOTONIC in libstdc++) |
| Wall time | `CLOCK_REALTIME` | `std::chrono::system_clock` |
| Durations | `int64_t` ns + manual `timespec` normalisation | `std::chrono::duration`, literals `2ms`, `300us`, type-checked units |
| Absolute sleep | `clock_nanosleep(..., TIMER_ABSTIME, ...)` | `std::this_thread::sleep_until(tp)` (GCC 11: loop of relative sleeps) or a wrapper (`cpp/01`) |
| Policy / priority | `pthread_setschedparam`, `sched_setattr` | not in the standard: `std::thread::native_handle()` + pthreads |
| Affinity | `pthread_setaffinity_np` | same, via `native_handle()` |
| Restore on scope exit | manual, every error path | RAII guard (`cpp/02`), `std::optional` for "may be EPERM" |
| Periodic task | a loop | a class with stats and an overrun policy (`cpp/01`) |

Two C++ details from the programs:

- `cpp/01` converts a `steady_clock::time_point` straight into a
  `CLOCK_MONOTONIC` `timespec`. That relies on libstdc++ (and libc++)
  implementing `steady_clock` with `CLOCK_MONOTONIC` on Linux. A
  `static_assert(is_steady)` documents it.
- `cpp/02`'s destructor cannot throw, but restoring may fail (leaving
  `SCHED_IDLE` needs privilege). It records the error instead. Only guard
  changes you are allowed to undo.
- Under TSan with GCC 11, prefer `clock_nanosleep` over
  `condition_variable::wait_until(steady_clock)`: libstdc++ uses
  `pthread_cond_clockwait`, which GCC 11's TSan does not intercept (see
  [04](../04-condition-variables/README.md)).

---

## 6. Where it is used in the real world

- **Linux kernel.** `kernel/sched/{core,fair,rt,deadline,idle,stop_task,ext}.c`.
  IRQ threads on PREEMPT_RT are `SCHED_FIFO 50` (`ps -eLo cls,rtprio,comm | grep irq/`);
  RCU boost and `ktimers`/`ksoftirqd` threads have RT priorities too, so
  pick your loop's priority relative to them (often 80–90, above the IRQ
  threads you do not depend on, below the one you do).
- **Industrial control (the BeagleBone's natural job).** LinuxCNC, EtherCAT
  masters (IgH, SOEM), CODESYS and ROS 2 real-time executors all use
  PREEMPT_RT + `SCHED_FIFO` + `mlockall` + absolute `clock_nanosleep` loops,
  exactly §3.12.
- **Audio.** JACK and PipeWire run their processing threads at `SCHED_FIFO`
  (through rtkit, which hands out RT priority with `RLIMIT_RTTIME` as a
  safety net). A buffer underrun is a missed deadline you can hear.
- **GStreamer.** `GstClock` / `GstSystemClock` uses `CLOCK_MONOTONIC` and
  `clock_nanosleep`-style absolute waits for sink synchronisation; a
  `GstTaskPool` lets you give streaming threads RT priority. Latency
  tuning on the TDA4VM is in
  [gstreamer/14-embedded-tda4vm](../../gstreamer/14-embedded-tda4vm/README.md).
- **Android.** `SCHED_FIFO` for the audio HAL and SurfaceFlinger's render
  thread, cgroup `cpu.uclamp` for "top-app".
- **systemd.** `CPUSchedulingPolicy=`, `CPUSchedulingPriority=`,
  `CPUAffinity=`, `LimitRTPRIO=`, `LimitMEMLOCK=`, `WatchdogSec=`: the whole
  recipe as unit-file options.
- **RTOS comparison.** FreeRTOS on the TDA4VM's R5F cores is *only* fixed
  priorities (like `SCHED_FIFO`, preemptive, with an optional time slice like
  `SCHED_RR`). There is no fair class and no virtual memory, so no page
  faults: that is why hard deadlines usually go there.

---

## 7. Common mistakes and pitfalls

1. **Relative sleep in a periodic loop.** `usleep(period)` after the work
   drifts by work+latency every cycle (`c/06`). Use absolute deadlines.
2. **Measuring intervals or setting deadlines with `CLOCK_REALTIME`** or
   `gettimeofday()`. It jumps on NTP sync. Use `CLOCK_MONOTONIC` /
   `steady_clock`.
3. **Forgetting `PTHREAD_EXPLICIT_SCHED`.** The attribute's policy is
   silently ignored and the thread inherits SCHED_OTHER.
4. **Ignoring the return value of `pthread_setschedparam`.** On a dev PC it
   is `EPERM`, the program "works" and the latency numbers are meaningless.
   Print what you actually got (`pthread_getschedparam`).
5. **A CPU-bound `SCHED_FIFO` thread.** It starves everything on its CPU,
   including kernel threads it depends on, until RT throttling kicks in and
   stops *it* for 50 ms. RT threads must block regularly.
6. **Priority 99 for everything.** Priority 99 is for the kernel's own
   watchdog/migration-like threads. Leave room above you and below; design
   a priority map.
7. **`malloc`, `printf`, `std::string`, `std::vector::push_back`, logging,
   or file I/O inside the RT loop.** Each can take a lock, fault in a page or
   block. Preallocate; hand data to a non-RT thread through a lock-free ring
   or a PI mutex.
8. **`mlockall` without prefaulting** (or prefaulting without `mlockall`).
   You need both: locked *and* already mapped. A thread's stack is only
   mapped as far as it has been touched.
9. **Sharing a plain mutex between RT and non-RT threads.** Unbounded
   priority inversion. Use `PTHREAD_PRIO_INHERIT`.
10. **Expecting `nice` to work across terminals.** Autogroup puts each
    session in its own group. Use cgroups `cpu.weight`, or disable autogroup.
11. **Benchmarking latency without load.** Idle-system latency is
    meaningless. Run `stress-ng` (CPU, I/O, memory, network, IRQs) and look
    at the maximum over hours.
12. **Measuring SCHED_OTHER latency and blaming the kernel** for a 50 µs
    floor that is timer slack (`c/08`).
13. **`SCHED_IDLE` (or a high nice) in a guard you expect to undo.**
    Without `CAP_SYS_NICE` you cannot go back (`c/01`, `cpp/02`).
14. **Calling `mlockall` under ASan/TSan.** Shadow memory is terabytes;
    skip it under `__SANITIZE_ADDRESS__` / `__SANITIZE_THREAD__`.
15. **A small explicit thread stack under TSan.** glibc carves static TLS
    from the top of the thread stack and TSan's per-thread state is large:
    a 512 KiB stack crashed `c/09` before the first line of the thread
    function. Use ≥ 2 MiB in tested programs.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_sched_policies.c](c/01_sched_policies.c) | preemption model and RT throttling of this kernel, priority ranges, nice/BATCH/IDLE per thread, SCHED_FIFO with EPERM explanation, IDLE is one-way | `make run P=linux-internals/15-scheduling-and-real-time/c/01_sched_policies.c` |
| [c/02_nice_weights.c](c/02_nice_weights.c) | two spinners pinned to one CPU with nice 0/5: CPU share vs the weight table, `/proc/<pid>/sched` | `make run P=linux-internals/15-scheduling-and-real-time/c/02_nice_weights.c` |
| [c/03_sched_deadline.c](c/03_sched_deadline.c) | raw `sched_setattr`/`sched_getattr`, EINVAL rules, 2/5/10 ms reservation, EPERM/EBUSY paths | `make run P=linux-internals/15-scheduling-and-real-time/c/03_sched_deadline.c` |
| [c/04_clocks.c](c/04_clocks.c) | all six clocks: resolution, cost (vDSO vs syscall), monotonicity, suspend time, NTP slew in ppm, CPU-time clocks | `make run P=linux-internals/15-scheduling-and-real-time/c/04_clocks.c` |
| [c/05_periodic_drift.c](c/05_periodic_drift.c) | relative `nanosleep` loop vs `clock_nanosleep(TIMER_ABSTIME)`: drift, worst lateness, missed releases | `make run P=linux-internals/15-scheduling-and-real-time/c/05_periodic_drift.c` |
| [c/06_relative_sleep_drift_bug.c](c/06_relative_sleep_drift_bug.c) | **BUG**: "1 kHz" loop with `usleep`; its own self-check fails (exit 1) | `make run P=linux-internals/15-scheduling-and-real-time/c/06_relative_sleep_drift_bug.c` |
| [c/07_posix_timers.c](c/07_posix_timers.c) | `timer_create` + `SIGEV_SIGNAL` + `sigtimedwait`, overrun count, one-shot + `timer_gettime`, CPU-time timer | `make run P=linux-internals/15-scheduling-and-real-time/c/07_posix_timers.c` |
| [c/08_cyclictest_lite.c](c/08_cyclictest_lite.c) | wakeup-latency histogram (min/avg/p99/max), SCHED_FIFO fallback, timer slack 50 µs vs 1 ns | `make run P=linux-internals/15-scheduling-and-real-time/c/08_cyclictest_lite.c` |
| [c/09_rt_app_recipe.c](c/09_rt_app_recipe.c) | the full RT recipe with a step report: mallopt, mlockall, prefault, FIFO, pinning, PI mutex to a logger, alloc-free loop, watchdog | `make run P=linux-internals/15-scheduling-and-real-time/c/09_rt_app_recipe.c` |
| [cpp/01_periodic_task.cpp](cpp/01_periodic_task.cpp) | `PeriodicTask` with `std::chrono` absolute deadlines, overrun detection, skip-missed-releases policy, vs `sleep_until` | `make run P=linux-internals/15-scheduling-and-real-time/cpp/01_periodic_task.cpp` |
| [cpp/02_sched_guard.cpp](cpp/02_sched_guard.cpp) | RAII `ScopedSchedPolicy` (throwing ctor + `std::optional` factory), `ScopedAffinity`, `ScopedTimerSlack`, the non-restorable IDLE case | `make run P=linux-internals/15-scheduling-and-real-time/cpp/02_sched_guard.cpp` |

Every non-bug program is also clean under ThreadSanitizer. Add `SAN=tsan`:

```bash
make run P=linux-internals/15-scheduling-and-real-time/c/09_rt_app_recipe.c SAN=tsan
```

To see the RT paths (SCHED_FIFO, SCHED_DEADLINE, `mlockall`) succeed, build
without sanitizers and run as root, ideally on the board:

```bash
make SAN=0 OPT=1 build/linux-internals/15-scheduling-and-real-time/c/09_rt_app_recipe
sudo ./build/linux-internals/15-scheduling-and-real-time/c/09_rt_app_recipe
```

---

## 9. Exercises

**Easy**
1. In `c/02`, try nice 0 vs nice 1, 0 vs 10 and 0 vs 19. Predict the share
   from the weight table first, then measure.
2. Run `c/02` with the two threads *not* pinned. What happens to the ratio,
   and why?
3. In `c/05`, raise the work to 2.5 ms (more than the period). What does the
   absolute loop report as missed releases? What does the relative loop do?
4. Run `ps -eLo pid,tid,cls,rtprio,ni,comm | grep -v ' TS '` on the board.
   Which threads are RT, and at what priority?

**Medium**
5. Fix `c/06` with absolute deadlines and keep its self-check unchanged.
6. Add `SIGEV_THREAD_ID` to `c/07`: deliver the timer signal to one worker
   thread that waits with `sigwaitinfo`, while the main thread keeps the
   signal blocked.
7. Extend `c/08` with a `--load` mode: start N `SCHED_OTHER` threads that
   spin and allocate memory during the measurement. How do min/avg/max
   change for SCHED_OTHER? (On the board, also as SCHED_FIFO.)
8. Add a `SCHED_RR` experiment to `c/02` (needs root): two RR threads of the
   same priority on one CPU. Measure the switch period and compare it with
   `sched_rr_timeslice_ms`.

**Hard**
9. On the board: boot with `isolcpus=1 nohz_full=1 rcu_nocbs=1
   irqaffinity=0`, run `cyclictest -m -p 90 -a 1 -i 1000 -h 200` with
   `stress-ng --cpu 1 --io 1 --vm 1` pinned to CPU 0. Compare the max with
   the default boot.
10. Build a two-thread priority-inversion demo with real `SCHED_FIFO`
    threads pinned to one CPU (needs root), first with a normal mutex, then
    with `PTHREAD_PRIO_INHERIT`. Measure the high thread's lock latency with
    `trace-cmd record -e sched_switch` and read the timeline in
    `kernelshark`.
11. Run a `SCHED_DEADLINE` task (as root) that overruns its runtime on
    purpose, with `SCHED_FLAG_DL_OVERRUN`. Catch `SIGXCPU`, count overruns,
    and watch `/proc/<pid>/sched` (`dl.runtime`, `dl.deadline`).

---

## 10. Remember forever

```
Classes in order: stop > deadline > rt (FIFO/RR 1..99) > fair (OTHER/BATCH/IDLE) > idle
Fair = weights: nice 0 = 1024, x1.25 per step; vruntime += t*1024/w; EEVDF since 6.6
RT thread always beats fair; RT throttling 950/1000 ms; never spin in SCHED_FIFO
Unprivileged: nicer/BATCH/IDLE ok; FIFO/RR need rtprio or CAP_SYS_NICE; DL needs CAP_SYS_NICE
PREEMPT_RT: threaded IRQs, sleeping spinlock_t, PI everywhere -> tens of us worst case
Clocks: MONOTONIC for time intervals/deadlines; REALTIME only for humans (it jumps)
Periodic: next += P; clock_nanosleep(MONOTONIC, TIMER_ABSTIME, &next) -- never sleep(P)
Latency = max(now - next) under load (cyclictest -m -p 90); timer slack 50 us for fair
RT recipe: mallopt + mlockall + prefault + FIFO(EXPLICIT_SCHED) + pin/isolate + PI mutex
           + no malloc/printf/IO/faults in the loop + watchdog
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name the scheduling classes in priority order. Which policies belong to
   each?
2. Two CPU-bound threads share a CPU, one at nice 0 and one at nice 5.
   What share does each get, and where does the number come from?
3. What problem did EEVDF solve that CFS handled with heuristics? What are
   "lag" and "virtual deadline"?
4. What exactly does PREEMPT_RT change about spinlocks, interrupt handlers
   and priority inheritance? Why can a `spinlock_t` section sleep on RT?
5. Which policy changes can a normal user make, and which need
   `RLIMIT_RTPRIO` or `CAP_SYS_NICE`? Why is `SCHED_IDLE` a one-way door?
6. Explain `runtime`, `deadline` and `period` of SCHED_DEADLINE, and what
   admission control and throttling do.
7. What is RT throttling, what are its default values, and what should you
   conclude when `dmesg` says it activated?
8. Why must a periodic loop use absolute sleeps? What happens to the error
   after N cycles with relative sleeps?
9. When do `CLOCK_MONOTONIC`, `CLOCK_MONOTONIC_RAW` and `CLOCK_BOOTTIME`
   differ? Which one do you use for a timeout?
10. What is a timer overrun, and how do you read it?
11. What does cyclictest measure, and why is only the maximum interesting?
    What is timer slack?
12. List the RT recipe steps in order and say for each which latency source
    it removes.
13. What do `isolcpus`, `nohz_full`, `rcu_nocbs` and `irqaffinity` each do?
    Why is isolating a CPU a hard trade-off on a 2-core TDA4VM?
14. Why does `mlockall` fail (or worse) under AddressSanitizer?
