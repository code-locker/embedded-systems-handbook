// SPDX-License-Identifier: GPL-2.0
/*
 * kprobe_demo.c — count calls to any kernel function with a kprobe, and
 *                 time them / count error returns with a kretprobe
 *
 * Concept : A kprobe replaces the first instruction of a kernel function
 *           with a breakpoint (arm64: BRK, x86: INT3) or, where possible, a
 *           jump through ftrace. When the CPU hits it, your pre_handler runs
 *           with the function's registers, then the original instruction is
 *           single-stepped and execution continues. A kretprobe also hijacks
 *           the return address, so a second handler runs when the function
 *           returns and can read the return value. No recompiling, any
 *           non-inlined, non-blacklisted function. Handlers run in atomic
 *           context: no sleeping, keep them tiny.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel)
 *           Built only when the target kernel has CONFIG_KPROBES.
 * Run     : sudo insmod kprobe_demo.ko [symbol=vfs_read]
 *           sudo cat /sys/kernel/debug/kprobe_demo/stats
 *           sudo rmmod kprobe_demo          (prints the totals)
 * Read    : ../README.md  (section 3.7 "kprobes and eBPF")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/kallsyms.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <linux/string.h>

/*
 * vfs_read: called for every read() on any file, exported, not inlined,
 * stable for many years. Others to try: vfs_write, do_sys_openat2,
 * kernel_clone (fork), schedule (very hot!), i2c_transfer (on the board).
 */
static char *symbol = "vfs_read";
module_param(symbol, charp, 0444);
MODULE_PARM_DESC(symbol, "kernel function to probe (default vfs_read)");

static atomic64_t hits = ATOMIC64_INIT(0);	/* kprobe pre_handler calls */
static atomic64_t rets = ATOMIC64_INIT(0);	/* kretprobe returns */
static atomic64_t errs = ATOMIC64_INIT(0);	/* returns with a negative value */
static atomic64_t total_ns = ATOMIC64_INIT(0);
static atomic64_t max_ns = ATOMIC64_INIT(0);
static struct dentry *kp_dir;

/* ---- kprobe: runs just before the probed instruction -------------------- */
static int kp_pre(struct kprobe *p, struct pt_regs *regs)
{
	/*
	 * Atomic context (preemption off, on some arches IRQs off): no
	 * sleeping, no locks that sleep, no printk in hot paths (it would
	 * flood the log and recurse if you probed something printk uses).
	 * regs holds the arguments: arm64 regs->regs[0..7], x86 di/si/dx...
	 */
	atomic64_inc(&hits);
	return 0;	/* 0 = continue normally (1 = we changed the PC) */
}

static struct kprobe kp = {
	.pre_handler = kp_pre,
};

/* ---- kretprobe: entry + return handlers, per-instance private data ------ */
struct call_data {
	ktime_t t0;
};

static int krp_entry(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct call_data *d = (struct call_data *)ri->data;

	d->t0 = ktime_get();
	return 0;	/* non-zero = don't track this call's return */
}

static int krp_ret(struct kretprobe_instance *ri, struct pt_regs *regs)
{
	struct call_data *d = (struct call_data *)ri->data;
	s64 ns = ktime_to_ns(ktime_sub(ktime_get(), d->t0));
	long ret = (long)regs_return_value(regs);	/* arm64 x0, x86 ax */
	s64 old = atomic64_read(&max_ns);

	atomic64_inc(&rets);
	atomic64_add(ns, &total_ns);
	if (ret < 0)
		atomic64_inc(&errs);
	/* lock-free max: retry if somebody else raised it meanwhile */
	while (ns > old && !atomic64_try_cmpxchg(&max_ns, &old, ns))
		;
	return 0;
}

/*
 * maxactive: how many calls can be in flight at once (each needs an
 * instance to remember its return address). Too few -> "nmissed" counts
 * returns we could not catch. For a function that may sleep (vfs_read
 * does), many tasks can be inside at once.
 */
static struct kretprobe krp = {
	.entry_handler = krp_entry,
	.handler = krp_ret,
	.data_size = sizeof(struct call_data),
	.maxactive = 64,
};

static int stats_show(struct seq_file *m, void *unused)
{
	s64 r = atomic64_read(&rets);

	seq_printf(m, "symbol:   %s\n", symbol);
	seq_printf(m, "calls:    %lld\n", atomic64_read(&hits));
	seq_printf(m, "returns:  %lld (%lld negative = errors)\n", r,
		   atomic64_read(&errs));
	seq_printf(m, "avg ns:   %lld\n", r ? atomic64_read(&total_ns) / r : 0);
	seq_printf(m, "max ns:   %lld\n", atomic64_read(&max_ns));
	seq_printf(m, "missed:   kprobe %lu, kretprobe %d\n", kp.nmissed, krp.nmissed);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static int __init kprobe_demo_init(void)
{
	int err;

	if (!symbol || !*symbol || strlen(symbol) >= KSYM_NAME_LEN) {
		pr_err("symbol must be a kernel function name\n");
		return -EINVAL;
	}
	kp.symbol_name = symbol;
	krp.kp.symbol_name = symbol;

	err = register_kprobe(&kp);
	if (err) {
		/*
		 * Typical failures, all handled by refusing to load:
		 *  -ENOENT  no such symbol (typo, static and inlined, or
		 *           not in this kernel: check /proc/kallsyms)
		 *  -EINVAL  blacklisted or notrace: see
		 *           /sys/kernel/debug/kprobes/blacklist
		 *  -EILSEQ  address is not at an instruction boundary
		 */
		pr_err("register_kprobe(%s) failed: %pe (%s)\n", symbol, ERR_PTR(err),
		       err == -ENOENT ? "symbol not found / inlined" :
		       err == -EINVAL ? "blacklisted or not probe-able" : "see dmesg");
		return err;
	}
	err = register_kretprobe(&krp);
	if (err) {
		pr_err("register_kretprobe(%s) failed: %pe\n", symbol, ERR_PTR(err));
		goto err_kp;
	}

	kp_dir = debugfs_create_dir("kprobe_demo", NULL);
	debugfs_create_file("stats", 0444, kp_dir, NULL, &stats_fops);

	/* %pS of the probe address: shows where it really landed (sym+off). */
	pr_info("probing %s at %pS; stats in /sys/kernel/debug/kprobe_demo/stats\n",
		symbol, kp.addr);
	return 0;

err_kp:
	unregister_kprobe(&kp);
	return err;
}

static void __exit kprobe_demo_exit(void)
{
	debugfs_remove(kp_dir);
	/*
	 * Unregistering waits until no CPU is inside our handlers any more
	 * (it synchronizes RCU), so the module text can go away safely.
	 */
	unregister_kretprobe(&krp);
	unregister_kprobe(&kp);
	pr_info("%s: %lld calls, %lld returns (%lld errors), max %lld ns, missed %d\n",
		symbol, atomic64_read(&hits), atomic64_read(&rets),
		atomic64_read(&errs), atomic64_read(&max_ns), krp.nmissed);
}

module_init(kprobe_demo_init);
module_exit(kprobe_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("kprobe + kretprobe call counter and timer for a chosen kernel function");
