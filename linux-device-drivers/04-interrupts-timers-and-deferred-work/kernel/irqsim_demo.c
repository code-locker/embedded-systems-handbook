// SPDX-License-Identifier: GPL-2.0
/*
 * irqsim_demo.c — a REAL Linux interrupt (irq_domain + irq_chip + virq +
 *                 request_threaded_irq) raised by a SIMULATED device, and five
 *                 bottom halves racing each other for the same event
 *
 * Concept : The host PC has no device we may safely interrupt from, so this
 *           module builds the hardware side in software: an hrtimer plays
 *           "the device" (it sets a STATUS register and asserts its line)
 *           and a tiny software interrupt controller (an irq_chip in its own
 *           irq_domain) turns that into a normal Linux IRQ number. From
 *           request_threaded_irq() upwards everything is the real genirq
 *           code path: /proc/interrupts counts it, IRQF_ONESHOT masks it,
 *           PREEMPT_RT force-threads it. The hard handler acks the device
 *           and timestamps; the bottom halves measure their latency:
 *             threaded handler | workqueue | kthread_worker | tasklet | BH wq
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/04-interrupts-timers-and-deferred-work/kernel)
 * Run     : sudo insmod irqsim_demo.ko period_us=1000 duration_ms=1000
 *           dmesg | grep irqsim_demo        -> PASS lines + latency table
 *           grep irqsim /proc/interrupts    ;  cat /sys/kernel/debug/irqsim_demo/stats
 *           sudo rmmod irqsim_demo
 *           (on the board or in a VM — never on a machine you care about)
 * Read    : ../README.md  (sections 3.1 "From pin to handler", 3.2
 *           "request_irq and handler rules", 3.3 "Bottom halves")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/workqueue.h>

/* BH workqueues (WQ_BH, system_bh_wq) are the tasklet replacement since 6.9. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
#define HAVE_BH_WQ 1
#else
#define HAVE_BH_WQ 0
#endif

static int period_us = 1000;
module_param(period_us, int, 0444);
MODULE_PARM_DESC(period_us, "simulated device event period in us (50..1000000, default 1000)");

static int duration_ms = 1000;
module_param(duration_ms, int, 0444);
MODULE_PARM_DESC(duration_ms, "self-test length in ms (100..60000, default 1000)");

static int thread_us;
module_param(thread_us, int, 0444);
MODULE_PARM_DESC(thread_us, "simulated slow work in the threaded handler, us (0..10000, default 0)");

static bool keep_running;
module_param(keep_running, bool, 0444);
MODULE_PARM_DESC(keep_running, "keep the device firing after the self-test (watch /proc/interrupts)");

static int nr_fail;

static __printf(3, 4) void check(bool ok, const char *test, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %-12s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-12s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ------------------------------------------------------------------------ */
/* Latency statistics: one writer per path, readers (debugfs) lock-free      */
/* ------------------------------------------------------------------------ */
/*
 * Each path below is guaranteed by the kernel never to run concurrently
 * with ITSELF (one IRQ's handler is serialised by genirq; one work item is
 * non-reentrant; a tasklet never runs on two CPUs at once). So each
 * lat_stats has exactly one writer and needs no lock. debugfs readers may
 * see a slightly stale mix of fields: fine for statistics. WRITE_ONCE /
 * READ_ONCE keep the compiler from tearing or caching the 64-bit fields.
 */
enum bh_path { P_HARD, P_THREAD, P_WQ, P_KWORKER, P_TASKLET, P_BHWQ, P_NR };

static const char * const path_name[P_NR] = {
	[P_HARD]	= "hard-irq",
	[P_THREAD]	= "irq-thread",
	[P_WQ]		= "workqueue",
	[P_KWORKER]	= "kthread_wkr",
	[P_TASKLET]	= "tasklet",
	[P_BHWQ]	= "bh-wq",
};

struct lat_stats {
	u64 n, sum_ns, min_ns, max_ns;
	bool in_hardirq, in_softirq, in_task;	/* context seen on first run */
};

static void lat_add(struct lat_stats *s, u64 ns)
{
	if (s->n == 0) {
		s->in_hardirq = in_hardirq();
		s->in_softirq = in_serving_softirq();
		s->in_task = in_task();
		WRITE_ONCE(s->min_ns, ns);
	}
	if (ns < s->min_ns)
		WRITE_ONCE(s->min_ns, ns);
	if (ns > s->max_ns)
		WRITE_ONCE(s->max_ns, ns);
	WRITE_ONCE(s->sum_ns, s->sum_ns + ns);
	WRITE_ONCE(s->n, s->n + 1);
}

/* ------------------------------------------------------------------------ */
/* The simulated device + its interrupt controller                          */
/* ------------------------------------------------------------------------ */
/*
 *   "hardware" (this module)                       genirq core (real kernel)
 *  +--------------------------+   line   +-----------+   virq   +------------------+
 *  | hrtimer every period_us  |--------->| esh-swirq |--------->| handle_level_irq |
 *  |  STATUS += 1             | asserted | irq_chip  | (domain  |  mask, run hard  |
 *  |  TIMESTAMP = now         |  while   | mask/unmask  maps    |  handler, wake   |
 *  +--------------------------+ STATUS>0 +-----------+ hwirq 0) |  thread, unmask  |
 *                                                                +------------------+
 * On a real SoC the left box is a peripheral and the middle box is the GIC
 * (plus, on the TDA4VM, an interrupt router in front of it). Here the
 * "line" is a function call: generic_handle_irq_safe(virq), made from the
 * hrtimer's HARD-irq callback — the same entry the GIC driver uses.
 */
struct swdev {
	struct hrtimer timer;
	ktime_t period;
	atomic_t status;	/* STATUS register: events not yet acked   */
	atomic64_t ts_ns;	/* TIMESTAMP register: newest event time    */
	atomic_t raised;	/* events the device produced (statistics)  */
	u64 while_masked;	/* ticks that found the line masked (timer only) */
	bool masked;		/* irq_chip state; set under the irq_desc lock */
};

static struct {
	struct swdev dev;
	struct fwnode_handle *fwnode;
	struct irq_domain *domain;
	unsigned int virq;
	bool irq_requested;

	atomic_t acked;		/* events acked by the hard handler */
	atomic64_t event_ns;	/* device timestamp of the newest acked event */
	struct lat_stats st[P_NR];

	struct workqueue_struct *wq;
	struct work_struct work;
	struct kthread_worker *kworker;
	struct kthread_work kwork;
	struct tasklet_struct tasklet;
#if HAVE_BH_WQ
	struct work_struct bh_work;
#endif
	struct dentry *dbg;
} sim;

static void swirq_mask(struct irq_data *d)
{
	WRITE_ONCE(sim.dev.masked, true);
}

static void swirq_unmask(struct irq_data *d)
{
	WRITE_ONCE(sim.dev.masked, false);
}

/*
 * The irq_chip is the driver of an interrupt CONTROLLER, not of a device:
 * mask/unmask/ack/set_type/set_affinity for one line. The GIC has one
 * (drivers/irqchip/irq-gic-v3.c), every GPIO controller that can interrupt
 * has one (gpio-davinci on the TDA4VM). No .irq_set_affinity here, so
 * writing /proc/irq/N/smp_affinity for this IRQ fails with EIO.
 */
static struct irq_chip swirq_chip = {
	.name		= "esh-swirq",
	.irq_mask	= swirq_mask,
	.irq_unmask	= swirq_unmask,
};

/* Called by irq_create_mapping(): connect a new virq to our chip + flow handler. */
static int swirq_map(struct irq_domain *d, unsigned int virq, irq_hw_number_t hw)
{
	/* level flow: mask+ack before the handler, unmask after it (ONESHOT: after the thread) */
	irq_set_chip_and_handler_name(virq, &swirq_chip, handle_level_irq, "level");
	irq_set_chip_data(virq, d->host_data);
	return 0;
}

static const struct irq_domain_ops swirq_domain_ops = {
	.map = swirq_map,
};

/* "Assert the line": mark an event and, if not masked, deliver the IRQ. */
static void swdev_raise(struct swdev *d)
{
	atomic_inc(&d->raised);
	atomic64_set(&d->ts_ns, ktime_get_ns());
	atomic_inc(&d->status);
	/*
	 * Level-triggered: while STATUS != 0 the line stays asserted. If it is
	 * masked (ONESHOT: the thread has not finished yet), a real controller
	 * holds it pending; we re-deliver on the next tick, which is when this
	 * function next runs. generic_handle_irq_safe() disables local IRQs
	 * itself, so it is legal from any context (here: hard hrtimer).
	 */
	if (!READ_ONCE(d->masked))
		generic_handle_irq_safe(sim.virq);
}

static enum hrtimer_restart swdev_tick(struct hrtimer *t)
{
	struct swdev *d = container_of(t, struct swdev, timer);

	if (READ_ONCE(d->masked))
		d->while_masked++;
	swdev_raise(d);
	/* forward on a fixed grid: start + k*period, no drift (see timers_demo.c) */
	hrtimer_forward_now(t, d->period);
	return HRTIMER_RESTART;
}

/* hrtimer_init() was replaced by hrtimer_setup() in 6.13. */
static void esh_hrtimer_setup(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *),
			      clockid_t clock, enum hrtimer_mode mode)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(t, fn, clock, mode);
#else
	hrtimer_init(t, clock, mode);
	t->function = fn;
#endif
}

static void swdev_start(void)
{
	/*
	 * _HARD: expire in hard-IRQ context even on PREEMPT_RT, where plain
	 * HRTIMER_MODE_REL timers are moved to softirq. A device interrupt
	 * does not wait for softirqs, so neither does our fake device.
	 */
	hrtimer_start(&sim.dev.timer, sim.dev.period, HRTIMER_MODE_REL_HARD);
}

static void swdev_stop(void)
{
	hrtimer_cancel(&sim.dev.timer);	/* waits for a running callback */
}

/* ------------------------------------------------------------------------ */
/* The driver: top half + bottom halves                                     */
/* ------------------------------------------------------------------------ */
static u64 since_event(void)
{
	return ktime_get_ns() - atomic64_read(&sim.event_ns);
}

/*
 * TOP HALF. Rules: be fast, never sleep, touch the device only as much as
 * needed to make it drop the line, hand everything else to a bottom half.
 * Return IRQ_NONE if it was not our device (shared lines!), IRQ_HANDLED if
 * done, IRQ_WAKE_THREAD to run the threaded handler.
 */
static irqreturn_t irqsim_hard(int irq, void *dev_id)
{
	u64 now = ktime_get_ns();
	u64 ts;
	int n;

	/* Read-and-clear STATUS = "ack the device": deasserts the line. */
	n = atomic_xchg(&sim.dev.status, 0);
	if (!n)
		return IRQ_NONE;	/* spurious or someone else's: genirq counts these */

	ts = atomic64_read(&sim.dev.ts_ns);
	atomic_add(n, &sim.acked);
	atomic64_set(&sim.event_ns, ts);
	lat_add(&sim.st[P_HARD], now - ts);

	/*
	 * Kick the other bottom halves. All four calls are IRQ-safe and
	 * idempotent: if the item is still pending they return false and the
	 * events are COALESCED — the bottom half must drain "everything
	 * pending", never assume one run per interrupt.
	 */
	queue_work(sim.wq, &sim.work);
	kthread_queue_work(sim.kworker, &sim.kwork);
	tasklet_schedule(&sim.tasklet);
#if HAVE_BH_WQ
	queue_work(system_bh_wq, &sim.bh_work);
#endif
	return IRQ_WAKE_THREAD;
}

/*
 * THREADED HANDLER: runs in the kthread "irq/<N>-irqsim_demo" (SCHED_FIFO 50),
 * process context, may sleep. With IRQF_ONESHOT the line stays masked
 * until this returns, so a level IRQ cannot storm meanwhile.
 */
static irqreturn_t irqsim_thread(int irq, void *dev_id)
{
	lat_add(&sim.st[P_THREAD], since_event());
	if (thread_us)
		usleep_range(thread_us, thread_us + 10);	/* allowed: we may sleep */
	return IRQ_HANDLED;
}

/* Workqueue item: a kworker thread, process context, may sleep. */
static void irqsim_work_fn(struct work_struct *w)
{
	lat_add(&sim.st[P_WQ], since_event());
}

/* kthread_worker item: YOUR dedicated thread (you can chrt it to SCHED_FIFO). */
static void irqsim_kwork_fn(struct kthread_work *w)
{
	lat_add(&sim.st[P_KWORKER], since_event());
}

/* Tasklet: softirq context (TASKLET_SOFTIRQ), may NOT sleep. Deprecated direction. */
static void irqsim_tasklet_fn(struct tasklet_struct *t)
{
	lat_add(&sim.st[P_TASKLET], since_event());
}

#if HAVE_BH_WQ
/* BH workqueue item: also softirq context, the modern tasklet replacement. */
static void irqsim_bh_fn(struct work_struct *w)
{
	lat_add(&sim.st[P_BHWQ], since_event());
}
#endif

/* Wait until every path is idle: device stopped, handlers and BHs done. */
static void irqsim_quiesce(void)
{
	swdev_stop();
	synchronize_irq(sim.virq);	/* hard handler AND threaded handler finished */
	flush_workqueue(sim.wq);
	kthread_flush_worker(sim.kworker);
	tasklet_kill(&sim.tasklet);
#if HAVE_BH_WQ
	flush_work(&sim.bh_work);
#endif
}

/* ------------------------------------------------------------------------ */
/* debugfs: stats (read) and trigger (write anything = raise one event)     */
/* ------------------------------------------------------------------------ */
static int stats_show(struct seq_file *m, void *v)
{
	int p;

	seq_printf(m, "virq %u  raised %d  acked %d  pending %d  ticks-while-masked %llu\n",
		   sim.virq, atomic_read(&sim.dev.raised), atomic_read(&sim.acked),
		   atomic_read(&sim.dev.status), READ_ONCE(sim.dev.while_masked));
	seq_printf(m, "%-12s %8s %10s %10s %10s\n", "path", "runs", "min_us", "avg_us", "max_us");
	for (p = 0; p < P_NR; p++) {
		u64 n = READ_ONCE(sim.st[p].n);
		u64 avg = n ? div64_u64(READ_ONCE(sim.st[p].sum_ns), n) : 0;

		seq_printf(m, "%-12s %8llu %10llu %10llu %10llu\n", path_name[p], n,
			   div_u64(READ_ONCE(sim.st[p].min_ns), 1000), div_u64(avg, 1000),
			   div_u64(READ_ONCE(sim.st[p].max_ns), 1000));
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static ssize_t trigger_write(struct file *f, const char __user *buf, size_t len, loff_t *pos)
{
	swdev_raise(&sim.dev);	/* process context is fine: generic_handle_irq_safe() */
	return len;
}

static const struct file_operations trigger_fops = {
	.owner	= THIS_MODULE,
	.write	= trigger_write,
	.llseek	= noop_llseek,
};

/* ------------------------------------------------------------------------ */
/* Self-test report                                                          */
/* ------------------------------------------------------------------------ */
static void report(void)
{
	int raised = atomic_read(&sim.dev.raised);
	int acked = atomic_read(&sim.acked);
	int pending = atomic_read(&sim.dev.status);
	u64 hard_runs = sim.st[P_HARD].n;
	int p;

	check(raised > 0 && acked + pending == raised, "accounting",
	      "device raised %d events, hard handler acked %d, %d still pending",
	      raised, acked, pending);
	check(hard_runs > 0 && sim.st[P_HARD].in_hardirq != sim.st[P_HARD].in_task,
	      "hard-irq", "%llu runs, in_hardirq=%d in_task=%d (%s)", hard_runs,
	      sim.st[P_HARD].in_hardirq, sim.st[P_HARD].in_task,
	      sim.st[P_HARD].in_hardirq ? "real hard-IRQ context" :
	      "force-threaded: PREEMPT_RT or threadirqs");
	pr_info("INFO %llu ticks found the line masked (ONESHOT thread still running) -> coalesced\n",
		sim.dev.while_masked);

	for (p = P_THREAD; p < P_NR; p++) {
		const struct lat_stats *s = &sim.st[p];
		bool want_task = p == P_THREAD || p == P_WQ || p == P_KWORKER;
		bool ctx_ok = want_task ? s->in_task : s->in_softirq;

		if (p == P_BHWQ && !HAVE_BH_WQ) {
			pr_info("INFO %-12s needs kernel >= 6.9 (WQ_BH), skipped\n", path_name[p]);
			continue;
		}
		check(s->n > 0 && s->n <= hard_runs && ctx_ok, path_name[p],
		      "%llu runs (<= %llu IRQs: coalesced %llu), %s=%d",
		      s->n, hard_runs, hard_runs - min(hard_runs, s->n),
		      want_task ? "in_task" : "in_serving_softirq",
		      want_task ? s->in_task : s->in_softirq);
	}

	pr_info("latency from device event to handler start (us):\n");
	pr_info("  %-12s %8s %8s %8s %8s\n", "path", "runs", "min", "avg", "max");
	for (p = 0; p < P_NR; p++) {
		const struct lat_stats *s = &sim.st[p];

		if (!s->n)
			continue;
		pr_info("  %-12s %8llu %8llu %8llu %8llu\n", path_name[p], s->n,
			div_u64(s->min_ns, 1000), div_u64(div64_u64(s->sum_ns, s->n), 1000),
			div_u64(s->max_ns, 1000));
	}
}

/* ------------------------------------------------------------------------ */
/* init / exit — note the teardown order is the setup order reversed         */
/* ------------------------------------------------------------------------ */
static void irqsim_teardown(void)
{
	debugfs_remove_recursive(sim.dbg);	/* no more trigger writes */
	swdev_stop();				/* 1. device silent       */
	if (sim.irq_requested)
		free_irq(sim.virq, &sim);	/* 2. waits for both handlers */
	if (sim.kworker) {			/* 3. no BH can be queued now: kill them */
		kthread_cancel_work_sync(&sim.kwork);
		kthread_destroy_worker(sim.kworker);
	}
	tasklet_kill(&sim.tasklet);
#if HAVE_BH_WQ
	cancel_work_sync(&sim.bh_work);
#endif
	if (sim.wq)
		destroy_workqueue(sim.wq);	/* drains it, then frees */
	if (sim.virq)
		irq_dispose_mapping(sim.virq);	/* 4. controller last */
	if (sim.domain)
		irq_domain_remove(sim.domain);
	if (sim.fwnode)
		irq_domain_free_fwnode(sim.fwnode);
}

static int __init irqsim_init(void)
{
	int err;

	if (period_us < 50 || period_us > 1000000 || duration_ms < 100 ||
	    duration_ms > 60000 || thread_us < 0 || thread_us > 10000) {
		pr_err("period_us 50..1000000, duration_ms 100..60000, thread_us 0..10000\n");
		return -EINVAL;
	}

	/* Static object: also zeroes every pointer the teardown tests. */
	sim.dev.period = ns_to_ktime((u64)period_us * NSEC_PER_USEC);
	esh_hrtimer_setup(&sim.dev.timer, swdev_tick, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);
	INIT_WORK(&sim.work, irqsim_work_fn);
	kthread_init_work(&sim.kwork, irqsim_kwork_fn);
	tasklet_setup(&sim.tasklet, irqsim_tasklet_fn);
#if HAVE_BH_WQ
	INIT_WORK(&sim.bh_work, irqsim_bh_fn);
#endif

	/* --- the interrupt controller: domain with 1 line, hwirq 0 -> virq --- */
	sim.fwnode = irq_domain_alloc_named_fwnode("irqsim_demo");
	if (!sim.fwnode) {
		err = -ENOMEM;
		goto fail;
	}
	sim.domain = irq_domain_create_linear(sim.fwnode, 1, &swirq_domain_ops, &sim);
	if (!sim.domain) {
		err = -ENOMEM;
		goto fail;
	}
	sim.virq = irq_create_mapping(sim.domain, 0);
	if (!sim.virq) {
		err = -ENXIO;
		goto fail;
	}

	/* --- the bottom-half machinery, BEFORE the IRQ can fire --- */
	/*
	 * Our own workqueue: WQ_HIGHPRI = workers at nice -20; WQ_UNBOUND =
	 * any CPU (scheduler picks). WQ_MEM_RECLAIM would add a rescuer thread
	 * (needed only if this work must make progress during memory reclaim,
	 * e.g. block/network drivers). Plain schedule_work() uses system_wq.
	 */
	sim.wq = alloc_workqueue("irqsim_wq", WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!sim.wq) {
		err = -ENOMEM;
		goto fail;
	}
#ifdef kthread_run_worker
	sim.kworker = kthread_run_worker(0, "irqsim_kworker");	/* >= 6.14 */
#else
	sim.kworker = kthread_create_worker(0, "irqsim_kworker");
#endif
	if (IS_ERR(sim.kworker)) {
		err = PTR_ERR(sim.kworker);
		sim.kworker = NULL;
		goto fail;
	}

	/* --- the driver part: exactly what a real driver does --- */
	err = request_threaded_irq(sim.virq, irqsim_hard, irqsim_thread,
				   IRQF_ONESHOT, KBUILD_MODNAME, &sim);
	if (err) {
		pr_err("request_threaded_irq(%u) failed: %d\n", sim.virq, err);
		goto fail;
	}
	sim.irq_requested = true;
	check(true, "request_irq", "virq %u = hwirq 0 of domain irqsim_demo, chip %s, IRQF_ONESHOT",
	      sim.virq, swirq_chip.name);

	sim.dbg = debugfs_create_dir(KBUILD_MODNAME, NULL);
	debugfs_create_file("stats", 0444, sim.dbg, NULL, &stats_fops);
	debugfs_create_file("trigger", 0200, sim.dbg, NULL, &trigger_fops);

	pr_info("start: device period %d us for %d ms, thread_us=%d, HZ=%d, %s\n",
		period_us, duration_ms, thread_us, HZ,
		IS_ENABLED(CONFIG_PREEMPT_RT) ? "PREEMPT_RT" : "not RT");

	/* --- self-test: let the device fire, then stop and count --- */
	swdev_start();
	msleep(duration_ms);
	irqsim_quiesce();
	report();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");

	if (keep_running) {
		pr_info("keep_running=1: device keeps firing until rmmod\n");
		swdev_start();
	}
	/* Return 0 even on FAIL so the module can be inspected and rmmod'ed. */
	return 0;

fail:
	irqsim_teardown();
	return err;
}

static void __exit irqsim_exit(void)
{
	irqsim_teardown();
	pr_info("unloaded (raised %d events in total)\n", atomic_read(&sim.dev.raised));
}

module_init(irqsim_init);
module_exit(irqsim_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: real virq from a simulated device, threaded IRQ, workqueue/kthread/tasklet/BH latency");
