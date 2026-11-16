// SPDX-License-Identifier: GPL-2.0
/*
 * trace_demo.c — your own tracepoints: a kthread emits esh_trace:esh_tick
 *                every period_ms and esh_trace:esh_late when it woke up
 *                more than 1 ms late
 *
 * Concept : A tracepoint is a printk you can switch on and off at run
 *           time, that writes a binary record into the per-CPU ftrace ring
 *           buffer (nanosecond timestamps, CPU, pid, preempt state) instead
 *           of the log, and that costs one NOP (a static key) while it is
 *           disabled. The events are defined in trace_demo_trace.h. Enable
 *           them in tracefs and read /sys/kernel/tracing/trace(_pipe). The
 *           rate is a module parameter that can be changed while loaded:
 *           its set() callback range-checks the new value.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel)
 * Run     : sudo insmod trace_demo.ko [period_ms=100]
 *           echo 1 | sudo tee /sys/kernel/tracing/events/esh_trace/enable
 *           sudo cat /sys/kernel/tracing/trace_pipe
 *           echo 20 | sudo tee /sys/module/trace_demo/parameters/period_ms
 *           sudo rmmod trace_demo
 * Read    : ../README.md  (section 3.6 "Tracepoints and your own TRACE_EVENT")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/string.h>

/*
 * Exactly ONE .c file of a module defines CREATE_TRACE_POINTS before
 * including the trace header: that instance generates the tracepoint code
 * and data. Every other file just includes the header.
 */
#define CREATE_TRACE_POINTS
#include "trace_demo_trace.h"

#define PERIOD_MIN 1
#define PERIOD_MAX 10000

static unsigned int period_ms = 100;

/* Writable parameter with validation: echo N > /sys/module/.../period_ms */
static int period_set(const char *val, const struct kernel_param *kp)
{
	return param_set_uint_minmax(val, kp, PERIOD_MIN, PERIOD_MAX);
}

static const struct kernel_param_ops period_ops = {
	.set = period_set,
	.get = param_get_uint,
};
module_param_cb(period_ms, &period_ops, &period_ms, 0644);
MODULE_PARM_DESC(period_ms, "tick period in ms (1..10000, default 100), writable at run time");

static struct task_struct *ticker;
static u64 ticks;

static int ticker_fn(void *unused)
{
	u64 seq = 0;

	while (!kthread_should_stop()) {
		unsigned int p = READ_ONCE(period_ms);
		ktime_t t0 = ktime_get();
		s64 late_us;

		/*
		 * Sleep, but return early when kthread_stop() wakes us.
		 * (msleep_interruptible() would NOT: it only stops early for
		 * a signal, and it keeps sleeping after a plain wake-up.)
		 */
		schedule_timeout_interruptible(msecs_to_jiffies(p));
		late_us = ktime_us_delta(ktime_get(), t0) - (s64)p * USEC_PER_MSEC;
		seq++;

		/* Disabled: a patched-out branch. Enabled: one ring-buffer record. */
		trace_esh_tick(seq, p, late_us);
		trace_esh_late(seq, late_us, current->comm);

		/*
		 * If computing an argument is expensive, guard it: the
		 * <event>_enabled() helper is the same static key.
		 */
		if (trace_esh_tick_enabled() && (seq % 100) == 0)
			pr_debug("100 more ticks traced (seq %llu)\n", seq);
	}
	WRITE_ONCE(ticks, seq);
	return 0;
}

static int __init trace_demo_init(void)
{
	if (period_ms < PERIOD_MIN || period_ms > PERIOD_MAX) {
		pr_err("period_ms must be %d..%d\n", PERIOD_MIN, PERIOD_MAX);
		return -EINVAL;
	}
	ticker = kthread_run(ticker_fn, NULL, "esh_trace");
	if (IS_ERR(ticker))
		return PTR_ERR(ticker);
	pr_info("ticking every %u ms; enable: echo 1 > /sys/kernel/tracing/events/esh_trace/enable\n",
		period_ms);
	return 0;
}

static void __exit trace_demo_exit(void)
{
	/*
	 * kthread_stop() wakes the thread (msleep_interruptible returns) and
	 * waits until ticker_fn() has returned. The thread only returns after
	 * kthread_should_stop() is true, so the task_struct is still valid.
	 */
	kthread_stop(ticker);
	pr_info("unloaded after %llu ticks\n", ticks);
}

module_init(trace_demo_init);
module_exit(trace_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Custom TRACE_EVENT tracepoints emitted from a kthread");
