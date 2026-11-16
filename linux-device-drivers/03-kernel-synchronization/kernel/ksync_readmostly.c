// SPDX-License-Identifier: GPL-2.0
/*
 * ksync_readmostly.c — read-mostly data: seqlock_t and RCU
 *
 * Concept : When reads vastly outnumber writes, make readers cheap.
 *           seqlock : readers take NO lock, they read a sequence number,
 *                     copy the data, and retry if a writer was active.
 *                     (jiffies_64 / timekeeping work like this.)
 *           RCU     : readers take no lock and never retry; the writer
 *                     publishes a NEW copy and frees the old one only after
 *                     every reader that could see it has finished
 *                     (a "grace period").
 * Build   : make drivers        (or: make -C linux-device-drivers/03-kernel-synchronization/kernel)
 * Run     : sudo insmod ksync_readmostly.ko threads=4 iters=100000
 *           dmesg | grep ksync_readmostly   -> PASS seqlock, PASS RCU
 *           sudo rmmod ksync_readmostly
 * Read    : ../README.md  (section 3.6 "Read-mostly data: seqlock and RCU")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seqlock.h>
#include <linux/slab.h>

#define KSYNC_MAX_THREADS 32

static int threads = 4;
module_param(threads, int, 0444);
MODULE_PARM_DESC(threads, "worker kthreads per test: 1 writer + (threads-1) readers (2..32, default 4)");

static int iters = 100000;
module_param(iters, int, 0444);
MODULE_PARM_DESC(iters, "writer updates (seqlock) per test (1000..10000000, default 100000)");

static int nr_fail;

static __printf(3, 4) void check(bool ok, const char *test, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %-14s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-14s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ------------------------------------------------------------------------ */
/* Thread runner (same as in ksync_spin.c — each module is standalone).     */
/* ------------------------------------------------------------------------ */
struct runner {
	void (*work)(int id);
	struct task_struct *task[KSYNC_MAX_THREADS];
	int nr_started;
	atomic_t next_id;
	bool abort;
	struct completion start;
	struct completion done;
};

static struct runner runner;

static int runner_fn(void *unused)
{
	int id = atomic_inc_return(&runner.next_id) - 1;

	wait_for_completion(&runner.start);
	if (!READ_ONCE(runner.abort))
		runner.work(id);
	complete(&runner.done);

	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static int run_threads(const char *name, int nr, void (*work)(int id))
{
	int i, err = 0;

	runner.work = work;
	runner.nr_started = 0;
	runner.abort = false;
	atomic_set(&runner.next_id, 0);
	init_completion(&runner.start);
	init_completion(&runner.done);

	for (i = 0; i < nr; i++) {
		struct task_struct *t;

		t = kthread_run(runner_fn, NULL, "ksync/%s%d", name, i);
		if (IS_ERR(t)) {
			err = PTR_ERR(t);
			pr_err("kthread_run(%s%d) failed: %d\n", name, i, err);
			WRITE_ONCE(runner.abort, true);
			break;
		}
		runner.task[runner.nr_started++] = t;
	}

	complete_all(&runner.start);
	if (!err)
		for (i = 0; i < nr; i++)
			wait_for_completion(&runner.done);

	for (i = 0; i < runner.nr_started; i++)
		kthread_stop(runner.task[i]);
	return err;
}

/* ------------------------------------------------------------------------ */
/* Test 1: seqlock_t                                                        */
/* ------------------------------------------------------------------------ */
/*
 *   seq:   0 (even)   1 (odd: write in progress)   2 (even)
 *   writer:        write_seqlock: seq++ ... update ... write_sequnlock: seq++
 *   reader: s = read_seqbegin()   (waits while odd)
 *           copy fields
 *           read_seqretry(s)?     -> seq changed -> copy again
 *
 * Readers never block the writer — the writer can starve readers instead,
 * never the other way round. Readers must only COPY: never follow a pointer
 * read inside a seqlock section (it may point at freed memory).
 */
static DEFINE_SEQLOCK(clock_lock);
static struct {
	u64 sec;
	u64 nsec;
	u64 check;		/* always sec * 1000 + nsec: the invariant */
} fake_clock;			/* written under clock_lock */

static bool writer_done;
static atomic_t seq_bad = ATOMIC_INIT(0);
static atomic64_t seq_reads, seq_retries;

static void seqlock_work(int id)
{
	u64 sec, nsec, chk, reads = 0, retries = 0;
	unsigned int seq;
	int i;

	if (id == 0) {			/* the single writer ("timer tick") */
		for (i = 1; i <= iters; i++) {
			write_seqlock(&clock_lock);
			fake_clock.nsec++;
			if (fake_clock.nsec == 1000) {
				fake_clock.nsec = 0;
				fake_clock.sec++;
			}
			fake_clock.check = fake_clock.sec * 1000 + fake_clock.nsec;
			write_sequnlock(&clock_lock);
			if ((i & 1023) == 0)
				cond_resched();
		}
		/* release: pairs with the readers' acquire below */
		smp_store_release(&writer_done, true);
		return;
	}

	/* acquire: pairs with the writer's release above */
	while (!smp_load_acquire(&writer_done)) {
		bool first = true;

		do {
			if (!first)
				retries++;
			first = false;
			seq = read_seqbegin(&clock_lock);
			sec = fake_clock.sec;
			nsec = fake_clock.nsec;
			chk = fake_clock.check;
		} while (read_seqretry(&clock_lock, seq));

		/* Only now, after a successful retry check, use the copy. */
		if (chk != sec * 1000 + nsec)
			atomic_inc(&seq_bad);
		if ((++reads & 1023) == 0)
			cond_resched();
	}
	atomic64_add(reads, &seq_reads);
	atomic64_add(retries, &seq_retries);
}

static void test_seqlock(void)
{
	if (run_threads("seq", threads, seqlock_work))
		return;
	check(!atomic_read(&seq_bad) && fake_clock.check == (u64)iters,
	      "seqlock", "%d writes, %lld lock-free reads, %lld retries, 0 torn copies",
	      iters, atomic64_read(&seq_reads), atomic64_read(&seq_retries));
}

/* ------------------------------------------------------------------------ */
/* Test 2: RCU-protected pointer swap                                        */
/* ------------------------------------------------------------------------ */
/*
 *   cur_cfg --> [v1]                     readers inside rcu_read_lock()
 *                                         may still be looking at v1
 *   writer: new = kmalloc; fill v2
 *           rcu_assign_pointer / rcu_replace_pointer(cur_cfg, new)
 *   cur_cfg --> [v2]         [v1] <- old: unreachable for NEW readers
 *           synchronize_rcu()   <- wait until every pre-existing reader left
 *           kfree(v1)           <- now nobody can hold it
 *
 * Three ways to free the old copy are rotated:
 *   synchronize_rcu() + kfree   sleeps a grace period (milliseconds)
 *   call_rcu(cb)                returns at once, cb runs after the GP
 *   kfree_rcu(p, rcu)           same, without writing a callback
 *
 * Before freeing, the old copy is POISONED. If RCU did not protect readers,
 * one would see magic != MAGIC and the test fails.
 */
#define CFG_MAGIC 0x5eedf00dU
#define CFG_POISON 0xdeadbeefU

struct cfg {
	u32 magic;
	u32 pad;
	u64 version;
	u64 check;		/* version * 3 */
	struct rcu_head rcu;
};

static struct cfg __rcu *cur_cfg;
static DEFINE_MUTEX(cfg_update_lock);	/* serialises WRITERS only */
static bool rcu_writer_done;
static atomic_t rcu_bad = ATOMIC_INIT(0);
static atomic64_t rcu_reads;
static int nr_sync, nr_call, nr_kfree_rcu;

static void cfg_poison(struct cfg *c)
{
	WRITE_ONCE(c->magic, CFG_POISON);
	WRITE_ONCE(c->check, 0);
}

/* Runs in softirq (or rcuc kthread) context after a grace period. */
static void cfg_free_cb(struct rcu_head *head)
{
	struct cfg *c = container_of(head, struct cfg, rcu);

	cfg_poison(c);
	kfree(c);
}

static int cfg_publish(u64 version)
{
	struct cfg *new, *old;

	new = kmalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return -ENOMEM;
	new->magic = CFG_MAGIC;
	new->pad = 0;
	new->version = version;
	new->check = version * 3;

	mutex_lock(&cfg_update_lock);
	/*
	 * rcu_replace_pointer = rcu_assign_pointer + return the old value.
	 * The release barrier inside makes the fields above visible BEFORE the
	 * pointer. The lockdep expression documents which lock protects it.
	 */
	old = rcu_replace_pointer(cur_cfg, new,
				  lockdep_is_held(&cfg_update_lock));
	mutex_unlock(&cfg_update_lock);

	if (!old)
		return 0;
	switch (version % 3) {
	case 0:
		synchronize_rcu();	/* sleeps until all old readers are out */
		cfg_poison(old);
		kfree(old);
		nr_sync++;
		break;
	case 1:
		call_rcu(&old->rcu, cfg_free_cb);
		nr_call++;
		break;
	default:
		kfree_rcu(old, rcu);
		nr_kfree_rcu++;
		break;
	}
	return 0;
}

static void rcu_work(int id)
{
	u64 v, last = 0, reads = 0;
	struct cfg *c;
	int i, updates = max(iters / 500, 30);

	if (id == 0) {				/* the writer */
		for (i = 1; i <= updates; i++) {
			if (cfg_publish(i + 1))
				atomic_inc(&rcu_bad);
			usleep_range(200, 400);
		}
		/* release: pairs with the readers' acquire below */
		smp_store_release(&rcu_writer_done, true);
		return;
	}

	/* acquire: pairs with the writer's release above */
	while (!smp_load_acquire(&rcu_writer_done)) {
		rcu_read_lock();		/* no sleeping until unlock */
		c = rcu_dereference(cur_cfg);	/* dependency-ordered load */
		v = READ_ONCE(c->version);
		if (READ_ONCE(c->magic) != CFG_MAGIC ||
		    READ_ONCE(c->check) != v * 3 || v < last)
			atomic_inc(&rcu_bad);
		last = v;	/* versions seen by one reader never go back */
		rcu_read_unlock();
		if ((++reads & 255) == 0)
			cond_resched();		/* NOT inside the read section */
	}
	atomic64_add(reads, &rcu_reads);
}

static void test_rcu(void)
{
	struct cfg *last;
	int err;

	if (cfg_publish(1)) {			/* initial version */
		check(false, "RCU", "kmalloc failed");
		return;
	}
	err = run_threads("rcu", threads, rcu_work);

	/* Teardown: unpublish, wait for readers, free. */
	mutex_lock(&cfg_update_lock);
	last = rcu_replace_pointer(cur_cfg, NULL,
				   lockdep_is_held(&cfg_update_lock));
	mutex_unlock(&cfg_update_lock);
	synchronize_rcu();
	kfree(last);
	/*
	 * rcu_barrier(): wait for every call_rcu() callback already queued.
	 * Without it, rmmod could free cfg_free_cb's code while a callback is
	 * still pending -> the CPU later jumps into freed memory.
	 */
	rcu_barrier();
	if (err)
		return;

	check(!atomic_read(&rcu_bad), "RCU",
	      "%lld lock-free reads, %d swaps (synchronize_rcu %d, call_rcu %d, kfree_rcu %d), 0 stale/poisoned",
	      atomic64_read(&rcu_reads), nr_sync + nr_call + nr_kfree_rcu,
	      nr_sync, nr_call, nr_kfree_rcu);
}

/* ------------------------------------------------------------------------ */

static int __init ksync_readmostly_init(void)
{
	if (threads < 2 || threads > KSYNC_MAX_THREADS ||
	    iters < 1000 || iters > 10000000) {
		pr_err("threads must be 2..%d and iters 1000..10000000\n",
		       KSYNC_MAX_THREADS);
		return -EINVAL;
	}
	pr_info("start: 1 writer + %d readers\n", threads - 1);

	test_seqlock();
	test_rcu();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");
	return 0;
}

static void __exit ksync_readmostly_exit(void)
{
	/* test_rcu() already did rcu_barrier(); nothing is pending. */
	pr_info("unloaded\n");
}

module_init(ksync_readmostly_init);
module_exit(ksync_readmostly_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: seqlock_t and RCU (rcu_assign_pointer, synchronize_rcu, call_rcu, kfree_rcu)");
