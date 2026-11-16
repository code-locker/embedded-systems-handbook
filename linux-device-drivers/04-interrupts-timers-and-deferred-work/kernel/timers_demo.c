// SPDX-License-Identifier: GPL-2.0
/*
 * timers_demo.c — kernel time: jiffies + timer_list, hrtimer (hard vs soft,
 *                 drift-free vs drifting re-arm), delayed_work, and a measured
 *                 table of every delay/sleep API
 *
 * Concept : A periodic timer_list ticks on the jiffies grid (HZ), so its
 *           real period is rounded to whole jiffies. An hrtimer has ns
 *           resolution; re-arming it with hrtimer_forward() keeps a fixed
 *           grid, re-arming it "now + period" accumulates every callback's
 *           lateness (drift). _HARD timers expire in hard-IRQ context,
 *           _SOFT ones in softirq. delayed_work is a timer that runs a work
 *           item in process context. Finally every delay API is timed:
 *           busy-waits (ndelay/udelay/mdelay) vs sleeps (usleep_range,
 *           msleep, fsleep) — and msleep(1) is NOT 1 ms.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/04-interrupts-timers-and-deferred-work/kernel)
 * Run     : sudo insmod timers_demo.ko timer_ms=10 hr_us=1000 duration_ms=1000
 *           dmesg | grep timers_demo        -> PASS lines + two tables
 *           sudo rmmod timers_demo
 *           (on the board or in a VM — never on a machine you care about)
 * Read    : ../README.md  (sections 3.5 "Timers" and 3.6 "Delays and sleeps")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/hrtimer.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/workqueue.h>

/* from_timer() was renamed timer_container_of() in 6.16; both are macros. */
#ifndef timer_container_of
#define timer_container_of(var, callback_timer, timer_fieldname) \
	from_timer(var, callback_timer, timer_fieldname)
#endif

static int timer_ms = 10;
module_param(timer_ms, int, 0444);
MODULE_PARM_DESC(timer_ms, "timer_list / delayed_work period in ms (1..1000, default 10)");

static int hr_us = 1000;
module_param(hr_us, int, 0444);
MODULE_PARM_DESC(hr_us, "hrtimer period in us (50..100000, default 1000)");

static int duration_ms = 1000;
module_param(duration_ms, int, 0444);
MODULE_PARM_DESC(duration_ms, "how long the periodic timers run, ms (200..10000, default 1000)");

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
/* 1. timer_list: the jiffies-resolution timer wheel (softirq context)      */
/* ------------------------------------------------------------------------ */
/*
 *  jiffies:  |----|----|----|----|----|----|----|    one tick = 1/HZ s
 *            ^ mod_timer(t, jiffies + 3)  ^ fires here (or a bit later)
 *
 * timer_ms is converted with msecs_to_jiffies(), which ROUNDS UP: on the
 * board (HZ=250, 4 ms ticks) 10 ms becomes 3 jiffies = 12 ms. Re-arming
 * with "t->expires + period" (not "jiffies + period") keeps a fixed grid:
 * a late callback does not push all later ones back.
 */
static struct tl_state {
	struct timer_list timer;
	unsigned long period_j;
	unsigned long n;
	long max_late_j;	/* jiffies - expires, seen in the callback */
	bool in_softirq;
} tl;

static void tl_fn(struct timer_list *t)
{
	struct tl_state *s = timer_container_of(s, t, timer);
	long late = (long)(jiffies - t->expires);

	if (s->n++ == 0)
		s->in_softirq = in_serving_softirq();
	s->max_late_j = max(s->max_late_j, late);
	mod_timer(t, t->expires + s->period_j);	/* fixed grid */
}

/* ------------------------------------------------------------------------ */
/* 2. hrtimer: ns resolution; hard vs soft expiry; forward vs naive re-arm  */
/* ------------------------------------------------------------------------ */
/*
 * planned expiries of the forward timer:   S+P  S+2P  S+3P  S+4P   (fixed grid)
 * callback actually runs:                   |+l1  |+l2  |+l3  |+l4  (lateness l)
 *
 * naive timer re-armed "now + P":           S+P  S+2P+l1  S+3P+l1+l2 ...
 *                                           -> drift = l1 + l2 + ... grows forever
 */
struct hrt_state {
	struct hrtimer timer;
	const char *name;
	ktime_t period;
	ktime_t start;		/* grid origin: first expiry = start + period */
	ktime_t last_planned;	/* expiry the latest callback was planned for */
	u64 last_index;		/* ... which is grid point number last_index  */
	u64 callbacks, periods;	/* periods > callbacks if some were overrun */
	u64 lat_min, lat_max, lat_sum;
	bool naive;
	bool in_hardirq, in_softirq;
};

static struct hrt_state hr_fwd_hard = { .name = "hr fwd HARD" };
static struct hrt_state hr_naive_hard = { .name = "hr naive HARD", .naive = true };
static struct hrt_state hr_fwd_soft = { .name = "hr fwd SOFT" };

static enum hrtimer_restart hrt_fn(struct hrtimer *t)
{
	struct hrt_state *s = container_of(t, struct hrt_state, timer);
	ktime_t now = ktime_get();
	u64 late;

	s->last_planned = hrtimer_get_expires(t);
	s->last_index = s->periods + 1;	/* grid point 1 is start + period */
	late = ktime_to_ns(ktime_sub(now, s->last_planned));
	if (s->callbacks++ == 0) {
		s->in_hardirq = in_hardirq();
		s->in_softirq = in_serving_softirq();
		s->lat_min = late;
	}
	s->lat_min = min(s->lat_min, late);
	s->lat_max = max(s->lat_max, late);
	s->lat_sum += late;

	if (s->naive) {
		/* WRONG for periodic work: the lateness of this run is lost forever. */
		hrtimer_set_expires(t, ktime_add(now, s->period));
		s->periods++;
	} else {
		/* RIGHT: next grid point after now; returns how many periods we advanced. */
		s->periods += hrtimer_forward(t, now, s->period);
	}
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

/*
 * The HARD/SOFT choice is made at setup time and must match at start time.
 * Without either suffix: hard on a normal kernel, SOFT on PREEMPT_RT.
 */
static void hrt_start(struct hrt_state *s, enum hrtimer_mode mode)
{
	ktime_t first;

	s->period = ns_to_ktime((u64)hr_us * NSEC_PER_USEC);
	esh_hrtimer_setup(&s->timer, hrt_fn, CLOCK_MONOTONIC, mode);
	first = ktime_add(ktime_get(), s->period);
	s->start = ktime_sub(first, s->period);
	hrtimer_start(&s->timer, first, mode);	/* ABS: an absolute CLOCK_MONOTONIC time */
}

/* drift = where the last callback was planned - where the grid says it should be */
static s64 hrt_drift_ns(const struct hrt_state *s)
{
	ktime_t grid = ktime_add_ns(s->start, s->last_index * ktime_to_ns(s->period));

	return ktime_to_ns(ktime_sub(s->last_planned, grid));
}

/* ------------------------------------------------------------------------ */
/* 3. delayed_work: a timer whose expiry queues a work item (process ctx)    */
/* ------------------------------------------------------------------------ */
static struct dw_state {
	struct delayed_work dw;
	unsigned long period_j;
	unsigned long n;
	bool in_task;
} dws;

static void dw_fn(struct work_struct *w)
{
	struct dw_state *s = container_of(to_delayed_work(w), struct dw_state, dw);

	if (s->n++ == 0)
		s->in_task = in_task();
	/* process context: we could msleep() or mutex_lock() here */
	schedule_delayed_work(&s->dw, s->period_j);	/* relative re-arm: drifts */
}

static void run_periodic(void)
{
	unsigned long j0, elapsed_j, expect_tl, expect_hr;
	ktime_t t0;
	s64 elapsed_ns;
	const struct hrt_state *h;
	int i;

	tl.period_j = max(1UL, msecs_to_jiffies(timer_ms));
	dws.period_j = tl.period_j;
	timer_setup(&tl.timer, tl_fn, 0);
	INIT_DELAYED_WORK(&dws.dw, dw_fn);

	t0 = ktime_get();
	j0 = jiffies;
	mod_timer(&tl.timer, j0 + tl.period_j);
	schedule_delayed_work(&dws.dw, dws.period_j);
	hrt_start(&hr_fwd_hard, HRTIMER_MODE_ABS_HARD);
	hrt_start(&hr_naive_hard, HRTIMER_MODE_ABS_HARD);
	hrt_start(&hr_fwd_soft, HRTIMER_MODE_ABS_SOFT);

	msleep(duration_ms);

	/*
	 * Stop each one so that no callback runs afterwards:
	 *  - timer_shutdown_sync(): waits for a running callback and makes the
	 *    self re-arming mod_timer() in tl_fn a no-op (timer_delete_sync
	 *    could lose that race). The timer cannot be re-armed afterwards.
	 *  - hrtimer_cancel(): waits for a running callback; a callback that
	 *    returned HRTIMER_RESTART is not re-queued once cancel started.
	 *  - cancel_delayed_work_sync(): handles self re-queueing work.
	 */
	timer_shutdown_sync(&tl.timer);
	hrtimer_cancel(&hr_fwd_hard.timer);
	hrtimer_cancel(&hr_naive_hard.timer);
	hrtimer_cancel(&hr_fwd_soft.timer);
	cancel_delayed_work_sync(&dws.dw);
	elapsed_j = jiffies - j0;
	elapsed_ns = ktime_to_ns(ktime_sub(ktime_get(), t0));

	/* timer_list */
	expect_tl = elapsed_j / tl.period_j;
	check(tl.n + 2 >= expect_tl * 9 / 10 && tl.n <= expect_tl + 1 && tl.in_softirq,
	      "timer_list",
	      "%lu runs (~%lu expected): %d ms -> %lu jiffies = %u ms, max late %ld jiffies, in_serving_softirq=%d",
	      tl.n, expect_tl, timer_ms, tl.period_j, jiffies_to_msecs(tl.period_j),
	      tl.max_late_j, tl.in_softirq);

	/* delayed_work: relative re-arm, so it can only fall behind, never ahead */
	check(dws.n > 0 && dws.n <= expect_tl + 1 && dws.in_task, "delayed_work",
	      "%lu runs (timer_list grid: %lu), in_task=%d", dws.n, expect_tl, dws.in_task);

	/* hrtimers */
	expect_hr = div_u64(elapsed_ns, hr_us * NSEC_PER_USEC);
	check(hr_fwd_hard.periods + 2 >= expect_hr * 9 / 10 &&
	      hr_fwd_hard.periods <= expect_hr + 1 && hrt_drift_ns(&hr_fwd_hard) == 0 &&
	      hr_fwd_hard.in_hardirq, "hrtimer HARD",
	      "%llu periods (~%lu expected), drift %lld ns, in_hardirq=%d",
	      hr_fwd_hard.periods, expect_hr, hrt_drift_ns(&hr_fwd_hard), hr_fwd_hard.in_hardirq);
	check(hr_fwd_soft.periods + 2 >= expect_hr * 9 / 10 && hrt_drift_ns(&hr_fwd_soft) == 0 &&
	      hr_fwd_soft.in_softirq, "hrtimer SOFT",
	      "%llu periods, drift %lld ns, in_serving_softirq=%d",
	      hr_fwd_soft.periods, hrt_drift_ns(&hr_fwd_soft), hr_fwd_soft.in_softirq);
	check(hrt_drift_ns(&hr_naive_hard) > hrt_drift_ns(&hr_fwd_hard), "naive drift",
	      "re-arm from now: %lld us behind after %llu periods (~%lld ms per hour)",
	      div_s64(hrt_drift_ns(&hr_naive_hard), 1000), hr_naive_hard.periods,
	      div_s64(hrt_drift_ns(&hr_naive_hard) * 3600LL, max_t(s64, elapsed_ns / 1000, 1)));

	pr_info("hrtimer lateness (callback start - planned expiry), us:\n");
	pr_info("  %-14s %8s %8s %8s %8s %8s\n", "timer", "calls", "overrun", "min", "avg", "max");
	for (i = 0; i < 3; i++) {
		h = i == 0 ? &hr_fwd_hard : i == 1 ? &hr_naive_hard : &hr_fwd_soft;
		if (!h->callbacks)
			continue;
		pr_info("  %-14s %8llu %8llu %8llu %8llu %8llu\n", h->name, h->callbacks,
			h->periods - h->callbacks, div_u64(h->lat_min, 1000),
			div_u64(div64_u64(h->lat_sum, h->callbacks), 1000),
			div_u64(h->lat_max, 1000));
	}
}

/* ------------------------------------------------------------------------ */
/* 4. Delay table: what each API really costs                                */
/* ------------------------------------------------------------------------ */
enum dkind { D_NDELAY, D_UDELAY, D_MDELAY, D_USLEEP, D_MSLEEP, D_FSLEEP };

struct delay_case {
	const char *call;
	enum dkind kind;
	unsigned long arg, arg2;
	u64 req_ns;		/* what the caller asked for (lower bound)    */
	bool sleeps;		/* false: busy-waits (burns the CPU)          */
	int samples;
};

static const struct delay_case cases[] = {
	{ "ndelay(500)",             D_NDELAY, 500,   0,    500,                 false, 20 },
	{ "udelay(10)",              D_UDELAY, 10,    0,    10 * NSEC_PER_USEC,  false, 20 },
	{ "udelay(100)",             D_UDELAY, 100,   0,    100 * NSEC_PER_USEC, false, 10 },
	{ "mdelay(2)",               D_MDELAY, 2,     0,    2 * NSEC_PER_MSEC,   false, 3 },
	{ "usleep_range(100,200)",   D_USLEEP, 100,   200,  100 * NSEC_PER_USEC, true, 20 },
	{ "usleep_range(1000,1100)", D_USLEEP, 1000,  1100, NSEC_PER_MSEC,       true, 10 },
	{ "msleep(1)",               D_MSLEEP, 1,     0,    NSEC_PER_MSEC,       true, 10 },
	{ "msleep(20)",              D_MSLEEP, 20,    0,    20 * NSEC_PER_MSEC,  true, 3 },
	{ "fsleep(5)",               D_FSLEEP, 5,     0,    5 * NSEC_PER_USEC,   false, 20 },
	{ "fsleep(200)",             D_FSLEEP, 200,   0,    200 * NSEC_PER_USEC, true, 10 },
	{ "fsleep(30000)",           D_FSLEEP, 30000, 0,    30 * NSEC_PER_MSEC,  true, 3 },
};

static void do_delay(const struct delay_case *c)
{
	switch (c->kind) {
	case D_NDELAY:
		ndelay(c->arg);
		break;
	case D_UDELAY:
		udelay(c->arg);
		break;
	case D_MDELAY:
		mdelay(c->arg);
		break;
	case D_USLEEP:
		usleep_range(c->arg, c->arg2);
		break;
	case D_MSLEEP:
		msleep(c->arg);
		break;
	case D_FSLEEP:
		fsleep(c->arg);
		break;
	}
}

static void run_delay_table(void)
{
	unsigned int i;
	int k;

	pr_info("delay API timing, HZ=%d (1 jiffy = %u us), hrtimer resolution %u ns:\n",
		HZ, jiffies_to_usecs(1), hrtimer_resolution);
	pr_info("  %-24s %-6s %10s %10s %10s %10s\n",
		"call", "kind", "asked_us", "min_us", "avg_us", "max_us");
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct delay_case *c = &cases[i];
		u64 mn = U64_MAX, mx = 0, sum = 0;
		bool ok;

		for (k = 0; k < c->samples; k++) {
			u64 t0 = ktime_get_ns(), d;

			do_delay(c);
			d = ktime_get_ns() - t0;
			mn = min(mn, d);
			mx = max(mx, d);
			sum += d;
		}
		/*
		 * A sleep never returns early. A busy-wait is calibrated and may be
		 * a hair short on some clocks, so allow 10% there.
		 */
		ok = c->sleeps ? mn >= c->req_ns : mn >= c->req_ns * 9 / 10;
		if (!ok)
			nr_fail++;
		pr_info("  %-24s %-6s %10llu %10llu %10llu %10llu%s\n", c->call,
			c->sleeps ? "sleep" : "busy", div_u64(c->req_ns, 1000),
			div_u64(mn, 1000), div_u64(div_u64(sum, c->samples), 1000),
			div_u64(mx, 1000), ok ? "" : "   <- FAIL: returned early");
		cond_resched();
	}
}

/* ------------------------------------------------------------------------ */

static int __init timers_demo_init(void)
{
	if (timer_ms < 1 || timer_ms > 1000 || hr_us < 50 || hr_us > 100000 ||
	    duration_ms < 200 || duration_ms > 10000) {
		pr_err("timer_ms 1..1000, hr_us 50..100000, duration_ms 200..10000\n");
		return -EINVAL;
	}
	pr_info("start: HZ=%d, timer_ms=%d, hr_us=%d, duration_ms=%d, %s\n", HZ, timer_ms,
		hr_us, duration_ms, IS_ENABLED(CONFIG_PREEMPT_RT) ? "PREEMPT_RT" : "not RT");

	run_periodic();
	run_delay_table();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");
	/* Return 0 even on FAIL so the module can be inspected and rmmod'ed. */
	return 0;
}

static void __exit timers_demo_exit(void)
{
	/* Every timer and work item was stopped synchronously in init. */
	pr_info("unloaded\n");
}

module_init(timers_demo_init);
module_exit(timers_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: timer_list, hrtimer hard/soft and drift, delayed_work, delay API table");
