# 04 — Interrupts, Timers and Deferred Work

> A device raises a line; a few hundred nanoseconds later a Cortex-A72 is
> running your handler. This lesson follows that path on the BeagleBone
> AI-64 (device → TI interrupt router → GICv3 → exception vector → genirq →
> your handler), then shows what to do with the event: the **top half**
> (`request_irq`, the handler rules, threaded IRQs and what PREEMPT_RT does
> to them) and the **bottom halves** (softirqs, tasklets, BH workqueues,
> workqueues, kthreads). Then **time**: jiffies and `timer_list`, `hrtimer`
> with hard/soft expiry, `delayed_work`, and a measured table of every
> delay/sleep API. Three modules prove it — one raises a *real* Linux IRQ
> from a simulated device on any PC, one times everything, and one handles
> a real push button on the board's P8 header.

---

## 1. Why this exists

Hardware does not wait for software. A UART FIFO overflows if nobody reads
it, a button press is gone in 100 ms, a motor encoder ticks thousands of
times per second. Polling all of them in a loop wastes the CPU and still
misses events. **Interrupts** let the device tap the CPU on the shoulder
exactly when something happened.

But interrupt context is hostile: other interrupts may be masked, you may
not sleep, every microsecond you spend there is latency for everybody else
(on the board: for the real-time tasks of a PREEMPT_RT kernel). So every
driver splits the work:

- a **top half** that runs immediately, acknowledges the device and grabs
  the data that would otherwise be lost, and
- a **bottom half** that does everything else later, in a context where it
  may take its time (and often may sleep).

Drivers also need **time**: debounce a button for 20 ms, poll a sensor every
100 ms, wait 10 µs after a reset pulse, time out a transfer. Picking the
wrong timer or delay either burns a CPU (`mdelay(100)`), sleeps far longer
than intended (`msleep(1)` is 4–8 ms on the board), or crashes (`msleep()`
in an interrupt handler).

This lesson builds on [03-kernel-synchronization](../03-kernel-synchronization/):
there you learned *which lock* to take against a timer or IRQ handler
(`spin_lock_bh`, `raw_spin_lock_irqsave`); here you learn where those
contexts come from and how to use them.

---

## 2. Mental model

**A doorbell and a to-do list.** The doorbell (IRQ) rings. You (the top
half) run to the door, sign for the parcel so the courier can leave (ack
the device — the line drops), drop it on the hall table, write "unpack
parcel" on the to-do list (queue the bottom half), and go back to what you
were doing. Later, someone with time (a kernel thread) unpacks it. You never
start unpacking at the door: the doorbell might ring again, and the courier
of the *next* parcel cannot wait.

```
  DEVICE           ROUTER (TDA4VM)     GIC-500 (GICv3)        CPU (A72)            LINUX
  -------          ---------------     ---------------        ---------            -----
  GPIO0_62 edge -> main_gpio_intr  ->  SPI 392..447     ->  IRQ exception  ->  gic_handle_irq()
  (bank 3 IRQ)     (ti,sci-intr)       priority, target      vector (EL1)        reads IAR = hwirq
                                       CPU, pending/active                       domain: hwirq -> virq
                                                                                 irq_desc[virq]
                                                                                   ->handle_irq() (flow:
                                                                                     level/edge/fasteoi)
                                                                                   ->action->handler()
                                                                                        |  TOP HALF
                                     IRQ_WAKE_THREAD  /  queue_work / tasklet  <--------+
                                              |                   |
                                              v                   v
                                     irq/NN-name thread   kworker / ksoftirqd / your kthread
                                     (SCHED_FIFO 50)      BOTTOM HALVES (may sleep: threads only)
```

The sentence to remember: **the top half makes the device stop shouting
and records what happened; everything else is somebody else's job, later,
in a thread.**

---

## 3. How it works

### 3.1 From pin to handler on ARM64

**The GIC** (Generic Interrupt Controller; the TDA4VM has a GIC-500, an
implementation of GICv3 — `gic500: interrupt-controller@1800000`,
`compatible = "arm,gic-v3"` in `k3-j721e-main.dtsi`) has four kinds of
interrupt:

| Type | IDs | Meaning | Example on the board |
|---|---|---|---|
| **SGI** — software generated | 0–15 | one CPU interrupts another (IPI) | reschedule, `smp_call_function`, `irq_work` self-IPI |
| **PPI** — private peripheral | 16–31 | one per CPU, same number on every core | the arch timer (drives `hrtimer` + the tick), PMU, GIC maintenance (`GIC_PPI 9` in the DT) |
| **SPI** — shared peripheral | 32–1019 | a device line, routable to any CPU | `main_uart0`: `interrupts = <GIC_SPI 192 IRQ_TYPE_LEVEL_HIGH>` |
| **LPI** — locality-specific | 8192+ | **message**-signalled: a device *writes* to the GIC's **ITS** (`gic_its: msi-controller@1820000`) instead of driving a wire | PCIe MSI/MSI-X |

The GIC holds per-interrupt state (*inactive → pending → active*), a
priority and a target CPU, and signals the CPU. The CPU takes an **IRQ
exception** to its vector table (`arch/arm64/kernel/entry.S`), which calls
the root irqchip driver: `gic_handle_irq()` reads the **IAR** register (the
acknowledged INTID), and from there the **generic IRQ layer (genirq)** takes
over:

```
  hwirq (hardware number, per controller)  --irq_domain-->  virq (Linux IRQ number)
                                                               |
  struct irq_desc  (one per virq: the "IRQ" you request) <-----+
     .irq_data.chip   -> struct irq_chip  : mask/unmask/ack/eoi/set_type/set_affinity
     .handle_irq      -> flow handler     : handle_fasteoi_irq (GIC), handle_level_irq,
                                            handle_edge_irq, handle_simple_irq
     .action          -> list of struct irqaction  (handler, thread_fn, flags, dev_id, name)
```

- **`irq_chip`** = driver of an interrupt *controller* (one per GIC, per
  GPIO block, per router...).
- **`irq_domain`** = the translation table hwirq → virq of one controller.
  Controllers form a **hierarchy**: the virq you get for a GPIO line is in
  the GPIO controller's domain, whose parent is a router, whose parent is
  the GIC. Each level's `irq_chip` does its part.
- **virq** numbers are arbitrary and change between boots and kernels.
  Never hard-code one; get it from DT (`platform_get_irq()`,
  `gpiod_to_irq()`, `of_irq_get()`).

**TI K3 specifics.** On the TDA4VM most peripherals do not connect to the
GIC directly. The **Interrupt Router (INTR, `ti,sci-intr`)** multiplexes
many inputs to a few GIC SPIs, and the **Interrupt Aggregator (INTA,
`ti,sci-inta`)** turns *events* (from DMA rings, etc.) into interrupts. Both
are programmed by the System Firmware through **TI-SCI** messages — Linux
asks the firmware for a route. From the board's DT:

```dts
main_gpio_intr: interrupt-controller@a00000 {     /* k3-j721e-main.dtsi */
        compatible = "ti,sci-intr";               /* drivers/irqchip/irq-ti-sci-intr.c */
        interrupt-controller;
        interrupt-parent = <&gic500>;             /* its parent domain           */
        #interrupt-cells = <1>;
        ti,sci = <&dmsc>;                         /* routes are set up via TI-SCI */
        ti,interrupt-ranges = <8 392 56>;         /* router outputs 8..63 -> GIC SPI 392..447 */
};

main_gpio0: gpio@600000 {
        compatible = "ti,j721e-gpio", "ti,keystone-gpio";   /* drivers/gpio/gpio-davinci.c */
        interrupt-parent = <&main_gpio_intr>;     /* NOT the GIC: the router            */
        interrupts = <256>, <257>, <258>, <259>,  /* one per bank of 16 GPIOs           */
                     <260>, <261>, <262>, <263>;
        interrupt-controller;                     /* ...and is itself a controller      */
        #interrupt-cells = <2>;                   /* <gpio-number trigger-type>         */
};
```

So the push button in §3.7 (GPIO0_62, bank 3) goes: pad → gpio-davinci
(demultiplexes bank 3's IRQ to line 62 in its own domain) → router input
259 → some GIC SPI in 392..447 → CPU. Three `irq_chip`s, one `request_irq()`.

**Seeing it** — on any Linux machine:

```sh
cat /proc/interrupts            # per-CPU counts | chip | hwirq-flow | action name(s)
#            CPU0       CPU1
#  11:     184213     183977     GICv3  27 Level     arch_timer        <- PPI 11 = INTID 27
#  25:        418          0     GICv3 224 Level     2800000.serial    <- SPI 192 = INTID 224
#  ...
cat /proc/irq/25/smp_affinity       # bitmask of CPUs allowed to take IRQ 25 (hex)
echo 2 | sudo tee /proc/irq/25/smp_affinity   # move it to CPU1 (GIC SPIs can be routed)
cat /proc/irq/25/smp_affinity_list  # same as a list: "1"
ls /sys/kernel/irq/25/              # chip_name, hwirq, type, actions, per_cpu_count
```

PPIs and SGIs are per-CPU and cannot be moved. `irqbalance` (a daemon)
rewrites `smp_affinity` periodically; on an RT system you usually disable
it and pin IRQs by hand, away from the CPU running your RT task.

### 3.2 `request_irq()` and the rules of the handler

```c
#include <linux/interrupt.h>

typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

int  request_irq(unsigned int irq, irq_handler_t handler, unsigned long flags,
                 const char *name, void *dev_id);
void free_irq(unsigned int irq, void *dev_id);           /* waits for running handlers */
int  devm_request_irq(struct device *dev, ...same...);   /* free_irq() at unbind */
```

| Flag | Meaning |
|---|---|
| `IRQF_SHARED` | several devices on one line (legacy PCI INTx, some SoC lines). `dev_id` **must** be unique and non-NULL: `free_irq()` uses it to find *your* action. Every handler on the line is called; each must check its own device |
| `IRQF_ONESHOT` | keep the line **masked** until the threaded handler returns (§3.3). Required when `handler == NULL` |
| `IRQF_TRIGGER_RISING/FALLING/HIGH/LOW` | override the trigger type (normally it comes from DT `interrupts = <... IRQ_TYPE_...>`) |
| `IRQF_NO_THREAD` | never force-thread this handler, even on PREEMPT_RT (timer, IPI, cascaded irqchip handlers). The handler must then only use `raw_spinlock_t` |
| `IRQF_NO_SUSPEND`, `IRQF_EARLY_RESUME` | keep it enabled across system suspend (wake-up sources use `enable_irq_wake()` instead) |
| `IRQF_PERCPU`, `request_percpu_irq()` | PPIs: one handler instance per CPU |

**Handler rules** (top half):

1. **Never sleep**: no `mutex_lock`, `msleep`, `kmalloc(GFP_KERNEL)`,
   `copy_to_user`, `wait_event`. (On PREEMPT_RT the handler is a thread,
   but write it as if it were not — the same driver runs on non-RT kernels.)
2. **Be short**: microseconds. Read status, ack the device, copy the
   minimum, kick a bottom half.
3. **Ack the device** before returning, or a *level*-triggered line is
   still asserted and you are called again immediately — forever.
4. **Return the truth**: `IRQ_NONE` if your device did not interrupt (shared
   line, spurious), `IRQ_HANDLED` if it did, `IRQ_WAKE_THREAD` to run the
   threaded handler. If a line returns `IRQ_NONE` 99 900 times out of
   100 000, genirq's spurious detector prints *"irq NN: nobody cared"* and
   **disables the line**. Counts are in `/proc/irq/N/spurious`.
5. A given IRQ's handler never runs on two CPUs at once (genirq serialises
   it); different IRQs and all other code *do* run concurrently — protect
   shared data as in [03 §3.3 "Spinlocks"](../03-kernel-synchronization/).

Control from outside the handler: `disable_irq(irq)` (masks and **waits**
for a running handler — never call it from that handler: deadlock),
`disable_irq_nosync()` (masks, does not wait), `enable_irq()` (nests:
two disables need two enables), `synchronize_irq()` (wait for running
handlers, including the thread). `free_irq()` = remove the action + wait.

### 3.3 Threaded IRQs, `IRQF_ONESHOT` and PREEMPT_RT

```c
int request_threaded_irq(unsigned int irq, irq_handler_t handler,
                         irq_handler_t thread_fn, unsigned long flags,
                         const char *name, void *dev_id);
int devm_request_threaded_irq(struct device *dev, ...same...);
```

The hard `handler` runs in interrupt context and returns `IRQ_WAKE_THREAD`;
genirq then wakes a per-IRQ kernel thread **`irq/<N>-<name>`**
(`SCHED_FIFO` priority 50) which calls `thread_fn` in **process context —
it may sleep**: talk to an I2C/SPI device, take a mutex, allocate with
`GFP_KERNEL`. `handler == NULL` installs a default that just wakes the
thread; then `IRQF_ONESHOT` is mandatory.

```
 line  ___|‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾‾|____     level IRQ, device holds it
           ^ hard handler: mask line (ONESHOT), ack?, IRQ_WAKE_THREAD
             ^ thread runs: read the device over I2C (sleeps!), device drops line
                                                    ^ thread returns -> genirq unmasks
 Without ONESHOT the line is unmasked as soon as the hard handler returns and,
 still asserted, fires again at once: an interrupt storm until the thread runs.
```

**PREEMPT_RT (the board's kernel) threads everything.** With
`CONFIG_PREEMPT_RT=y` (or `threadirqs` on the command line of a normal
kernel), genirq **force-threads** every handler not marked
`IRQF_NO_THREAD`: the real hard IRQ only wakes `irq/<N>-<name>`, which
then runs your "hard" handler (with BHs disabled), and your `thread_fn`
runs in a second thread `irq/<N>-s-<name>`. Consequences:

- Your interrupt handlers are now preemptible and schedulable: give an
  important IRQ a higher priority with `chrt -f -p 80 <pid of irq/N-...>`,
  and an RT task can preempt a less important handler. That is what makes
  RT latency predictable.
- `spinlock_t` becomes a sleeping lock — fine in a threaded handler. Code
  that runs in *real* hard-IRQ context even on RT (`IRQF_NO_THREAD`
  handlers, `hrtimer` `_HARD` callbacks, `irq_work` hard) must use
  `raw_spinlock_t` ([03 §3.3](../03-kernel-synchronization/)).
- The `irqsim_demo` self-test prints which world it ran in:
  `in_hardirq=1` (PC) or `in_task=1 (force-threaded)` (board).

### 3.4 Bottom halves: softirqs, tasklets, BH workqueues, workqueues, kthreads

| Mechanism | Context | May sleep | Runs on | Since / status | Use for |
|---|---|---|---|---|---|
| **threaded IRQ** | kthread `irq/N-name`, SCHED_FIFO 50 | yes | any CPU (follows IRQ affinity) | 2.6.30 | the default bottom half of a device IRQ |
| **softirq** | softirq (on IRQ exit, or `ksoftirqd/N`) | no | the CPU that raised it | fixed list, core code only | networking, block, timers, RCU |
| **tasklet** | `TASKLET_SOFTIRQ` / `HI_SOFTIRQ` | no | the CPU that scheduled it; never two CPUs at once | **being phased out** | legacy drivers |
| **BH workqueue** (`system_bh_wq`, `WQ_BH`) | softirq | no | the queuing CPU | 6.9+, the tasklet replacement | short atomic work that must run soon |
| **workqueue** (`schedule_work`, `queue_work`) | kthread `kworker/...` | yes | per-CPU pools or unbound | the general-purpose answer | "do this later in process context" |
| **kthread** / **kthread_worker** | your own thread | yes | where you pin it | — | long loops, your own RT priority |

**Softirqs** are a fixed, compile-time list (`include/linux/interrupt.h`):
`HI`, `TIMER`, `NET_TX`, `NET_RX`, `BLOCK`, `IRQ_POLL`, `TASKLET`, `SCHED`,
`HRTIMER`, `RCU` — drivers cannot add one. They run when a hard IRQ
returns, at `local_bh_enable()`, or in the per-CPU `ksoftirqd/N` thread
when they keep re-raising themselves. On PREEMPT_RT they always run in
thread context (and timer softirqs in dedicated `ktimers/N` threads on
recent RT kernels). `in_serving_softirq()` tells you that you are in one.

**Tasklets** are a softirq that runs a driver callback. They are on their
way out (new code should not add them): they run in atomic context with
no priority control, and on RT they complicate things. The replacement is
a **BH workqueue** (6.9+): `queue_work(system_bh_wq, &w)` — same softirq
context, normal workqueue API. Old API, still there in 6.18:

```c
tasklet_setup(&t, fn);          /* void fn(struct tasklet_struct *t); from_tasklet() to get your struct */
tasklet_schedule(&t);           /* IRQ-safe; no-op if already scheduled */
tasklet_kill(&t);               /* wait + make sure it is not scheduled (process context) */
```

**Workqueues** are a pool of kernel threads executing `work_struct` items:

```c
INIT_WORK(&w, fn);                      /* void fn(struct work_struct *w); container_of() for your struct */
schedule_work(&w);                      /* = queue_work(system_wq, &w); IRQ-safe; false if already pending */
queue_work(my_wq, &w);
flush_work(&w);                         /* wait until it has run */
cancel_work_sync(&w);                   /* dequeue, or wait if running; safe vs self-requeue */

INIT_DELAYED_WORK(&dw, fn);             /* a timer that queues the work on expiry */
schedule_delayed_work(&dw, msecs_to_jiffies(100));
mod_delayed_work(system_wq, &dw, delay);/* (re)start the timer: perfect for debounce */
cancel_delayed_work_sync(&dw);

my_wq = alloc_workqueue("name", flags, max_active);   /* 0 = default max_active */
destroy_workqueue(my_wq);               /* drains, then frees */
```

| `alloc_workqueue()` flag | Effect |
|---|---|
| (none) | per-CPU: the item runs on the CPU that queued it (cache-friendly) |
| `WQ_UNBOUND` | any CPU, chosen by the scheduler — long or CPU-hungry items, power saving |
| `WQ_HIGHPRI` | workers at nice −20 |
| `WQ_MEM_RECLAIM` | adds a **rescuer** thread so the queue makes progress under memory pressure — mandatory for anything on the block I/O / reclaim path |
| `WQ_CPU_INTENSIVE` | long-running items do not block other items on that CPU |
| `WQ_FREEZABLE` | frozen during system suspend |
| `WQ_BH` (6.9+) | execute in softirq context |
| `WQ_PERCPU` (6.17+) | explicit per-CPU; the kernel is moving to `system_percpu_wq` / `system_dfl_wq` (unbound) and will retire `system_wq` |

A work item is **non-reentrant**: it never runs on two CPUs at once, and
queueing it while still pending does nothing. So a bottom half must handle
*all* pending events, never assume one run per interrupt. `irqsim_demo`
shows the coalescing (`runs <= IRQs`).

**kthreads** — your own thread, your own loop:

```c
struct task_struct *t = kthread_run(fn, data, "mydrv/%d", id);   /* int fn(void *data) */
/* in fn: */  while (!kthread_should_stop()) { ...wait for work... }
kthread_stop(t);                        /* sets the flag, wakes it, waits for fn to return */
```

**kthread_worker** = one dedicated thread executing queued items (like a
private single-thread workqueue whose thread you can give an RT priority):
`kthread_run_worker(0, "name")` (6.14+; before: `kthread_create_worker()`),
`kthread_init_work()`, `kthread_queue_work()` (IRQ-safe),
`kthread_flush_worker()`, `kthread_cancel_work_sync()`,
`kthread_destroy_worker()`. SPI uses it for its message pump.

**`irqsim_demo` — a real IRQ on any machine.** The PC has no device we may
safely make interrupt, so the module simulates one, and *only* the device:

```
  hrtimer (HARD, every period_us)          your driver (real genirq API)
  = "the device"                           ---------------------------------------------
  STATUS += 1, TIMESTAMP = now   --line--> irqsim_hard()   ack: xchg(STATUS, 0)
  if line not masked:                        |  latency = now - TIMESTAMP
     generic_handle_irq_safe(virq)           |  queue_work / kthread_queue_work /
        (irq_chip "esh-swirq",               |  tasklet_schedule / queue_work(system_bh_wq)
         handle_level_irq, domain            +-> IRQ_WAKE_THREAD -> irqsim_thread()
         "irqsim_demo", hwirq 0)                 each bottom half records its latency
```

`irq_domain_create_linear()` + an `irq_chip` with `irq_mask`/`irq_unmask`
+ `irq_create_mapping()` give a genuine virq (this is how the kernel's own
`irq_sim` and `gpio-sim` work). From `request_threaded_irq()` on, nothing is
simulated: the line shows in `/proc/interrupts`, ONESHOT masks it (the
`while_masked` count), PREEMPT_RT force-threads it. `thread_us=` makes the
threaded handler slow so you can watch events coalesce. The debugfs files
`/sys/kernel/debug/irqsim_demo/stats` and `.../trigger` (write anything =
one event) let you poke it after loading. `user/irqsim_watch.c` reads it
all from user space.

### 3.5 Timers

**jiffies and HZ.** The kernel tick fires `HZ` times per second
(`CONFIG_HZ`: **250** on the board's config → 4 ms per jiffy; 1000 on
Ubuntu's x86 kernel). `jiffies` counts ticks since boot (wraps: compare with
`time_after(a, b)`, `time_before()`, never `a > b`). Convert with
`msecs_to_jiffies()` (rounds **up**), `jiffies_to_msecs()`, `HZ`.

**`timer_list`** — a callback at a jiffy, in softirq context (the timer
wheel: O(1) add/remove, slack grows for far-away timers):

```c
timer_setup(&t, fn, 0);                 /* void fn(struct timer_list *t) */
mod_timer(&t, jiffies + msecs_to_jiffies(10));   /* (re)arm; returns 1 if it was pending */
/* in fn: struct my *m = timer_container_of(m, t, timer);   (from_timer() before 6.16) */
/*        mod_timer(&m->timer, m->timer.expires + period);  fixed grid, no drift */
timer_delete_sync(&t);                  /* old name del_timer_sync(), gone in 6.15+ */
timer_shutdown_sync(&t);                /* 6.2+: delete AND forbid re-arming — use it in teardown
                                           of self-re-arming timers */
```

**`hrtimer`** — nanosecond resolution, red-black tree per CPU, programs the
hardware timer (the A72 arch timer PPI) for the exact expiry:

```c
hrtimer_setup(&h, fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);  /* 6.13+; before:
                                     hrtimer_init(&h, clock, mode); h.function = fn; */
hrtimer_start(&h, ns_to_ktime(500 * NSEC_PER_USEC), HRTIMER_MODE_REL_HARD);
/* fn returns HRTIMER_NORESTART or, after hrtimer_forward_now(h, period), HRTIMER_RESTART */
hrtimer_cancel(&h);                     /* waits for a running callback */
```

| Mode bit | Meaning |
|---|---|
| `_REL` / `_ABS` | expiry relative to now / an absolute time of the clock |
| `_PINNED` | stay on this CPU |
| `_SOFT` | callback in `HRTIMER_SOFTIRQ` (softirq context) |
| `_HARD` | callback in hard-IRQ context **even on PREEMPT_RT** — must use raw spinlocks |
| none | hard on a normal kernel, **soft on PREEMPT_RT** |

The hard/soft choice is made at setup and must match at start. Clocks:
`CLOCK_MONOTONIC` (the default choice), `CLOCK_BOOTTIME` (counts suspend),
`CLOCK_REALTIME` (wall clock, jumps), `CLOCK_TAI`.

**Drift.** Re-arming a periodic hrtimer as "now + period" adds every
callback's lateness to all later expiries; `hrtimer_forward()` /
`hrtimer_forward_now()` advances on the fixed grid `start + k·period` and
returns how many periods it skipped (overruns). `timers_demo` runs both
side by side and extrapolates the drift to an hour.

**ktime API** (`<linux/ktime.h>`): `ktime_t` is s64 nanoseconds.
`ktime_get()` (monotonic), `ktime_get_ns()`, `ktime_get_boottime()`,
`ktime_get_real()`, `ktime_add/sub()`, `ktime_to_us()`, `ns_to_ktime()`,
`ms_to_ktime()`, `ktime_after()`. Use these for measuring, not jiffies.

### 3.6 Delays and sleeps

| Call | Mechanism | Allowed in atomic context (IRQ, softirq, spinlock held)? | Use for |
|---|---|---|---|
| `ndelay(ns)` | busy-wait | yes | a few ns–µs between register writes |
| `udelay(us)` | busy-wait | yes | < ~10 µs (hardware setup/hold times) |
| `mdelay(ms)` | busy-wait | yes, but **avoid** — burns the CPU, kills RT latency | almost never; early boot only |
| `usleep_range(min, max)` | hrtimer sleep | **no** | 10 µs – 20 ms; the range lets the kernel merge wake-ups |
| `msleep(ms)` | jiffies sleep | **no** | ≥ 20 ms; rounds up to whole jiffies (+ wheel slack) |
| `msleep_interruptible(ms)` | jiffies sleep, signals wake it | **no** | waits a user may abort |
| `fsleep(us)` | picks one of the above | only if `us` ≤ 10 (then it is `udelay`) | "sleep about this long", let the kernel choose |
| `schedule_timeout(j)` / `schedule_hrtimeout()` | low-level | **no** | building your own wait loops |

The rule: **if you may sleep, sleep; busy-wait only when you must** (atomic
context) **and only for microseconds**. Sleeps never return early; they
can return much later (scheduling, slack). `timers_demo` prints the real
numbers — on the board, `msleep(1)` is a full jiffy (4 ms) or more, while
`usleep_range(1000,1100)` is ~1.05 ms.

### 3.7 A real GPIO interrupt on the board (`gpio_button_irq.c` + overlay)

Wiring (3.3 V logic only — never 5 V on a header pin):

```
  P8 header (BeagleBone AI-64)
   P8_1/P8_2 GND  o-----------+
                              |
                            [ / ]  push button (normally open)
                              |
   P8_16 GPIO0_62 o-----------+       internal pull-up -> idle 1, pressed 0
```

**Why P8_16?** It is GPIO0_62 on ball AB28, pad register `0xfc` of
`main_pmx0`, mux mode 7 = GPIO (from the `gpio-line-names` in
`k3-j721e-beagleboneai64-pinmux.dtsi` and the upstream BBORG_MOTOR overlay),
and nothing in the base DT uses it. **Check the pinmux**: if another overlay
you load claims P8_16 (BBORG_MOTOR does), choose another pin and change the
pad offset *and* the GPIO number together.

The overlay [kernel/gpio-button-overlay.dtso](kernel/gpio-button-overlay.dtso)
adds a pinmux group (input, pull-up, mode 7) and a node:

```dts
esh_button: esh-button {
        compatible = "esh,gpio-button";       /* matches btn_of_match[] in the driver */
        pinctrl-names = "default";
        pinctrl-0 = <&esh_button_pins>;       /* applied by the driver core before probe */
        button-gpios = <&main_gpio0 62 GPIO_ACTIVE_LOW>;   /* devm_gpiod_get(dev, "button", ...) */
        debounce-ms = <20>;                   /* device_property_read_u32() */
};
```

The driver: `devm_gpiod_get()` → `gpiod_set_debounce()` (gpio-davinci has
no hardware debounce, so software it is) → `gpiod_to_irq()` →
`devm_add_action_or_reset(cancel the work)` → `devm_request_threaded_irq(...,
IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT)`. Hard handler:
count the raw edge (bounces included), return `IRQ_WAKE_THREAD`. Thread:
`mod_delayed_work()` restarts the 20 ms debounce timer on every edge. Work:
read the settled level and count a press or release, `sysfs_notify()`.
Unbind releases in reverse order: IRQ freed first, then the work cancelled,
then the memory — which is why the cancel action is registered *before*
the IRQ. How DT matching and overlays work in general is lesson
[06-device-model-and-device-tree](../06-device-model-and-device-tree/);
GPIO, I2C and SPI consumers in depth are [07](../07-gpio-i2c-spi-drivers/).

Build the overlay (needs the board kernel *source* tree for
`<dt-bindings/gpio/gpio.h>` and TI's `k3-pinctrl.h`):

```sh
BB=~/beaglebone-ai64/sources/arm64-multiplatform          # adjust the path
make -C linux-device-drivers/04-interrupts-timers-and-deferred-work/kernel dtbo KDIR=$BB/KERNEL
#  = cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $KDIR/include \
#        -I $KDIR/arch/arm64/boot/dts/ti gpio-button-overlay.dtso | dtc -@ -I dts -O dtb -o ...dtbo -
# Optional offline check: apply it to the board's base DTB on the PC
fdtoverlay -i $BB/KERNEL/arch/arm64/boot/dts/ti/k3-j721e-beagleboneai64.dtb \
           -o /tmp/test.dtb kernel/gpio-button-overlay.dtbo && echo applies cleanly
```

Install and load it on the board (the overlay is applied by U-Boot at
boot). BeagleBoard.org Debian images boot through
`/boot/firmware/extlinux/extlinux.conf` — check yours first (the
beaglebone-ai64 workspace's `deploy-and-test` and `kernel-dev` skills
describe its boot files and kernel install):

```sh
scp kernel/gpio-button-overlay.dtbo kernel/gpio_button_irq.ko debian@<board-ip>:
# on the board:
sudo cp gpio-button-overlay.dtbo /boot/firmware/overlays/
sudoedit /boot/firmware/extlinux/extlinux.conf
#   in your label, add (or extend) the line:
#       fdtoverlays /overlays/gpio-button-overlay.dtbo
sudo reboot
ls /proc/device-tree/esh-button              # node is in the live DT
sudo insmod gpio_button_irq.ko               # (or modprobe after installing: modalias autoload)
#   press the button a few times
dmesg | grep esh-button
grep esh-button /proc/interrupts
cat /sys/bus/platform/devices/esh-button/{presses,releases,raw_edges,state}
echo 50 | sudo tee /sys/bus/platform/devices/esh-button/debounce_ms
sudo rmmod gpio_button_irq
```

Manually at the U-Boot prompt instead (load addresses from the
beaglebone-ai64 workspace's `docs/build/TFTP-BOOT.md`):

```
=> load mmc 1:1 ${fdt_addr_r} k3-j721e-beagleboneai64.dtb
=> fdt addr ${fdt_addr_r}
=> fdt resize 8192
=> load mmc 1:1 ${fdtoverlay_addr_r} overlays/gpio-button-overlay.dtbo
=> fdt apply ${fdtoverlay_addr_r}
```

Expected (illustrative — this module is build-tested only):

```
esh-gpio-button esh-button: hardware debounce not supported, using 20 ms software debounce
esh-gpio-button esh-button: ready: irq 87, level now 0 (1 = pressed)
esh-gpio-button esh-button: pressed (presses 1, releases 0, raw edges 3)
esh-gpio-button esh-button: released (presses 1, releases 1, raw edges 7)
```

Raw edges > 2 per press is contact bounce — exactly why debouncing exists.

### 3.8 Measuring latency

- **In the driver**: `ktime_get_ns()` in the handler minus a timestamp of
  the event (a hardware timestamp register if the device has one; here the
  simulated device has one). `irqsim_demo` prints min/avg/max per path.
- **System-wide**: `cyclictest -m -S -p 90 -i 1000` (rt-tests) measures
  timer-wake-up latency per CPU — the standard RT benchmark. On the board
  run it while `irqsim_demo keep_running=1 period_us=100` hammers a CPU.
- **Tracing**: `trace-cmd record -e irq:irq_handler_entry -e irq:irq_handler_exit
  -e irq:softirq_entry -e workqueue:workqueue_execute_start` then
  `trace-cmd report`; ftrace's `irqsoff` and `preemptoff` tracers find who
  disabled interrupts the longest. See [09-kernel-debugging-and-tracing](../09-kernel-debugging-and-tracing/).
- **From user space**: `/proc/interrupts` deltas (`user/irqsim_watch.c`),
  `/proc/irq/N/spurious`, `/proc/softirqs`, `ps -eLo pid,cls,rtprio,comm | grep irq/`.

### 3.9 Which mechanism do I use?

| I need to... | Use |
|---|---|
| react to a device interrupt, and the follow-up may sleep (I2C/SPI device, mutex) | `request_threaded_irq()` (handler may be NULL + `IRQF_ONESHOT`) |
| react to a device interrupt, follow-up is tiny and atomic | `request_irq()`, do it in the handler |
| run something later in process context, from anywhere | `schedule_work()` / `queue_work()` |
| ...after a delay, or restartable (debounce, poll) | `delayed_work` + `mod_delayed_work()` |
| a long-running loop / my own RT priority | `kthread_run()` or `kthread_worker` |
| short atomic deferred work (instead of a tasklet) | BH workqueue (6.9+), else tasklet |
| a callback at ms granularity, cheap, many of them (timeouts) | `timer_list` |
| precise (µs) or periodic timing | `hrtimer` (`_HARD` only if you really need hard-IRQ context on RT) |
| wait inside process context | `usleep_range()` / `msleep()` / `fsleep()` |
| wait in atomic context | `udelay()` / `ndelay()`, a few µs at most — or restructure |

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| IRQ entry → your handler | ~0.5–2 µs on an A72 (non-RT), more when force-threaded (a context switch) | exception entry, GIC IAR read, domain lookup, flow handler, `irq_desc` lock |
| hwirq → virq lookup | O(1) linear domain, O(log n) radix/tree domain | `irq_domain_create_linear` is an array |
| `request_irq()` / `free_irq()` | O(actions on the line); `free_irq` may sleep (waits for handlers) | process context only |
| `queue_work()`, `tasklet_schedule()`, `kthread_queue_work()` | O(1), IRQ-safe | test-and-set a PENDING bit, add to a list, wake a worker |
| `mod_timer()` / `timer_delete()` | O(1) | hashed timer wheel; precision traded for speed (coarser buckets far away) |
| `hrtimer_start()` / `hrtimer_cancel()` | O(log n) | per-CPU red-black tree; may reprogram the clock-event device |
| `*_sync()` teardown calls | unbounded (waits for a running callback) | must be in process context |
| `udelay(n)` | n µs of one CPU at 100% | busy loop on the arch counter |
| `usleep_range(a, b)` | ~2 context switches + an hrtimer | the CPU is free meanwhile |

---

## 5. C vs C++ — and kernel vs user space

The kernel is C only (C11 with GNU extensions; Rust exists for some
drivers, not C++). The concepts map to user space like this:

| Kernel | User-space equivalent | See |
|---|---|---|
| hard IRQ handler (async, restricted) | a **signal handler** (async-signal-safe functions only) | [13-signals-eventfd-and-epoll](../../linux-internals/13-signals-eventfd-and-epoll/) |
| threaded IRQ / "signal → wake a thread" | `signalfd` or self-pipe + a thread | same |
| workqueue | a thread pool (`std::async`, Boost.Asio `post`) | [08-barriers-once-and-thread-pools](../../linux-internals/08-barriers-once-and-thread-pools/) |
| `kthread_run` / `kthread_stop` | `pthread_create` / `std::jthread` + `stop_token` | [01-processes-and-threads](../../linux-internals/01-processes-and-threads/) |
| `hrtimer`, `timer_list` | `timerfd_create`, `timer_create` | [13](../../linux-internals/13-signals-eventfd-and-epoll/) |
| `usleep_range`, `msleep` | `clock_nanosleep`, `std::this_thread::sleep_for` | — |
| `udelay` | a busy loop on `clock_gettime` | — |
| irq thread `SCHED_FIFO 50`, `chrt` | `sched_setscheduler(SCHED_FIFO)` | [15-scheduling-and-real-time](../../linux-internals/15-scheduling-and-real-time/) |
| `ktime_get()` | `clock_gettime(CLOCK_MONOTONIC)`, `std::chrono::steady_clock` | — |

A user-space program *can* receive a device interrupt: UIO
(`/dev/uioN`, `read()` blocks until an IRQ) and VFIO do exactly that, and
`libgpiod`'s `gpiod_line_request_read_edge_events()` delivers GPIO edges
(the kernel's handler queues them for you).

---

## 6. Where it is used in the real world

- **`drivers/irqchip/irq-gic-v3.c`** (+ `irq-gic-v3-its.c`): the board's
  root interrupt controller. **`irq-ti-sci-intr.c` / `irq-ti-sci-inta.c`**:
  the TDA4VM's routers, hierarchical domains talking TI-SCI.
- **`drivers/gpio/gpio-davinci.c`**: the TDA4VM's GPIO controller — an
  `irq_chip` with chained bank handlers, the parent of the button's IRQ.
- **`drivers/input/keyboard/gpio_keys.c`**: the production version of
  `gpio_button_irq` (the board's BOOT and POWER buttons use it — see the
  `gpio-keys` node in `k3-j721e-beagleboneai64.dts`), with debounce via
  `delayed_work` or hardware.
- **`drivers/i2c/busses/i2c-omap.c`** (the board's I2C):
  `devm_request_threaded_irq()` with a hard handler that masks and a thread
  that moves bytes. Most I2C client drivers (touchscreens, PMIC interrupts)
  use `handler = NULL` + `IRQF_ONESHOT`, because reading the device sleeps.
- **NAPI networking**: the NIC IRQ handler disables its IRQ and schedules
  `NET_RX_SOFTIRQ` polling — the classic softirq bottom half.
- **`leds-gpio` heartbeat trigger**: a `timer_list` (the board's USR0 LED).
- **`kernel/irq/irq_sim.c`**, **`drivers/gpio/gpio-sim.c`**: the kernel's
  own interrupt simulator — the idea behind `irqsim_demo`.
- **rt-tests `cyclictest`**: measures exactly the hrtimer wake-up latency
  `timers_demo` shows.
- **SPI core**: a `kthread_worker` message pump with optional RT priority.

---

## 7. Common mistakes and pitfalls

1. **Sleeping in the hard handler** (`mutex_lock`, `msleep`, `i2c_transfer`,
   `kmalloc(GFP_KERNEL)`): "BUG: scheduling while atomic". Move it to the
   threaded handler. Works on the RT board by accident, explodes on a PC.
2. **Not acking a level-triggered device** → the handler runs forever
   (storm), or genirq disables the line ("nobody cared").
3. **Threaded IRQ without `IRQF_ONESHOT`** on a level line → storm between
   hard handler and thread. With `handler == NULL` request_irq refuses (-EINVAL).
4. **`IRQ_HANDLED` from a shared-line handler that did not check its
   device** → the real owner's interrupts look handled; spurious detection
   breaks.
5. **`dev_id == NULL` with `IRQF_SHARED`** → `request_irq` fails; and
   `free_irq()` with a different `dev_id` than requested frees nothing (WARN).
6. **`disable_irq()` inside its own handler** → deadlock (it waits for the
   handler to finish). Use `disable_irq_nosync()`.
7. **Teardown in the wrong order**: freeing the data or the workqueue while
   the IRQ can still fire and queue work → use-after-free. Order: stop the
   device → `free_irq()` → `cancel_work_sync()`/`tasklet_kill()`/`hrtimer_cancel()`
   → free. With devm, register cleanup actions *before* the IRQ.
8. **Self-re-arming timer killed with `timer_delete_sync()`** — it can
   re-arm between the delete and the wait. Use `timer_shutdown_sync()`.
9. **Assuming one bottom-half run per IRQ.** Work items, tasklets and
   threads coalesce. Drain *all* pending events each run.
10. **`msleep(1)` for "1 ms"**: 4–8 ms on HZ=250. Use `usleep_range()`.
    **`udelay(5000)`** in a driver: 5 ms of a CPU wasted, latency spike for
    every RT task. **`jiffies > timeout`**: wrong at wrap-around; use `time_after()`.
11. **`spinlock_t` in an `IRQF_NO_THREAD` handler or a `_HARD` hrtimer on
    RT** → "sleeping function called from invalid context". Use
    `raw_spinlock_t` there.
12. **Hard-coding an IRQ number** — virqs change. Get them from DT/ACPI.
13. **Forgetting `tasklet_kill()` / `cancel_work_sync()` in module exit** —
    the callback runs after the module text is freed: oops at an address in
    no module.
14. **Re-using a periodic hrtimer with "now + period"** → drift. Use
    `hrtimer_forward_now()`.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile: `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `W=1`, `clean`, plus `make dtbo KDIR=<board tree>` for the overlay | `make drivers` |
| [kernel/irqsim_demo.c](kernel/irqsim_demo.c) | **real virq** from a simulated device: `irq_domain_create_linear` + `irq_chip` (mask/unmask) + `handle_level_irq`, hrtimer `_HARD` as the device, `request_threaded_irq` + `IRQF_ONESHOT`, `IRQ_NONE`/`IRQ_WAKE_THREAD`, five bottom halves (irq thread, `alloc_workqueue`, `kthread_worker`, tasklet, BH workqueue) with latency min/avg/max and coalescing, debugfs `stats`/`trigger`, correct teardown order | `insmod irqsim_demo.ko [period_us=1000 duration_ms=1000 thread_us=0 keep_running=0]` |
| [kernel/timers_demo.c](kernel/timers_demo.c) | `timer_list` on a jiffy grid (`timer_container_of`, `timer_shutdown_sync`), three hrtimers (forward HARD, naive HARD → drift, forward SOFT) with lateness tables, `delayed_work`, and the measured delay table (`ndelay`…`fsleep`) | `insmod timers_demo.ko [timer_ms=10 hr_us=1000 duration_ms=1000]` |
| [kernel/gpio_button_irq.c](kernel/gpio_button_irq.c) | **board**: platform driver for `esh,gpio-button` — `devm_gpiod_get`, `gpiod_to_irq`, `devm_request_threaded_irq` both edges + ONESHOT, debounce with `mod_delayed_work`, devm ordering, sysfs counters via `dev_groups` | board only: §3.7 |
| [kernel/gpio-button-overlay.dtso](kernel/gpio-button-overlay.dtso) | **board**: DT overlay — P8_16 pinmux (pad `0xfc`, pull-up, mode 7) + the `esh-button` node | `make -C …/kernel dtbo KDIR=<board tree>` |
| [user/irqsim_watch.c](user/irqsim_watch.c) | user-space view of `irqsim_demo`'s IRQ: `/proc/interrupts` line and rate, `/proc/irq/N/{smp_affinity_list,spurious}`, the `irq/N-irqsim_demo` thread's `SCHED_FIFO` priority, debugfs stats | `make drivers`, then `sudo ./build/linux-device-drivers/04-interrupts-timers-and-deferred-work/user/irqsim_watch` |

**Build** (from the repo root):

```sh
make drivers                                   # host: modules + user/ programs
make -C linux-device-drivers/04-interrupts-timers-and-deferred-work/kernel W=1      # extra warnings
make drivers KDIR=<board kernel tree> ARCH=arm64 CROSS_COMPILE=<its toolchain prefix>   # board
                                               # (see ../README.md#building for the exact paths)
make -C linux-device-drivers/04-interrupts-timers-and-deferred-work/kernel dtbo KDIR=<board kernel tree>
make drivers-clean                             # remove *.ko, *.o, .*.cmd, *.dtbo …
```

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1`; checkpatch-clean apart from the
file-name-in-header note this repo's conventions require and the
`LINUX_VERSION_CODE` notes of the 6.8/6.18 compatibility guards) and
cross-built against the board's `6.18.x-arm64-k3` PREEMPT_RT tree with its
own GCC 15. The overlay compiles with `dtc -@` and applies cleanly to the
board's base DTB with `fdtoverlay`. Loading the modules is **your** step,
on the board or in a VM — see [../README.md](../README.md#safety).
`irqsim_demo` and `timers_demo` need no hardware: a QEMU VM is enough.

**Run** (board or VM):

```sh
sudo insmod irqsim_demo.ko
sudo dmesg | grep irqsim_demo
sudo rmmod irqsim_demo

sudo insmod irqsim_demo.ko keep_running=1 period_us=1000
sudo ./irqsim_watch                    # rate, irq thread priority, stats
echo 1 | sudo tee /sys/kernel/debug/irqsim_demo/trigger
sudo rmmod irqsim_demo

sudo insmod timers_demo.ko && sudo dmesg | grep timers_demo && sudo rmmod timers_demo
```

Expected output on the board (illustrative: the modules are build-tested
only, and numbers vary from run to run and machine to machine):

```
irqsim_demo: PASS request_irq  virq 112 = hwirq 0 of domain irqsim_demo, chip esh-swirq, IRQF_ONESHOT
irqsim_demo: start: device period 1000 us for 1000 ms, thread_us=0, HZ=250, PREEMPT_RT
irqsim_demo: PASS accounting   device raised 1000 events, hard handler acked 1000, 0 still pending
irqsim_demo: PASS hard-irq     1000 runs, in_hardirq=0 in_task=1 (force-threaded: PREEMPT_RT or threadirqs)
irqsim_demo: INFO 0 ticks found the line masked (ONESHOT thread still running) -> coalesced
irqsim_demo: PASS irq-thread   1000 runs (<= 1000 IRQs: coalesced 0), in_task=1
irqsim_demo: PASS workqueue    998 runs (<= 1000 IRQs: coalesced 2), in_task=1
irqsim_demo: PASS kthread_wkr  1000 runs (<= 1000 IRQs: coalesced 0), in_task=1
irqsim_demo: PASS tasklet      1000 runs (<= 1000 IRQs: coalesced 0), in_serving_softirq=1
irqsim_demo: PASS bh-wq        1000 runs (<= 1000 IRQs: coalesced 0), in_serving_softirq=1
irqsim_demo: latency from device event to handler start (us):
irqsim_demo:   path             runs      min      avg      max
irqsim_demo:   hard-irq         1000        6       11       48
irqsim_demo:   irq-thread       1000       14       22       71
irqsim_demo:   workqueue         998       19       35      160
irqsim_demo:   kthread_wkr      1000       17       30      122
irqsim_demo:   tasklet          1000       12       19       65
irqsim_demo:   bh-wq            1000       13       21       70
irqsim_demo: done: all checks passed
```

On a PC (non-RT) the hard handler reports `in_hardirq=1 in_task=0 (real
hard-IRQ context)` and a latency of ~0 µs (it runs nested inside the
hrtimer interrupt); with `thread_us=2000` the ONESHOT mask makes most ticks
coalesce (`ticks found the line masked` is then about two thirds of all ticks).

```
timers_demo: start: HZ=250, timer_ms=10, hr_us=1000, duration_ms=1000, PREEMPT_RT
timers_demo: PASS timer_list   83 runs (~83 expected): 10 ms -> 3 jiffies = 12 ms, max late 0 jiffies, in_serving_softirq=1
timers_demo: PASS delayed_work 81 runs (timer_list grid: 83), in_task=1
timers_demo: PASS hrtimer HARD 1000 periods (~1000 expected), drift 0 ns, in_hardirq=1
timers_demo: PASS hrtimer SOFT 1000 periods, drift 0 ns, in_serving_softirq=1
timers_demo: PASS naive drift  re-arm from now: 7210 us behind after 993 periods (~25956 ms per hour)
timers_demo: hrtimer lateness (callback start - planned expiry), us:
timers_demo:   timer             calls  overrun      min      avg      max
timers_demo:   hr fwd HARD        1000        0        3        6       25
timers_demo:   hr naive HARD       993        0        3        7       27
timers_demo:   hr fwd SOFT        1000        0        8       15       80
timers_demo: delay API timing, HZ=250 (1 jiffy = 4000 us), hrtimer resolution 1 ns:
timers_demo:   call                     kind     asked_us     min_us     avg_us     max_us
timers_demo:   ndelay(500)              busy            0          0          0          1
timers_demo:   udelay(10)               busy           10         10         10         11
timers_demo:   udelay(100)              busy          100        100        100        101
timers_demo:   mdelay(2)                busy         2000       2000       2000       2001
timers_demo:   usleep_range(100,200)    sleep         100        160        190        210
timers_demo:   usleep_range(1000,1100)  sleep        1000       1060       1095       1130
timers_demo:   msleep(1)                sleep        1000       4100       6000       7990
timers_demo:   msleep(20)               sleep       20000      20200      22100      23900
timers_demo:   fsleep(5)                busy            5          5          5          6
timers_demo:   fsleep(200)              sleep         200        215        245        260
timers_demo:   fsleep(30000)            sleep       30000      32100      33900      35800
timers_demo: done: all checks passed
```

Any `FAIL` line, a `done: N check(s) FAILED`, or any kernel
`BUG:`/`WARNING:` splat means something is wrong. The modules always
return 0 from init so you can `rmmod` them even after a FAIL.

---

## 9. Exercises

**Easy**
1. Load `irqsim_demo` with `period_us=100`, then `period_us=50`. Watch the
   coalesced counts and max latencies grow. Which path suffers first, and
   why is it the workqueue on a busy system?
2. Load it with `thread_us=2000` (thread slower than the device). Explain
   the `ticks found the line masked` number using ONESHOT.
3. Load `timers_demo timer_ms=1` on the board (HZ=250) and on the PC
   (HZ=1000). Why does the board do 250 runs/s at best?
4. Find the `irq/<N>-irqsim_demo` thread with `ps -eLo pid,cls,rtprio,comm`
   and raise it to priority 80 with `chrt`. Does the irq-thread latency change?

**Medium**
5. Make `irqsim_demo`'s line *shared*: request it a second time with
   `IRQF_SHARED` and another `dev_id` whose handler returns `IRQ_NONE`.
   What does `/proc/interrupts` show? What happens if the second handler
   wrongly returns `IRQ_HANDLED`?
6. Replace `handle_level_irq` with `handle_edge_irq` and remove ONESHOT.
   Raise `thread_us`. What changes in the accounting, and why do edge
   interrupts get *lost* where level ones are only delayed?
7. Change `timers_demo`'s `tl_fn` to re-arm with `jiffies + period`. Measure
   the drift after 10 s, and explain it.
8. On the board, add `chrt`-style priority to the `irqsim_kworker` thread
   from the module with `sched_set_fifo()`. Compare its latency with the
   workqueue path under `stress-ng --cpu 2`.

**Hard**
9. Turn `gpio_button_irq` into an input device (`input_allocate_device`,
   `input_report_key`, `input_sync`) so `evtest` sees `KEY_ENTER`. You
   have now rewritten `gpio_keys.c` — compare.
10. Add a `/dev/esh-button` char device to `gpio_button_irq` with a blocking
    `read()` returning one event record per press and `.poll` (see
    [03 §3.5](../03-kernel-synchronization/) and
    [01-char-devices-and-file-operations](../01-char-devices-and-file-operations/)).
11. Make `irqsim_demo`'s chip support `irq_set_affinity` and show with
    `/proc/irq/N/smp_affinity` and `ps -o psr` that the irq thread follows
    the IRQ's affinity.
12. Run `cyclictest -m -S -p 90 -i 200 -D 60` on the board with and without
    `irqsim_demo keep_running=1 period_us=50`. Then pin the simulated IRQ
    thread to CPU0 and cyclictest to CPU1 (`-a 1`). Explain the results.

---

## 10. Remember forever

```
device -> (TI INTR/INTA router) -> GIC SPI/PPI/SGI/LPI -> exception -> irq_domain hwirq->virq
       -> irq_desc: irq_chip (mask/ack/eoi) + flow handler + irqaction list
TOP HALF: fast, NEVER sleep, ack the device, return IRQ_NONE/HANDLED/WAKE_THREAD
request_threaded_irq(irq, hard_or_NULL, thread_fn, IRQF_ONESHOT, name, dev) -> irq/N-name FIFO 50
PREEMPT_RT: all handlers threaded (except IRQF_NO_THREAD); hard-ctx code needs raw_spinlock_t
BH: threaded IRQ > workqueue (may sleep) > BH wq/tasklet (atomic) ; kthread(_worker) for own loop/prio
work items coalesce: drain everything per run; cancel_*_sync()/tasklet_kill() before freeing
timer_list = jiffies (HZ=250 on board), softirq; hrtimer = ns, _HARD/_SOFT; hrtimer_forward = no drift
teardown self-rearming timer: timer_shutdown_sync(); hrtimer_cancel(); cancel_delayed_work_sync()
atomic: ndelay/udelay only (µs). process: usleep_range(10us..20ms), msleep(>=20ms), fsleep
devm order = reverse of acquisition: add cleanup actions BEFORE devm_request_irq
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name the four GIC interrupt types with an example of each on the
   BeagleBone AI-64. Which ones can be moved with `smp_affinity`?
2. What is the difference between a hwirq and a virq? Who translates, and
   why must a driver never hard-code an IRQ number?
3. Trace a press of a button on GPIO0_62 from the pad to your handler on the
   TDA4VM, naming every `irq_chip` on the way.
4. List five things a hard IRQ handler must never do and say what goes wrong.
5. When do you return `IRQ_NONE`? What does genirq do if a line keeps
   returning it?
6. Why does a threaded handler on a level-triggered line need
   `IRQF_ONESHOT`? Draw the storm without it.
7. What exactly changes for your interrupt handlers on PREEMPT_RT? What is
   `IRQF_NO_THREAD` for, and what must such a handler use for locking?
8. Softirq, tasklet, BH workqueue, workqueue, kthread: context, may-sleep,
   and one typical user each. Why are tasklets being removed?
9. What do `WQ_UNBOUND`, `WQ_HIGHPRI` and `WQ_MEM_RECLAIM` do? When is the
   last one mandatory?
10. Why must a bottom half handle "all pending events" instead of "one"?
11. Give the correct teardown order for a driver with an IRQ, a work item
    and a timer. How does devm get it right, and how can it get it wrong?
12. `timer_list` vs `hrtimer`: resolution, data structure, context,
    cost. What do `_SOFT` and `_HARD` mean, and which is the default on RT?
13. Why does re-arming an hrtimer with "now + period" drift? Write the
    correct callback.
14. Why is `msleep(1)` 4–8 ms on the board? What do you use for a 500 µs
    sleep? For a 5 µs wait while holding a spinlock?
15. Why must the debounce work be cancelled *after* the IRQ is freed, and
    how does `gpio_button_irq` guarantee that order with devm?
