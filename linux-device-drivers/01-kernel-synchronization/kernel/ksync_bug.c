// SPDX-License-Identifier: GPL-2.0
/*
 * ksync_bug.c — INTENTIONALLY BROKEN: three locking bugs the kernel's debug
 *               options catch for you
 *
 * Concept : Each bug is written so that it does NOT actually hang the
 *           machine — it only does what a real driver does on its bad day,
 *           once, so the checker can report it:
 *             bug=1  mutex_lock() inside a spinlock (sleeping in atomic
 *                    context)  -> CONFIG_DEBUG_ATOMIC_SLEEP:
 *                    "BUG: sleeping function called from invalid context"
 *             bug=2  lock A then B here, B then A there (ABBA)
 *                    -> CONFIG_PROVE_LOCKING (lockdep):
 *                    "WARNING: possible circular locking dependency detected"
 *             bug=3  lock taken with plain spin_lock() in process context
 *                    AND in a timer (softirq)  -> lockdep:
 *                    "WARNING: inconsistent lock state
 *                     {SOFTIRQ-ON-W} -> {IN-SOFTIRQ-W} usage"
 *           Without those options enabled nothing is printed: that is the
 *           lesson — the bug is still there, only nobody tells you.
 * Build   : make -C linux-device-drivers/01-kernel-synchronization/kernel BUGS=1
 *           (NOT built by default, and not by `make drivers`)
 * Run     : ONLY on the board or in a VM with a debug kernel:
 *           sudo insmod ksync_bug.ko bug=1 ; dmesg ; sudo rmmod ksync_bug
 * Read    : ../README.md  (section 3.7 "Debugging tools: lockdep, KCSAN, sparse")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

static int bug = 1;
module_param(bug, int, 0444);
MODULE_PARM_DESC(bug, "1 = mutex under spinlock, 2 = ABBA lock order, 3 = spinlock without _bh vs timer");

/* ---- bug 1: sleeping in atomic context ---------------------------------- */
/*
 * raw_spinlock_t on purpose: on PREEMPT_RT a spinlock_t is itself a sleeping
 * lock, so "mutex inside spinlock_t" is legal there. raw_ is atomic on every
 * kernel, so the bug is a bug everywhere. (On a non-RT kernel the classic
 * spin_lock() + mutex_lock() mistake behaves exactly like this.)
 *
 * The mutex is free, so mutex_lock() does not really sleep — it would only
 * sleep under contention, which is exactly why this bug hides in testing.
 * might_sleep() inside mutex_lock() reports it anyway.
 */
static DEFINE_RAW_SPINLOCK(bug_spin);
static DEFINE_MUTEX(bug_mutex);

static void bug_sleep_in_atomic(void)
{
	raw_spin_lock(&bug_spin);
	mutex_lock(&bug_mutex);		/* BUG: may sleep, but we are atomic */
	mutex_unlock(&bug_mutex);
	raw_spin_unlock(&bug_spin);
}

/* ---- bug 2: ABBA lock ordering ----------------------------------------- */
/*
 *   path 1:  lock(A) -> lock(B)        thread X holds A, wants B
 *   path 2:  lock(B) -> lock(A)        thread Y holds B, wants A  => deadlock
 *
 * Here both paths run one after the other in the same thread, so nothing
 * hangs — but lockdep remembers "A before B" from path 1 and screams when
 * path 2 creates "B before A". It finds deadlocks that did not happen (yet).
 */
static DEFINE_MUTEX(lock_a);
static DEFINE_MUTEX(lock_b);

static void bug_abba(void)
{
	mutex_lock(&lock_a);
	mutex_lock(&lock_b);
	mutex_unlock(&lock_b);
	mutex_unlock(&lock_a);

	mutex_lock(&lock_b);
	mutex_lock(&lock_a);		/* lockdep: circular dependency */
	mutex_unlock(&lock_a);
	mutex_unlock(&lock_b);
}

/* ---- bug 3: softirq-unsafe lock --------------------------------------- */
/*
 * Process context takes bug_bh_lock with plain spin_lock() (softirqs stay
 * ON), a timer callback takes it in softirq context. If the timer fires on
 * the CPU that holds the lock -> self-deadlock (see ksync_spin.c test 6).
 * We take it in process context FIRST and only then arm the timer, so the
 * deadlock cannot happen here; lockdep still sees both usages and reports.
 * Fix: spin_lock_bh() in process context.
 */
static DEFINE_SPINLOCK(bug_bh_lock);
static struct timer_list bug_timer;
static int bug_shared;

static void bug_timer_fn(struct timer_list *t)
{
	spin_lock(&bug_bh_lock);
	bug_shared++;
	spin_unlock(&bug_bh_lock);
}

static void bug_softirq_unsafe(void)
{
	spin_lock(&bug_bh_lock);	/* should be spin_lock_bh() */
	bug_shared++;
	spin_unlock(&bug_bh_lock);

	timer_setup(&bug_timer, bug_timer_fn, 0);
	mod_timer(&bug_timer, jiffies + 1);
	msleep(50);			/* let it fire */
	timer_shutdown_sync(&bug_timer);
}

static int __init ksync_bug_init(void)
{
	pr_warn("running intentional bug %d — expect a splat if the debug option is on\n",
		bug);
	switch (bug) {
	case 1:
		bug_sleep_in_atomic();
		break;
	case 2:
		bug_abba();
		break;
	case 3:
		bug_softirq_unsafe();
		break;
	default:
		pr_err("bug must be 1, 2 or 3\n");
		return -EINVAL;
	}
	pr_info("bug %d done; now read dmesg above this line\n", bug);
	return 0;
}

static void __exit ksync_bug_exit(void)
{
	pr_info("unloaded\n");
}

module_init(ksync_bug_init);
module_exit(ksync_bug_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("INTENTIONALLY BROKEN locking examples for lockdep / DEBUG_ATOMIC_SLEEP");
