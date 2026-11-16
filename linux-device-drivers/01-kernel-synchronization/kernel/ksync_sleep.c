// SPDX-License-Identifier: GPL-2.0
/*
 * ksync_sleep.c — the SLEEPING toolbox: mutex, semaphore, rw_semaphore,
 *                 completion, wait queue (producer/consumer)
 *
 * Concept : Sleeping locks put the waiter to sleep instead of spinning, so
 *           the holder may itself sleep (kmalloc(GFP_KERNEL), copy_to_user,
 *           msleep, I/O). The price: process context only — never in an
 *           interrupt, softirq, timer callback or under a spinlock.
 *           Completions and wait queues are not locks: they are "sleep until
 *           something happens" — the kernel's condition variables.
 * Build   : make drivers        (or: make -C linux-device-drivers/01-kernel-synchronization/kernel)
 * Run     : sudo insmod ksync_sleep.ko threads=4 iters=100000
 *           dmesg | grep ksync_sleep     -> one PASS line per test
 *           sudo rmmod ksync_sleep
 * Read    : ../README.md  (section 3.4 "Sleeping locks" and 3.5 "Waiting")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#define KSYNC_MAX_THREADS 32

static int threads = 4;
module_param(threads, int, 0444);
MODULE_PARM_DESC(threads, "worker kthreads per test (2..32, default 4)");

static int iters = 100000;
module_param(iters, int, 0444);
MODULE_PARM_DESC(iters, "loop iterations per thread (1000..10000000, default 100000)");

static int sem_limit = 2;
module_param(sem_limit, int, 0444);
MODULE_PARM_DESC(sem_limit, "semaphore count = max threads inside at once (default 2)");

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
/* The runner itself is the completion demo: "start" is a gate opened with  */
/* complete_all(), "done" is counted down with one complete() per worker.   */
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

	/* Park until kthread_stop(): it needs our task_struct alive. */
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

/* Track the maximum of a value that several threads update. */
static void atomic_max(atomic_t *max, int v)
{
	int old = atomic_read(max);

	/* try_cmpxchg reloads "old" on failure, so this loop is short. */
	while (old < v && !atomic_try_cmpxchg(max, &old, v))
		;
}

/* ------------------------------------------------------------------------ */
/* Test 1: mutex-protected linked list                                      */
/* ------------------------------------------------------------------------ */
/*
 *   item_list (head) <-> [id0,v0] <-> [id2,v0] <-> [id0,v1] <-> ...
 *
 * Why a mutex and not a spinlock? The add path calls kmalloc(GFP_KERNEL)
 * which may sleep to reclaim memory. Under a spinlock that is a bug
 * ("BUG: sleeping function called from invalid context"); under a mutex it
 * is fine. Here the allocation is done OUTSIDE the lock anyway (shorter
 * critical section), but the remove path does kfree() inside — both OK.
 */
struct item {
	struct list_head node;
	int owner;
	int value;
};

static LIST_HEAD(item_list);
static DEFINE_MUTEX(item_lock);		/* protects item_list and nr_items */
static int nr_items;

/*
 * __must_hold() is for sparse (make C=1): "caller holds item_lock".
 * lockdep_assert_held() checks the same thing at run time when lockdep is
 * enabled (CONFIG_PROVE_LOCKING) and costs nothing otherwise.
 */
static void item_add_locked(struct item *it)
	__must_hold(&item_lock)
{
	lockdep_assert_held(&item_lock);
	list_add_tail(&it->node, &item_list);
	nr_items++;
}

static void mutex_work(int id)
{
	int per_thread = min(iters, 2000);
	struct item *it, *tmp;
	int i;

	for (i = 0; i < per_thread; i++) {
		/* Owner: the list, after item_add_locked(). Freed below. */
		it = kmalloc(sizeof(*it), GFP_KERNEL);
		if (!it)
			return;
		it->owner = id;
		it->value = i;

		mutex_lock(&item_lock);
		item_add_locked(it);
		mutex_unlock(&item_lock);
	}

	/* Remove every odd-valued item we own; others keep inserting. */
	mutex_lock(&item_lock);
	list_for_each_entry_safe(it, tmp, &item_list, node) {
		if (it->owner == id && (it->value & 1)) {
			list_del(&it->node);
			nr_items--;
			kfree(it);
		}
	}
	mutex_unlock(&item_lock);
}

static void test_mutex_list(void)
{
	int per_thread = min(iters, 2000);
	int expect = threads * ((per_thread + 1) / 2);
	struct item *it, *tmp;
	int walked = 0, odd = 0;

	if (run_threads("mutex", threads, mutex_work))
		goto free_all;

	mutex_lock(&item_lock);
	list_for_each_entry(it, &item_list, node) {
		walked++;
		odd += it->value & 1;
	}
	check(walked == expect && nr_items == expect && !odd, "mutex",
	      "%d threads added %d items each, removed odd ones: %d left, list intact",
	      threads, per_thread, walked);
	mutex_unlock(&item_lock);

free_all:
	/*
	 * All threads are stopped, but take the lock anyway: it is cheap and
	 * keeps the "always under item_lock" rule simple to audit.
	 */
	mutex_lock(&item_lock);
	list_for_each_entry_safe(it, tmp, &item_list, node) {
		list_del(&it->node);
		kfree(it);
	}
	nr_items = 0;
	mutex_unlock(&item_lock);
}

/* ------------------------------------------------------------------------ */
/* Test 2: counting semaphore — at most sem_limit threads inside            */
/* ------------------------------------------------------------------------ */
/*
 *   count = 2       T0 down -> 1     T1 down -> 0     T2 down -> sleeps
 *                   ... T0 up -> wakes T2 ...
 *
 * A mutex is "one owner, the owner unlocks". A semaphore is just a counter:
 * any task may up(), and it may let N in. Today it is mostly legacy; prefer
 * a mutex for mutual exclusion and a completion/waitqueue for signalling.
 */
static struct semaphore pool_sem;
static atomic_t inside = ATOMIC_INIT(0);
static atomic_t max_inside = ATOMIC_INIT(0);

static void sem_work(int id)
{
	int i, rounds = max(iters / 2000, 20);

	for (i = 0; i < rounds; i++) {
		/*
		 * down() is uninterruptible; drivers usually want
		 * down_interruptible() so a signal can cancel the wait.
		 */
		down(&pool_sem);
		atomic_max(&max_inside, atomic_inc_return(&inside));
		usleep_range(50, 100);	/* "use the resource" — sleeping is OK */
		atomic_dec(&inside);
		up(&pool_sem);
	}
}

static void test_semaphore(void)
{
	int i, got = 0, trylock_failed;

	sema_init(&pool_sem, sem_limit);
	if (run_threads("sem", threads, sem_work))
		return;

	/*
	 * Take every unit, then down_trylock() must fail.
	 * WATCH OUT: down_trylock() returns 0 on SUCCESS (the opposite of
	 * mutex_trylock()/spin_trylock(), which return 1 on success).
	 */
	for (i = 0; i < sem_limit; i++)
		got += !down_trylock(&pool_sem);
	trylock_failed = down_trylock(&pool_sem);
	for (i = 0; i < got; i++)
		up(&pool_sem);

	check(atomic_read(&max_inside) <= sem_limit && got == sem_limit && trylock_failed,
	      "semaphore", "limit %d, max seen inside %d, down_trylock() on empty = %d (1 = busy)",
	      sem_limit, atomic_read(&max_inside), trylock_failed);
}

/* ------------------------------------------------------------------------ */
/* Test 3: rw_semaphore — many readers OR one writer                        */
/* ------------------------------------------------------------------------ */
/*
 * Thread 0 is the writer: it sets cfg_a, SLEEPS, then sets cfg_b. A reader
 * must never see cfg_a != cfg_b. Readers sleep inside too, so several of them
 * are inside at the same time (max_readers > 1 shows the sharing).
 */
static DECLARE_RWSEM(cfg_rwsem);
static int cfg_a, cfg_b;		/* protected by cfg_rwsem */
static atomic_t readers_in = ATOMIC_INIT(0);
static atomic_t max_readers = ATOMIC_INIT(0);
static atomic_t rw_bad = ATOMIC_INIT(0);

static void rwsem_work(int id)
{
	int i, rounds = max(iters / 2000, 20);

	for (i = 0; i < rounds; i++) {
		if (id == 0) {
			down_write(&cfg_rwsem);
			if (atomic_read(&readers_in))
				atomic_inc(&rw_bad);	/* reader inside with us! */
			cfg_a = i;
			usleep_range(20, 40);		/* window for a torn read */
			cfg_b = i;
			up_write(&cfg_rwsem);
			usleep_range(100, 200);		/* give readers a turn */
		} else {
			down_read(&cfg_rwsem);
			atomic_max(&max_readers, atomic_inc_return(&readers_in));
			if (cfg_a != cfg_b)
				atomic_inc(&rw_bad);
			usleep_range(20, 40);
			atomic_dec(&readers_in);
			up_read(&cfg_rwsem);
		}
	}
}

static void test_rwsem(void)
{
	if (run_threads("rwsem", threads, rwsem_work))
		return;
	check(!atomic_read(&rw_bad), "rw_semaphore",
	      "1 writer + %d readers, max %d readers inside together, 0 torn reads",
	      threads - 1, atomic_read(&max_readers));
}

/* ------------------------------------------------------------------------ */
/* Test 4: completion — "wait until that one thing has happened"            */
/* ------------------------------------------------------------------------ */
/*
 * Typical driver use: probe() starts firmware loading or a self-test and
 * waits with a timeout; the IRQ handler / worker calls complete().
 */
static DECLARE_COMPLETION(fw_ready);

static int fake_fw_loader(void *unused)
{
	msleep(20);			/* "load the firmware" */
	/*
	 * complete() + exit in one call that never returns into this module.
	 * With "complete(); return 0;" the waiter could finish, rmmod could
	 * free our code, and this thread would still be executing "return".
	 * (Elsewhere complete() is commonly called from an IRQ handler.)
	 */
	kthread_complete_and_exit(&fw_ready, 0);
}

static void test_completion(void)
{
	struct task_struct *t;
	unsigned long left;

	reinit_completion(&fw_ready);
	t = kthread_run(fake_fw_loader, NULL, "ksync/fw");
	if (IS_ERR(t)) {
		check(false, "completion", "kthread_run: %ld", PTR_ERR(t));
		return;
	}
	/* Returns 0 on timeout, else the jiffies that were left. */
	left = wait_for_completion_timeout(&fw_ready, msecs_to_jiffies(2000));
	check(left > 0, "completion",
	      "woken by complete() after ~%u ms (timeout was 2000 ms)",
	      2000 - jiffies_to_msecs(left));
}

/* ------------------------------------------------------------------------ */
/* Test 5: wait queues — bounded buffer, producers and consumers            */
/* ------------------------------------------------------------------------ */
/*
 *   producers ---push--->  [ ring of 8 ]  ---pop--->  consumers
 *      sleep on not_full when full          sleep on not_empty when empty
 *      wake_up(not_empty) after push        wake_up(not_full) after pop
 *
 * wait_event_timeout(wq, cond, t) is the kernel's "while (!cond) wait":
 *   loop { prepare_to_wait(); if (cond) break; schedule_timeout(); }
 * The condition is re-checked after every wake-up, so spurious or "someone
 * else got it first" wake-ups are harmless. Here the condition itself tries
 * to push/pop under the spinlock and is true only if it succeeded.
 *
 * Termination: the last producer to finish pushes one 0 ("poison pill") per
 * consumer. Real items are always > 0.
 */
#define RING_SIZE 8

static struct {
	spinlock_t lock;		/* protects buf/head/count */
	int buf[RING_SIZE];
	unsigned int head, count;
	wait_queue_head_t not_empty, not_full;
} ring;

static int nr_producers, nr_consumers;
static atomic_t producers_left;
static atomic64_t sum_produced, sum_consumed;
static atomic_t wq_timeouts = ATOMIC_INIT(0);

static bool ring_try_push(int v)
{
	bool ok = false;

	spin_lock(&ring.lock);
	if (ring.count < RING_SIZE) {
		ring.buf[(ring.head + ring.count) % RING_SIZE] = v;
		ring.count++;
		ok = true;
	}
	spin_unlock(&ring.lock);
	return ok;
}

static bool ring_try_pop(int *v)
{
	bool ok = false;

	spin_lock(&ring.lock);
	if (ring.count) {
		*v = ring.buf[ring.head];
		ring.head = (ring.head + 1) % RING_SIZE;
		ring.count--;
		ok = true;
	}
	spin_unlock(&ring.lock);
	return ok;
}

static bool push_wait(int v)
{
	if (!wait_event_timeout(ring.not_full, ring_try_push(v), 5 * HZ)) {
		atomic_inc(&wq_timeouts);
		return false;
	}
	wake_up(&ring.not_empty);
	return true;
}

static void wq_work(int id)
{
	int per_producer = min(iters, 20000);
	int i, v;

	if (id < nr_producers) {
		for (i = 1; i <= per_producer; i++) {
			v = id * per_producer + i;	/* > 0, unique */
			if (!push_wait(v))
				break;
			atomic64_add(v, &sum_produced);
		}
		if (atomic_dec_and_test(&producers_left))
			for (i = 0; i < nr_consumers; i++)
				push_wait(0);		/* poison pills */
		return;
	}

	for (;;) {
		if (!wait_event_timeout(ring.not_empty, ring_try_pop(&v), 5 * HZ)) {
			atomic_inc(&wq_timeouts);
			return;
		}
		wake_up(&ring.not_full);
		if (v == 0)
			return;
		atomic64_add(v, &sum_consumed);
	}
}

static void test_waitqueue(void)
{
	spin_lock_init(&ring.lock);
	init_waitqueue_head(&ring.not_empty);
	init_waitqueue_head(&ring.not_full);
	ring.head = 0;
	ring.count = 0;
	nr_producers = threads / 2;
	nr_consumers = threads - nr_producers;
	atomic_set(&producers_left, nr_producers);

	if (run_threads("wq", threads, wq_work))
		return;
	check(!atomic_read(&wq_timeouts) && ring.count == 0 &&
	      atomic64_read(&sum_produced) == atomic64_read(&sum_consumed),
	      "waitqueue", "%d producers -> %d consumers through %d slots, sum %lld == %lld",
	      nr_producers, nr_consumers, RING_SIZE,
	      atomic64_read(&sum_produced), atomic64_read(&sum_consumed));
}

/* ------------------------------------------------------------------------ */

static int __init ksync_sleep_init(void)
{
	if (threads < 2 || threads > KSYNC_MAX_THREADS ||
	    iters < 1000 || iters > 10000000 || sem_limit < 1) {
		pr_err("threads must be 2..%d, iters 1000..10000000, sem_limit >= 1\n",
		       KSYNC_MAX_THREADS);
		return -EINVAL;
	}
	pr_info("start: %d threads x %d iterations\n", threads, iters);

	/* We are in process context (insmod's finit_module syscall): may sleep. */
	might_sleep();

	test_mutex_list();
	test_semaphore();
	test_rwsem();
	test_completion();
	test_waitqueue();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");
	return 0;
}

static void __exit ksync_sleep_exit(void)
{
	pr_info("unloaded\n");
}

module_init(ksync_sleep_init);
module_exit(ksync_sleep_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: mutex, semaphore, rw_semaphore, completion, wait queues");
