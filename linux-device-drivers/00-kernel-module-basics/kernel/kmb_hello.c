// SPDX-License-Identifier: GPL-2.0
/*
 * kmb_hello.c — the smallest useful module: init/exit, printk levels and
 *               every kind of module parameter, checked and self-reported
 *
 * Concept : A module is an ELF object (.ko) the kernel links into itself at
 *           run time. module_init() names the function run by insmod (its
 *           return value decides whether the load succeeds), module_exit()
 *           the one run by rmmod. Parameters are set on the insmod command
 *           line (or modprobe.d) BEFORE init runs and appear under
 *           /sys/module/kmb_hello/parameters/ with the permissions given.
 *           A parameter that may be changed at run time gets its own
 *           kernel_param_ops so a bad value is rejected with -EINVAL.
 * Build   : make drivers   (or: make -C linux-device-drivers/00-kernel-module-basics/kernel)
 * Run     : sudo insmod kmb_hello.ko name=AI64 values=1,2,3 verbose=1 level=5
 *           grep -r . /sys/module/kmb_hello/parameters
 *           sudo ./build/linux-device-drivers/00-kernel-module-basics/user/kmb_params
 *           sudo rmmod kmb_hello
 * Read    : ../README.md  (sections 3.2 "Lifecycle" and 3.4 "Module parameters")
 */

/*
 * pr_fmt() is applied by every pr_*() call in this file, so each log line
 * starts with "kmb_hello: ". It must be defined BEFORE any #include, because
 * <linux/printk.h> only provides a default when it is not defined yet.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/string.h>

#define KMB_MAX_VALUES 8
#define KMB_LEVEL_MAX 7

/* ---- 1. plain parameters ------------------------------------------------ */
/*
 * module_param(var, type, perm):
 *   type  : byte, short, ushort, int, uint, long, ulong, charp, bool, invbool
 *   perm  : mode of /sys/module/kmb_hello/parameters/<var>
 *           0     -> no sysfs file at all (load-time only)
 *           0444  -> anyone may read
 *           0644  -> root may also WRITE it while the module runs, so the
 *                    code must re-read it (READ_ONCE) instead of caching it
 * Defaults are the C initialisers. MODULE_PARM_DESC feeds `modinfo`.
 */
static int count = 3;
module_param(count, int, 0444);
MODULE_PARM_DESC(count, "greeting lines printed at load (1..16, default 3)");

/*
 * charp: the kernel kmalloc()s a copy of the string and points `name` at it.
 * Keep it read-only (0444): a writable charp would be replaced (and the old
 * string freed) under your feet by a sysfs write.
 */
static char *name = "BeagleBone";
module_param(name, charp, 0444);
MODULE_PARM_DESC(name, "who to greet (string)");

static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "Y/N/1/0: print extra lines (writable at run time)");

/* perm 0: settable at insmod time but invisible in sysfs. */
static int secret = 42;
module_param(secret, int, 0);
MODULE_PARM_DESC(secret, "load-time only, no sysfs file");

/* ---- 2. array and fixed-buffer string ------------------------------------ */
/*
 * values=1,2,3 fills the first 3 slots and sets nr_values = 3. More than
 * KMB_MAX_VALUES elements makes the load fail with -EINVAL.
 */
static int values[KMB_MAX_VALUES];
static int nr_values;
module_param_array(values, int, &nr_values, 0444);
MODULE_PARM_DESC(values, "up to 8 comma-separated ints");

/*
 * module_param_string copies into OUR buffer (no allocation); a string that
 * does not fit is rejected ("string parameter too long").
 */
static char tag[16] = "ai64";
module_param_string(tag, tag, sizeof(tag), 0444);
MODULE_PARM_DESC(tag, "short label, max 15 chars (copied into a fixed buffer)");

/* ---- 3. a parameter with its own set() so run-time writes are checked ---- */
static int level = 4;

/* Pure function: easy to self-test at load time. */
static int kmb_level_parse(const char *val, int *out)
{
	int v, ret;

	ret = kstrtoint(val, 0, &v);	/* handles "5", "0x5", "5\n" */
	if (ret)
		return ret;
	if (v < 0 || v > KMB_LEVEL_MAX)
		return -EINVAL;		/* user sees: write error: Invalid argument */
	*out = v;
	return 0;
}

/*
 * Called for "level=N" on the insmod line (before init) AND for every
 * `echo N > /sys/module/kmb_hello/parameters/level`. The module core holds
 * the module's param lock around set/get, so two writers do not interleave;
 * our own code reads `level` without that lock, hence WRITE_ONCE/READ_ONCE.
 */
static int kmb_level_set(const char *val, const struct kernel_param *kp)
{
	int v, ret;

	ret = kmb_level_parse(val, &v);
	if (ret)
		return ret;
	WRITE_ONCE(*(int *)kp->arg, v);
	return 0;
}

static const struct kernel_param_ops kmb_level_ops = {
	.set = kmb_level_set,
	.get = param_get_int,		/* the stock formatter is fine for reads */
};
module_param_cb(level, &kmb_level_ops, &level, 0644);
MODULE_PARM_DESC(level, "0..7, writable at run time; out-of-range writes fail with EINVAL");

/* ---- helpers ------------------------------------------------------------- */

/*
 * How deep into the (small, fixed) kernel stack are we? The stack of the
 * insmod process's system call is THREAD_SIZE bytes (16 KiB on arm64 and
 * x86-64) and grows DOWN from task_stack_page() + THREAD_SIZE:
 *
 *   task_stack_page(current)                 + THREAD_SIZE
 *   |<------------------ 16 KiB ------------------>|
 *   [ guard | ......... free ......... | used by   ]
 *                                      ^ here      ^ top: syscall entry
 *                                        finit_module -> do_init_module -> us
 *
 * This is why kernel code never puts big arrays on the stack: there is no
 * "grow on demand", an overflow hits the guard page (VMAP_STACK) and oopses.
 */
static unsigned long kmb_stack_used(void)
{
	unsigned long top = (unsigned long)task_stack_page(current) + THREAD_SIZE;
	unsigned long here = (unsigned long)&top;	/* a local lives on the stack */

	return top - here;
}

/* ---- init / exit --------------------------------------------------------- */
/*
 * __init puts the function in .init.text, which the kernel frees right after
 * init returns successfully: never call it (or take its address) later.
 * __initconst/__initdata do the same for data.
 */
static const char kmb_banner[] __initconst = "hello from a loadable module";

static int __init kmb_hello_init(void)
{
	int i, sum = 0, v, failed = 0;

	/*
	 * Validate load-time parameters FIRST and refuse to load if they are
	 * wrong: returning a negative errno from init aborts insmod, frees the
	 * module and never calls exit. (`level` was already checked by its
	 * set() during argument parsing.)
	 */
	if (count < 1 || count > 16) {
		pr_err("count=%d out of range 1..16\n", count);
		return -EINVAL;
	}
	if (!name || !*name) {
		pr_err("name must not be empty\n");
		return -EINVAL;
	}

	/* printk levels: KERN_ERR(3) .. KERN_DEBUG(7); see README 3.3. */
	pr_notice("%s (kernel stack THREAD_SIZE = %lu bytes)\n", kmb_banner,
		  (unsigned long)THREAD_SIZE);
	for (i = 0; i < count; i++)
		pr_info("hello, %s! (%d/%d)\n", name, i + 1, count);

	/*
	 * pr_debug() compiles to nothing visible unless dynamic debug enables
	 * it: `insmod kmb_hello.ko dyndbg=+p`, or later
	 * echo 'module kmb_hello +p' > /sys/kernel/debug/dynamic_debug/control
	 */
	pr_debug("dynamic debug is ON for this module (secret=%d)\n", secret);

	/* ---- self-report: the parameters as the module sees them ---- */
	pr_info("INFO params: name=\"%s\" tag=\"%s\" verbose=%c level=%d values given=%d\n",
		name, tag, verbose ? 'Y' : 'N', READ_ONCE(level), nr_values);

	/* Test 1: everything the kernel parsed is inside our limits. */
	if (nr_values >= 0 && nr_values <= KMB_MAX_VALUES &&
	    READ_ONCE(level) >= 0 && READ_ONCE(level) <= KMB_LEVEL_MAX &&
	    strnlen(tag, sizeof(tag)) < sizeof(tag)) {
		pr_info("PASS params    count=%d in 1..16, level in 0..%d, %d of %d array slots, tag NUL-terminated\n",
			count, KMB_LEVEL_MAX, nr_values, KMB_MAX_VALUES);
	} else {
		pr_err("FAIL params    nr_values=%d level=%d\n", nr_values, level);
		failed++;
	}

	/* Test 2: the array really holds what was passed. */
	for (i = 0; i < nr_values; i++)
		sum += values[i];
	pr_info("PASS array     values[0..%d) sum = %d\n", nr_values, sum);
	if (verbose)
		for (i = 0; i < nr_values; i++)
			pr_info("  values[%d] = %d\n", i, values[i]);

	/* Test 3: the run-time setter's parser accepts/rejects correctly. */
	if (kmb_level_parse("7", &v) == 0 && v == 7 &&
	    kmb_level_parse("8", &v) == -EINVAL &&
	    kmb_level_parse("-1", &v) == -EINVAL &&
	    kmb_level_parse("abc", &v) == -EINVAL) {
		pr_info("PASS setter    \"7\" accepted; \"8\", \"-1\", \"abc\" -> -EINVAL\n");
	} else {
		pr_err("FAIL setter    level parser misbehaves\n");
		failed++;
	}

	/* Test 4: we are nowhere near the end of the fixed kernel stack. */
	if (kmb_stack_used() < THREAD_SIZE / 2) {
		pr_info("PASS stack     ~%lu of %lu bytes used in insmod's call chain\n",
			kmb_stack_used(), (unsigned long)THREAD_SIZE);
	} else {
		pr_err("FAIL stack     %lu bytes used\n", kmb_stack_used());
		failed++;
	}

	if (failed)
		pr_err("done: %d check(s) FAILED\n", failed);
	else
		pr_info("done: all checks passed\n");
	return 0;	/* loaded: parameters are now in /sys/module/kmb_hello/ */
}

/*
 * __exit: dropped entirely when the code is built into the kernel (it can
 * never be unloaded then). Without a module_exit() the module cannot be
 * removed at all ("rmmod: ERROR: ... Device or resource busy").
 */
static void __exit kmb_hello_exit(void)
{
	pr_info("goodbye, %s (level was %d at unload)\n", name, READ_ONCE(level));
}

module_init(kmb_hello_init);
module_exit(kmb_hello_exit);

/*
 * MODULE_LICENSE is not decoration: without a GPL-compatible license the
 * kernel taints itself ("P", proprietary) and refuses to resolve any
 * EXPORT_SYMBOL_GPL symbol for this module.
 */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Module basics: init/exit, printk levels, all parameter kinds");
MODULE_VERSION("1.0");
