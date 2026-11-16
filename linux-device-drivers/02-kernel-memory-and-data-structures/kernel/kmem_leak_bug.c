// SPDX-License-Identifier: GPL-2.0
/*
 * kmem_leak_bug.c — INTENTIONALLY BROKEN: leaks kmalloc() and vmalloc()
 *                   memory so kmemleak has something to find
 *
 * Concept : A kernel leak is never cleaned up by "process exit": memory
 *           allocated by a module and forgotten stays allocated until
 *           reboot, even after rmmod. kmemleak finds such blocks the way a
 *           tracing garbage collector would: it scans RAM (data, bss, stacks,
 *           slab objects) for anything that looks like a pointer to each
 *           allocated block; a block nobody points to is reported with the
 *           stack trace of its allocation. This module leaks three ways:
 *             1. a pointer kept only in a local variable that goes away,
 *             2. an error path that forgets to free (the classic one),
 *             3. a vmalloc() buffer, to show kmemleak tracks it too.
 * Tool    : kernel with CONFIG_DEBUG_KMEMLEAK=y, then
 *             echo scan > /sys/kernel/debug/kmemleak
 *             cat /sys/kernel/debug/kmemleak
 * Build   : make -C linux-device-drivers/02-kernel-memory-and-data-structures/kernel BUGS=1
 * Run     : sudo insmod kmem_leak_bug.ko leaks=5 ; sudo rmmod kmem_leak_bug
 *           (board or VM only — the memory is gone until reboot)
 * Read    : ../README.md  (section 3.9 "Leaks, kmemleak and KASAN")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

static int leaks = 5;
module_param(leaks, int, 0444);
MODULE_PARM_DESC(leaks, "number of 128-byte kmalloc blocks to leak (1..100, default 5)");

/* noinline: keeps each leak site as its own frame in kmemleak's backtrace. */
static noinline void leak_local_pointer(int i)
{
	char *buf = kmalloc(128, GFP_KERNEL);

	if (!buf)
		return;
	/* A recognisable string: kmemleak's hex dump of the block shows it. */
	snprintf(buf, 128, "esh-leak-%d", i);
	/* BUG: no kfree(buf). The only pointer dies with this stack frame. */
}

struct widget {
	char *name;
	int *table;
};

static noinline int widget_setup_leaky(void)
{
	struct widget *w = kzalloc(sizeof(*w), GFP_KERNEL);

	if (!w)
		return -ENOMEM;
	w->name = kstrdup("esh-widget", GFP_KERNEL);
	if (!w->name)
		goto err;
	/* Pretend the second step fails: */
	w->table = NULL;
	goto err;
	/* ... normal path would return 0 here ... */

err:
	/*
	 * BUG: frees the widget but forgets w->name. Correct unwinding frees
	 * in reverse order of allocation:  kfree(w->name); kfree(w);
	 */
	kfree(w);
	return -ENOMEM;
}

static noinline void leak_vmalloc(void)
{
	void *big = vmalloc(64 * 1024);

	if (big)
		memset(big, 0x5A, 64 * 1024);
	/* BUG: no vfree(big). */
}

static int __init kmem_leak_bug_init(void)
{
	int i;

	if (leaks < 1 || leaks > 100) {
		pr_err("leaks must be 1..100\n");
		return -EINVAL;
	}
	for (i = 0; i < leaks; i++)
		leak_local_pointer(i);
	if (widget_setup_leaky())
		pr_info("widget setup failed (on purpose), leaked its name\n");
	leak_vmalloc();

	pr_warn("leaked %d x 128 B kmalloc + 1 kstrdup + 64 KiB vmalloc on purpose\n",
		leaks);
	pr_warn("now: echo scan > /sys/kernel/debug/kmemleak; cat /sys/kernel/debug/kmemleak\n");
	return 0;
}

static void __exit kmem_leak_bug_exit(void)
{
	/* Nothing to free: we lost every pointer. That is the bug. */
	pr_info("unloaded (the leaked memory stays allocated until reboot)\n");
}

module_init(kmem_leak_bug_init);
module_exit(kmem_leak_bug_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Intentionally leaking module for kmemleak practice");
