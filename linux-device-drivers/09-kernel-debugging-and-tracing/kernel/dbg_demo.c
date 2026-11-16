// SPDX-License-Identifier: GPL-2.0
/*
 * dbg_demo.c — printk in depth: log levels, pr_debug + dynamic debug,
 *              rate limiting, print_hex_dump, %p format specifiers,
 *              WARN_ONCE, and a function made to be traced with ftrace
 *
 * Concept : printk is the first debugging tool and the one you always have.
 *           This module prints at each level (filter with console_loglevel
 *           / dmesg -l), has pr_debug() call sites that are off until you
 *           enable them through /sys/kernel/debug/dynamic_debug/control,
 *           proves that a ratelimit state lets exactly `burst` messages
 *           through, dumps a buffer with print_hex_dump, shows what %p,
 *           %pK, %pS, %ps, %pe, %pOF, %*ph print, and fires a WARN_ONCE on
 *           request. dbg_demo_target() is called from a delayed work every
 *           period_ms (or on demand from debugfs) so you have a function to
 *           point ftrace's function / function_graph tracers at.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel)
 * Run     : sudo insmod dbg_demo.ko [period_ms=500] [warn=1]
 *           echo 'module dbg_demo +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
 *           echo 5 | sudo tee /sys/kernel/debug/dbg_demo/poke
 *           sudo rmmod dbg_demo
 * Read    : ../README.md  (sections 3.1 "printk" and 3.5 "ftrace")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

static unsigned int period_ms;
module_param(period_ms, uint, 0444);
MODULE_PARM_DESC(period_ms, "call dbg_demo_target() every period_ms from a work item (0 = off, 10..10000)");

static bool warn;
module_param(warn, bool, 0444);
MODULE_PARM_DESC(warn, "fire a WARN_ONCE() at load time (taints the kernel with W)");

static int nr_fail;
static u64 target_calls;		/* read through debugfs "calls" */
static struct dentry *dbg_dir;
static struct delayed_work tick;

static __printf(3, 4) void check(bool ok, const char *test, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	if (ok) {
		pr_info("PASS %-10s %pV\n", test, &vaf);
	} else {
		nr_fail++;
		pr_err("FAIL %-10s %pV\n", test, &vaf);
	}
	va_end(args);
}

/* ------------------------------------------------------------------------ */
/* ftrace targets                                                            */
/* ------------------------------------------------------------------------ */
/*
 * noinline: an inlined function has no entry of its own, so ftrace (and
 * kprobes) cannot hook it. Static functions are fine as long as they are
 * not inlined: they show up as "dbg_demo_target [dbg_demo]" in
 * /sys/kernel/tracing/available_filter_functions.
 *
 * function_graph shows the nesting:
 *    dbg_demo_target [dbg_demo]() {
 *      dbg_demo_helper [dbg_demo]() {
 *        __kmalloc_cache_noprof();
 *        kfree();
 *      }
 *    }
 */
static noinline u32 dbg_demo_helper(u32 x)
{
	u8 *tmp = kmalloc(32, GFP_KERNEL);	/* something for the graph to show */
	u32 r = x * 2654435761u;		/* Knuth's multiplicative hash */

	kfree(tmp);
	return r;
}

static noinline u32 dbg_demo_target(u32 x)
{
	u32 r = dbg_demo_helper(x);

	target_calls++;
	/* Off by default; costs one static-key NOP when disabled. */
	pr_debug("target(%u) -> %08x, call %llu\n", x, r, target_calls);
	return r;
}

static void tick_fn(struct work_struct *w)
{
	dbg_demo_target((u32)target_calls);
	schedule_delayed_work(&tick, msecs_to_jiffies(period_ms));
}

/* ------------------------------------------------------------------------ */
/* Load-time demonstrations                                                  */
/* ------------------------------------------------------------------------ */
static void demo_levels(void)
{
	/*
	 * KERN_EMERG(0) ALERT(1) CRIT(2) ERR(3) WARNING(4) NOTICE(5) INFO(6)
	 * DEBUG(7). A message reaches the CONSOLE (serial port) only if its
	 * level < console_loglevel (first number in /proc/sys/kernel/printk).
	 * Every level always goes to the log buffer (dmesg). 0..2 are skipped
	 * here: emergency messages are broadcast to every terminal.
	 */
	pr_err("level 3 KERN_ERR     (pr_err)\n");
	pr_warn("level 4 KERN_WARNING (pr_warn)\n");
	pr_notice("level 5 KERN_NOTICE  (pr_notice)\n");
	pr_info("level 6 KERN_INFO    (pr_info)\n");
	/* pr_debug: compiled in only with DEBUG or CONFIG_DYNAMIC_DEBUG, then off until enabled */
	pr_debug("level 7 KERN_DEBUG   (pr_debug: you enabled dynamic debug!)\n");
	/*
	 * (pr_cont() can continue a line, but another CPU's message may land
	 * in between: build the whole line first and print it once.)
	 */
}

static void demo_ratelimit(void)
{
	/* At most 3 messages per second from this site; the rest are counted. */
	static DEFINE_RATELIMIT_STATE(rs, HZ, 3);
	int i, printed = 0;

	/*
	 * Don't print from the "suppressed" report automatically at release
	 * time: we want the count, printed below. Without this flag the next
	 * __ratelimit() after the interval prints "N callbacks suppressed".
	 */
	ratelimit_set_flags(&rs, RATELIMIT_MSG_ON_RELEASE);
	for (i = 0; i < 100; i++) {
		if (__ratelimit(&rs)) {
			pr_info("ratelimited message %d\n", i);
			printed++;
		}
	}
	/*
	 * 3 is exact unless the loop happened to straddle a 1 s window
	 * boundary (another burst of up to 3). Also: pr_info_ratelimited()
	 * uses a hidden per-call-site state (5 s, burst 10).
	 */
	check(printed >= 3 && printed <= 6, "ratelimit",
	      "100 attempts, %d printed, %d suppressed (burst 3 per second)",
	      printed, 100 - printed);
}

struct demo_pkt {
	__be16 type;
	__be16 len;
	u8 payload[20];
};

static void demo_hexdump(void)
{
	struct demo_pkt pkt = { .type = cpu_to_be16(0x88b5), .len = cpu_to_be16(20) };
	int i;

	for (i = 0; i < ARRAY_SIZE(pkt.payload); i++)
		pkt.payload[i] = 'A' + i;
	/* prefix, offsets, 16 bytes per row, 1-byte groups, with ASCII column */
	print_hex_dump(KERN_INFO, KBUILD_MODNAME ": ", DUMP_PREFIX_OFFSET, 16, 1,
		       &pkt, sizeof(pkt), true);
	/* _debug variant: a dynamic-debug site like pr_debug */
	print_hex_dump_debug(KBUILD_MODNAME ": dbg ", DUMP_PREFIX_OFFSET, 16, 1,
			     &pkt, sizeof(pkt), false);
	/* Short buffers inline: %*ph (up to 64 bytes) */
	pr_info("header bytes: %*ph\n", 4, &pkt);
}

/* noinline: so that __builtin_return_address(0) below is dbg_demo_init */
static noinline void demo_formats(void)
{
	void *obj = kmalloc(16, GFP_KERNEL);
	struct device_node *root = of_find_node_by_path("/");

	/*
	 * %p   hashed (unique per pointer, but no address leak) — the default
	 * %pK  raw or zeros depending on /proc/sys/kernel/kptr_restrict
	 * %px  raw address: avoid (checkpatch warns), never in production logs
	 * %pS  symbol+offset/size [module]      %ps  symbol only
	 * %pe  an ERR_PTR as its name            %pOF device-tree node path
	 */
	pr_info("%%p   %p\n", obj);
	pr_info("%%pK  %pK\n", obj);
	pr_info("%%pS  %pS\n", dbg_demo_target);
	pr_info("%%ps  %ps\n", dbg_demo_target);
	pr_info("%%pS of the caller of this function: %pS\n", __builtin_return_address(0));
	pr_info("%%pe  %pe\n", ERR_PTR(-ENOMEM));
	if (root)	/* NULL on x86 without a device tree */
		pr_info("%%pOF %pOF (the device-tree root)\n", root);
	of_node_put(root);
	kfree(obj);
}

/* ------------------------------------------------------------------------ */
/* debugfs: poke the target, fire a WARN                                     */
/* ------------------------------------------------------------------------ */
static ssize_t poke_write(struct file *f, const char __user *ubuf, size_t count,
			  loff_t *ppos)
{
	unsigned int n, i;
	int err = kstrtouint_from_user(ubuf, count, 0, &n);

	if (err)
		return err;
	if (n > 100000)
		return -EINVAL;
	for (i = 0; i < n; i++)
		dbg_demo_target(i);
	return count;
}

static const struct file_operations poke_fops = {
	.owner = THIS_MODULE,
	.write = poke_write,
};

static ssize_t warn_write(struct file *f, const char __user *ubuf, size_t count,
			  loff_t *ppos)
{
	/*
	 * WARN*() prints "WARNING: CPU: ... at file:line function", the
	 * registers and a call trace, taints the kernel (W) and CONTINUES.
	 * _ONCE: only the first time this line runs. With panic_on_warn=1 it
	 * panics instead (used by fuzzers and some safety-critical setups).
	 */
	WARN_ONCE(1, "dbg_demo: WARN_ONCE requested via debugfs (%zu bytes written)\n",
		  count);
	return count;
}

static const struct file_operations warn_fops = {
	.owner = THIS_MODULE,
	.write = warn_write,
};

/* ------------------------------------------------------------------------ */

static int __init dbg_demo_init(void)
{
	if (period_ms && (period_ms < 10 || period_ms > 10000)) {
		pr_err("period_ms must be 0 or 10..10000\n");
		return -EINVAL;
	}

	demo_levels();
	demo_ratelimit();
	demo_hexdump();
	demo_formats();
	check(dbg_demo_target(1) == 2654435761u, "target", "dbg_demo_target(1) hash ok");

	if (warn)
		WARN_ONCE(1, "dbg_demo: WARN_ONCE at load time because warn=1\n");

	dbg_dir = debugfs_create_dir("dbg_demo", NULL);
	debugfs_create_file("poke", 0200, dbg_dir, NULL, &poke_fops);
	debugfs_create_file("warn", 0200, dbg_dir, NULL, &warn_fops);
	debugfs_create_u64("calls", 0444, dbg_dir, &target_calls);

	INIT_DELAYED_WORK(&tick, tick_fn);
	if (period_ms)
		schedule_delayed_work(&tick, msecs_to_jiffies(period_ms));

	pr_info("ready: ftrace target dbg_demo_target, debugfs /sys/kernel/debug/dbg_demo/{poke,warn,calls}%s\n",
		period_ms ? ", ticking" : "");
	if (nr_fail)
		pr_err("done: %d check(s) FAILED\n", nr_fail);
	else
		pr_info("done: all checks passed\n");
	return 0;
}

static void __exit dbg_demo_exit(void)
{
	debugfs_remove(dbg_dir);		/* no more pokes */
	cancel_delayed_work_sync(&tick);	/* safe even if never scheduled */
	pr_info("unloaded, dbg_demo_target was called %llu times\n", target_calls);
}

module_init(dbg_demo_init);
module_exit(dbg_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("printk levels, dynamic debug, ratelimit, hexdump, %p formats, WARN, ftrace target");
