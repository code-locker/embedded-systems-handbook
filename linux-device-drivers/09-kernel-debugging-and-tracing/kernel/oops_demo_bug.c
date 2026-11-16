// SPDX-License-Identifier: GPL-2.0
/*
 * oops_demo_bug.c — INTENTIONALLY BROKEN: oopses the kernel on request so
 *                   you can practise reading and decoding an oops
 *
 * Concept : Loading the module is harmless. Writing a number to its
 *           parameter runs one of the bugs, three function calls deep so
 *           the call trace has something to show:
 *             1  NULL pointer dereference -> "Unable to handle kernel NULL
 *                pointer dereference" (arm64) / "BUG: kernel NULL pointer
 *                dereference" (x86), Oops, the writing process is killed
 *             2  BUG()  -> "kernel BUG at .../oops_demo_bug.c:NN!", Oops
 *             3  WARN() -> "WARNING: ... at ...", the kernel continues
 *           After 1 or 2 the kernel is tainted (D) and the module usually
 *           cannot be removed (its refcount is stuck): reboot.
 * Tool    : the oops itself; decode it with scripts/decode_stacktrace.sh,
 *           scripts/faddr2line, or gdb on oops_demo_bug.ko.
 * Build   : make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel BUGS=1
 * Run     : sudo insmod oops_demo_bug.ko
 *           echo 1 | sudo tee /sys/module/oops_demo_bug/parameters/trigger
 *           (board or VM ONLY; with panic_on_oops=1 the machine reboots)
 * Read    : ../README.md  (section 3.2 "Reading an oops" and 3.3 "Decoding")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

struct esh_widget {
	u64 id;
	u64 flags;
	u32 value;		/* offset 0x10: the fault address will be 0x10 */
};

static struct esh_widget *widget;	/* never set: NULL on purpose */
static bool armed;			/* triggers only after init finished */

/* noinline everywhere: we want each frame in the call trace. */
static noinline u32 widget_read_value(struct esh_widget *w)
{
	/* BUG: w is NULL. Loads from address 0 + offsetof(value) = 0x10. */
	return READ_ONCE(w->value);
}

static noinline u32 widget_process(int how)
{
	u32 v = 0;

	switch (how) {
	case 1:
		v = widget_read_value(READ_ONCE(widget));
		break;
	case 2:
		/* BUG(): "this can never happen". Prefer WARN + recovery. */
		BUG_ON(!READ_ONCE(widget));
		break;
	case 3:
		WARN(1, "oops_demo_bug: a WARN, the kernel keeps running\n");
		break;
	}
	return v;
}

static int trigger_set(const char *val, const struct kernel_param *kp)
{
	int how, err = kstrtoint(val, 0, &how);

	if (err)
		return err;
	if (how < 1 || how > 3)
		return -EINVAL;
	if (!READ_ONCE(armed))	/* "insmod ... trigger=1" must not crash the load */
		return -EBUSY;
	pr_alert("triggering bug %d now\n", how);
	pr_info("value %u\n", widget_process(how));	/* not reached for 1 and 2 */
	return 0;
}

static const struct kernel_param_ops trigger_ops = {
	.set = trigger_set,
};
module_param_cb(trigger, &trigger_ops, NULL, 0200);
MODULE_PARM_DESC(trigger, "write 1 = NULL deref, 2 = BUG(), 3 = WARN()");

static int __init oops_demo_init(void)
{
	WRITE_ONCE(armed, true);
	pr_info("armed: echo 1|2|3 > /sys/module/oops_demo_bug/parameters/trigger\n");
	return 0;
}

static void __exit oops_demo_exit(void)
{
	pr_info("unloaded\n");
}

module_init(oops_demo_init);
module_exit(oops_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Intentionally oopsing module for oops-decoding practice");
