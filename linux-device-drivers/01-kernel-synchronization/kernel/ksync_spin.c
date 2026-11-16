// SPDX-License-Identifier: GPL-2.0
/*
 * ksync_spin.c — the NON-sleeping toolbox: atomics, bitops, refcount_t,
 *                release/acquire, per-CPU data, spinlocks (_bh, _irqsave, raw)
 *
 * Concept : Every test starts N kthreads that hammer one shared object at the
 *           same time and then checks the result. A plain int loses updates,
 *           atomic_t and a spinlock do not. Data shared with a timer
 *           (softirq) needs spin_lock_bh() in process context; data shared
 *           with a hard interrupt handler needs spin_lock_irqsave() — and a
 *           raw_spinlock_t, so it still works on PREEMPT_RT (the board).
 * Build   : make drivers        (or: make -C linux-device-drivers/01-kernel-synchronization/kernel)
 * Run     : sudo insmod ksync_spin.ko threads=4 iters=100000
 *           dmesg | grep ksync_spin      -> one PASS line per test
 *           sudo rmmod ksync_spin
 *           (on the board or in a VM — never on a machine you care about)
 * Read    : ../README.md  (sections 3.2 "Atomics, bitops, barriers" and
 *           3.3 "Spinlocks")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/irq_work.h>
#include <linux/kcsan-checks.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>

#define KSYNC_MAX_THREADS 32

static int threads = 4;
module_param(threads, int, 0444);
MODULE_PARM_DESC(threads, "worker kthreads per test (2..32, default 4)");

static int iters = 100000;
module_param(iters, int, 0444);
MODULE_PARM_DESC(iters, "loop iterations per thread (1000..10000000, default 100000)");

static int nr_fail;

/* One "PASS name: details" or "FAIL name: details" line per check. */
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
/* Thread runner: start N kthreads at the same moment, wait until all are    */
/* done, then kthread_stop() each one.                                       */
/* ------------------------------------------------------------------------ */
/*
 *   insmod (process context)          kthread 0..N-1
 *   -------------------------         ------------------------------
 *   kthread_run() x N  ------------>  wait_for_completion(&start)
 *   complete_all(&start) ---------->  work(id)            <- all at once
 *   wait_for_completion(&done) x N <- complete(&done)
 *   kthread_stop() x N ------------>  sleeps until kthread_should_stop()
 *
 * Why park instead of just returning? kthread_stop() needs the task_struct
 * to still exist. A kthread that returns on its own may already be freed
 * when kthread_stop() looks at it (use-after-free). Parking until stop is
 * the simplest correct pattern.
 */
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

	/* Park. set_current_state() BEFORE the check: no lost wake-up. */
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
			/* Threads already started must skip the work. */
			WRITE_ONCE(runner.abort, true);
			break;
		}
		runner.task[runner.nr_started++] = t;
	}

	/* Release everybody (also on error, or they would wait forever). */
	complete_all(&runner.start);
	if (!err)
		for (i = 0; i < nr; i++)
			wait_for_completion(&runner.done);

	for (i = 0; i < runner.nr_started; i++)
		kthread_stop(runner.task[i]);
	return err;
}

/* ------------------------------------------------------------------------ */
/* Test 1: plain int vs atomic_t vs spinlock-protected counter              */
/* ------------------------------------------------------------------------ */
/*
 *   counter++ is THREE steps: load, add, store.
 *
 *   CPU0: load 41 ......... add -> 42 .. store 42
 *   CPU1:      load 41 .. add -> 42 ........... store 42   <- one +1 lost
 *
 * atomic_inc() does the read-modify-write as one indivisible operation
 * (arm64: LDXR/STXR retry loop, or one LDADD with LSE atomics; x86: LOCK XADD).
 * A spinlock makes the three steps a critical section instead.
 */
static int plain_counter;
static atomic_t atomic_counter = ATOMIC_INIT(0);
static atomic64_t atomic64_counter = ATOMIC64_INIT(0);
static DEFINE_SPINLOCK(counter_lock);
static unsigned long locked_counter;	/* protected by counter_lock */

static void plain_work(int id)
{
	int i;

	for (i = 0; i < iters; i++) {
		/*
		 * READ_ONCE/WRITE_ONCE stop the compiler from turning the loop
		 * into one "plain_counter += iters" — we want one load and one
		 * store per iteration so the race is visible. They do NOT make
		 * the increment atomic. Under KCSAN this assertion reports the
		 * race (no-op otherwise).
		 */
		ASSERT_EXCLUSIVE_WRITER(plain_counter);
		WRITE_ONCE(plain_counter, READ_ONCE(plain_counter) + 1);
		if ((i & 1023) == 0)
			cond_resched();	/* be nice on non-preemptible kernels */
	}
}

static void atomic_work(int id)
{
	int i;

	for (i = 0; i < iters; i++) {
		atomic_inc(&atomic_counter);
		atomic64_add(2, &atomic64_counter);
		if ((i & 1023) == 0)
			cond_resched();
	}
}

static void locked_work(int id)
{
	int i;

	for (i = 0; i < iters; i++) {
		/*
		 * Plain spin_lock(): only kthreads (process context) touch
		 * locked_counter, no interrupt handler does, so there is no
		 * reason to disable interrupts or bottom halves.
		 */
		spin_lock(&counter_lock);
		locked_counter++;
		spin_unlock(&counter_lock);
		if ((i & 1023) == 0)
			cond_resched();
	}
}

static u64 time_threads(const char *name, void (*work)(int id))
{
	ktime_t t0 = ktime_get();

	if (run_threads(name, threads, work))
		return 0;
	return ktime_to_ns(ktime_sub(ktime_get(), t0));
}

static void test_counters(void)
{
	long expect = (long)threads * iters;
	u64 ns_plain, ns_atomic, ns_locked;
	int plain;

	ns_plain = time_threads("plain", plain_work);
	ns_atomic = time_threads("atomic", atomic_work);
	ns_locked = time_threads("locked", locked_work);

	/* All threads are stopped: plain reads are fine from here on. */
	plain = READ_ONCE(plain_counter);
	pr_info("INFO plain int       %d of %ld, lost %ld updates (expected on SMP, not a failure) %llu ms\n",
		plain, expect, expect - plain, ns_plain / NSEC_PER_MSEC);
	check(atomic_read(&atomic_counter) == expect, "atomic_t",
	      "%d of %ld, %llu ms", atomic_read(&atomic_counter), expect,
	      ns_atomic / NSEC_PER_MSEC);
	check(atomic64_read(&atomic64_counter) == 2 * expect, "atomic64_t",
	      "%lld of %ld", atomic64_read(&atomic64_counter), 2 * expect);
	check(locked_counter == expect, "spinlock",
	      "%lu of %ld, %llu ms", locked_counter, expect,
	      ns_locked / NSEC_PER_MSEC);
}

/* ------------------------------------------------------------------------ */
/* Test 2: bitops — test_and_set_bit() as "claim a free slot"               */
/* ------------------------------------------------------------------------ */
/*
 * Every thread walks all slots and tries to claim each one. test_and_set_bit()
 * is an atomic RMW that returns the OLD bit, so exactly one thread sees 0 for
 * each slot. This is how IDs, minor numbers, DMA channels... are handed out.
 */
#define NR_SLOTS 4096
static DECLARE_BITMAP(slots, NR_SLOTS);
static atomic_t slots_won = ATOMIC_INIT(0);

static void bitops_work(int id)
{
	int i, n;

	/* Start at a different place per thread so they collide in the middle. */
	for (n = 0; n < NR_SLOTS; n++) {
		i = (n + id * (NR_SLOTS / KSYNC_MAX_THREADS)) % NR_SLOTS;
		if (!test_and_set_bit(i, slots))
			atomic_inc(&slots_won);
	}
}

static void test_bitops(void)
{
	if (run_threads("bits", threads, bitops_work))
		return;
	check(atomic_read(&slots_won) == NR_SLOTS && bitmap_full(slots, NR_SLOTS),
	      "bitops", "%d slots claimed exactly once each by %d threads",
	      atomic_read(&slots_won), threads);
}

/* ------------------------------------------------------------------------ */
/* Test 3: refcount_t — the object lives while anyone holds a reference     */
/* ------------------------------------------------------------------------ */
struct ksync_obj {
	refcount_t ref;
	int payload;
};

static struct ksync_obj *shared_obj;

static void refcount_work(int id)
{
	int i;

	for (i = 0; i < iters; i++) {
		/* "get" before using, "put" after. Never drops to 0 here. */
		refcount_inc(&shared_obj->ref);
		WARN_ON_ONCE(READ_ONCE(shared_obj->payload) != 42);
		/* refcount_dec() WARNs if it would hit 0: use _and_test for the last put */
		refcount_dec(&shared_obj->ref);
		if ((i & 1023) == 0)
			cond_resched();
	}
}

static void test_refcount(void)
{
	unsigned int left;
	bool freed;

	/* Owner: this function. kmalloc here, kfree on the last put below. */
	shared_obj = kmalloc(sizeof(*shared_obj), GFP_KERNEL);
	if (!shared_obj) {
		check(false, "refcount_t", "kmalloc failed");
		return;
	}
	refcount_set(&shared_obj->ref, 1);	/* the creator's reference */
	shared_obj->payload = 42;

	if (run_threads("ref", threads, refcount_work)) {
		kfree(shared_obj);
		return;
	}
	left = refcount_read(&shared_obj->ref);
	freed = refcount_dec_and_test(&shared_obj->ref);
	if (freed)
		kfree(shared_obj);	/* last reference gone -> free */
	shared_obj = NULL;
	check(left == 1 && freed, "refcount_t",
	      "%d threads x %d get/put, count back to %u, last put freed the object",
	      threads, iters, left);
}

/* ------------------------------------------------------------------------ */
/* Test 4: message passing with smp_store_release / smp_load_acquire        */
/* ------------------------------------------------------------------------ */
/*
 *   producer (id 0)                       consumer (id 1)
 *   payload[0..3] = f(seq)                while (load_acquire(&msg_seq) != seq) ;
 *   store_release(&msg_seq, seq) -------> read payload[0..3]   <- must see f(seq)
 *   while (load_acquire(&ack) != seq) <-- store_release(&ack, seq)
 *
 * Release: all my earlier writes are visible before this store.
 * Acquire: none of my later reads happen before this load.
 * arm64: STLR / LDAR instructions. Use plain WRITE_ONCE for msg_seq and the
 * consumer may see the new seq but an OLD payload on ARM (not on x86).
 */
#define MSG_ROUNDS 10000
static u64 msg_payload[4];
static unsigned long msg_seq, ack_seq;
static atomic_t msg_bad = ATOMIC_INIT(0);

/*
 * Busy-wait helper. Mostly cpu_relax() (arm64: YIELD), but every 1024 spins
 * sleep briefly: if both threads landed on the same CPU, the one we wait for
 * needs this CPU to make progress.
 */
static void relax(unsigned int *spins)
{
	if (++*spins & 1023)
		cpu_relax();
	else
		usleep_range(10, 20);
}

static void msg_work(int id)
{
	unsigned int k, spins = 0;
	unsigned long seq;

	if (id > 1)
		return;		/* only one producer and one consumer */

	for (seq = 1; seq <= MSG_ROUNDS; seq++) {
		if (id == 0) {
			for (k = 0; k < ARRAY_SIZE(msg_payload); k++)
				msg_payload[k] = seq * (k + 1);
			/* release: payload writes above become visible first */
			smp_store_release(&msg_seq, seq);
			/* acquire: pairs with the consumer's release of ack_seq */
			while (smp_load_acquire(&ack_seq) != seq)
				relax(&spins);
		} else {
			/* acquire: pairs with the producer's release of msg_seq */
			while (smp_load_acquire(&msg_seq) != seq)
				relax(&spins);
			for (k = 0; k < ARRAY_SIZE(msg_payload); k++)
				if (msg_payload[k] != seq * (k + 1))
					atomic_inc(&msg_bad);
			/* release: our payload reads are done before the ack */
			smp_store_release(&ack_seq, seq);
		}
	}
}

static void test_release_acquire(void)
{
	if (run_threads("msg", threads, msg_work))
		return;
	check(atomic_read(&msg_bad) == 0 && msg_seq == MSG_ROUNDS,
	      "release/acq", "%d messages, %d stale payload words",
	      MSG_ROUNDS, atomic_read(&msg_bad));
}

/* ------------------------------------------------------------------------ */
/* Test 5: per-CPU data — no sharing, so no lock and no atomic needed        */
/* ------------------------------------------------------------------------ */
/*
 *     CPU0          CPU1          CPU2          CPU3
 *   [hits=..]     [hits=..]     [hits=..]     [hits=..]    one copy per CPU
 *       \_____________\_____________\_____________/
 *                    sum = for_each_possible_cpu(...)      (slow, rare)
 *
 * this_cpu_inc() is one instruction on x86 (gs-relative inc) and is safe
 * against preemption by itself. A multi-step update (a and b together) must
 * stay on one CPU: get_cpu_ptr() disables preemption until put_cpu_ptr().
 */
struct pcpu_pair {
	u64 a;
	u64 b;
};

static DEFINE_PER_CPU(unsigned long, pcpu_hits);
static DEFINE_PER_CPU(struct pcpu_pair, pcpu_pair);

static void percpu_work(int id)
{
	struct pcpu_pair *p;
	int i;

	for (i = 0; i < iters; i++) {
		this_cpu_inc(pcpu_hits);

		p = get_cpu_ptr(&pcpu_pair);	/* preempt_disable() */
		p->a++;
		p->b += 2;
		put_cpu_ptr(&pcpu_pair);	/* preempt_enable()  */

		if ((i & 1023) == 0)
			cond_resched();
	}
}

static void test_percpu(void)
{
	unsigned long hits = 0;
	u64 a = 0, b = 0;
	int cpu, used = 0;

	if (run_threads("pcpu", threads, percpu_work))
		return;
	for_each_possible_cpu(cpu) {
		hits += per_cpu(pcpu_hits, cpu);
		a += per_cpu(pcpu_pair, cpu).a;
		b += per_cpu(pcpu_pair, cpu).b;
		used += per_cpu(pcpu_hits, cpu) != 0;
	}
	check(hits == (unsigned long)threads * iters && b == 2 * a && a == hits,
	      "per-CPU", "sum over %d CPUs used = %lu, no lock taken", used, hits);
}

/* ------------------------------------------------------------------------ */
/* Test 6: data shared with a timer (softirq) -> spin_lock_bh()             */
/* ------------------------------------------------------------------------ */
/*
 * Invariant protected by pair.lock: b == 2 * a.
 *
 * Why _bh? A timer_list callback runs in SOFTIRQ context. With plain
 * spin_lock() in the kthread, this can happen on ONE CPU:
 *
 *   kthread: spin_lock(&pair.lock)  -- holds it
 *     <timer softirq runs on the same CPU, on the way out of an IRQ>
 *     tick_fn: spin_lock(&pair.lock)  -- spins forever: the holder cannot
 *                                        run until the softirq returns
 *
 * spin_lock_bh() disables softirqs on this CPU while the lock is held, so the
 * timer waits (or runs on another CPU and just spins briefly). spin_lock_irq*
 * would also work but needlessly blocks hard interrupts too. Inside the
 * callback plain spin_lock() is enough: softirqs do not nest on a CPU and no
 * hardirq handler takes pair.lock.
 */
static struct {
	spinlock_t lock;		/* protects a, b, timer_updates */
	u64 a, b;
	u64 timer_updates;
	bool in_softirq, in_task;	/* what the callback saw, for the log */
} pair = { .lock = __SPIN_LOCK_UNLOCKED(pair.lock) };

static struct timer_list tick;
static atomic_t pair_bad = ATOMIC_INIT(0);

static void tick_fn(struct timer_list *t)
{
	spin_lock(&pair.lock);
	pair.a++;
	pair.b += 2;
	if (pair.timer_updates++ == 0) {
		pair.in_softirq = in_serving_softirq();
		pair.in_task = in_task();
	}
	spin_unlock(&pair.lock);

	mod_timer(&tick, jiffies + 1);	/* re-arm: fire every jiffy */
}

static void bh_work(int id)
{
	int i;

	for (i = 0; i < iters; i++) {
		spin_lock_bh(&pair.lock);
		if (pair.b != 2 * pair.a)
			atomic_inc(&pair_bad);
		pair.a++;
		pair.b += 2;
		spin_unlock_bh(&pair.lock);	/* pending softirqs may run here */
		if ((i % 1000) == 0)
			usleep_range(500, 1000);	/* let jiffies tick */
	}
}

static void test_timer_bh(void)
{
	u64 expect;
	int err;

	timer_setup(&tick, tick_fn, 0);
	mod_timer(&tick, jiffies + 1);
	err = run_threads("bh", threads, bh_work);
	/*
	 * shutdown_sync: waits for a running callback AND makes the re-arming
	 * mod_timer() in tick_fn a no-op. Plain timer_delete_sync() could lose
	 * the race with a self-re-arming timer.
	 */
	timer_shutdown_sync(&tick);
	if (err)
		return;

	expect = (u64)threads * iters + pair.timer_updates;
	check(pair.a == expect && pair.b == 2 * pair.a && !atomic_read(&pair_bad),
	      "spin_lock_bh", "a=%llu (%llu by timer, in_serving_softirq=%d in_task=%d), b==2a",
	      pair.a, pair.timer_updates, pair.in_softirq, pair.in_task);
}

/* ------------------------------------------------------------------------ */
/* Test 7: data shared with a HARD irq handler -> raw_spin_lock_irqsave()   */
/* ------------------------------------------------------------------------ */
/*
 * irq_work with IRQ_WORK_INIT_HARD runs its callback in hard-interrupt
 * context (a self-IPI) — a stand-in for a device interrupt handler.
 *
 * Why irqsave in the kthread? Same deadlock as above, one level up:
 *
 *   kthread on CPU1: spin_lock(&l)  ----------------------+ holds l
 *     <IRQ arrives on CPU1>                                |
 *     handler on CPU1: spin_lock(&l)  -> spins forever  <-+ holder is the
 *                                                          interrupted code
 *
 * spin_lock_irqsave() disables local IRQs, so the IRQ is delivered only after
 * the unlock. "save" restores the previous IRQ state, so it is safe to call
 * whether IRQs were already off or not.
 *
 * Why raw_spinlock_t? On PREEMPT_RT (the board's kernel!) spinlock_t becomes
 * a sleeping rt_mutex, and you may not sleep in hard-IRQ context. raw_ keeps
 * the real spinning lock. Keep raw sections tiny: they add latency on RT.
 */
static DEFINE_RAW_SPINLOCK(irq_lock);
static u64 irq_a, irq_b, irq_updates;	/* protected by irq_lock */
static bool irq_saw_hardirq;
static atomic_t irq_bad = ATOMIC_INIT(0);

static void irq_fn(struct irq_work *w)
{
	raw_spin_lock(&irq_lock);	/* IRQs are already off in here */
	irq_a++;
	irq_b += 2;
	if (irq_updates++ == 0)
		irq_saw_hardirq = in_hardirq();
	raw_spin_unlock(&irq_lock);
}

static struct irq_work ksync_irq_work = IRQ_WORK_INIT_HARD(irq_fn);

static void irq_work_fn_work(int id)
{
	unsigned long flags;
	int i;

	for (i = 0; i < iters; i++) {
		raw_spin_lock_irqsave(&irq_lock, flags);
		if (irq_b != 2 * irq_a)
			atomic_inc(&irq_bad);
		irq_a++;
		irq_b += 2;
		raw_spin_unlock_irqrestore(&irq_lock, flags);

		/* Raise the "interrupt" now and then (no-op if still pending). */
		if ((i & 63) == 0)
			irq_work_queue(&ksync_irq_work);
		if ((i & 1023) == 0)
			cond_resched();
	}
}

static void test_irqsave(void)
{
	int err = run_threads("irq", threads, irq_work_fn_work);

	irq_work_sync(&ksync_irq_work);	/* no callback may run after this */
	if (err)
		return;
	check(irq_a == (u64)threads * iters + irq_updates && irq_b == 2 * irq_a &&
	      !atomic_read(&irq_bad), "irqsave",
	      "a=%llu (%llu from the hard-IRQ handler, in_hardirq=%d), b==2a held",
	      irq_a, irq_updates, irq_saw_hardirq);
}

/* ------------------------------------------------------------------------ */

static int __init ksync_spin_init(void)
{
	if (threads < 2 || threads > KSYNC_MAX_THREADS ||
	    iters < 1000 || iters > 10000000) {
		pr_err("threads must be 2..%d and iters 1000..10000000\n",
		       KSYNC_MAX_THREADS);
		return -EINVAL;
	}
	pr_info("start: %d threads x %d iterations, %u online CPUs\n",
		threads, iters, num_online_cpus());

	test_counters();
	test_bitops();
	test_refcount();
	test_release_acquire();
	test_percpu();
	test_timer_bh();
	test_irqsave();

	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");
	/* Return 0 even on FAIL so the module can be inspected and rmmod'ed. */
	return 0;
}

static void __exit ksync_spin_exit(void)
{
	/* Everything was torn down at the end of each test; nothing left. */
	pr_info("unloaded\n");
}

module_init(ksync_spin_init);
module_exit(ksync_spin_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Self-test: atomics, bitops, refcount, barriers, per-CPU, spinlocks");
