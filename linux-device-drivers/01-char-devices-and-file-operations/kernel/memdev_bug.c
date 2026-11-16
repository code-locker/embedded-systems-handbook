// SPDX-License-Identifier: GPL-2.0
/*
 * memdev_bug.c — INTENTIONALLY BROKEN: dereferences __user pointers
 *                directly instead of using copy_{to,from}_user()
 *
 * Concept : Built only with `make BUGS=1`. read() memcpy()s straight into
 *           the user buffer and write() straight out of it. What happens
 *           depends on the CPU, which is exactly why it is dangerous:
 *             x86-64 with SMAP, arm64 with PAN (ARMv8.1+, e.g. A76):
 *               every access faults -> kernel oops, even for a valid buffer
 *             BeagleBone AI-64 (Cortex-A72 = ARMv8.0, no PAN, and the
 *             board kernel has CONFIG_ARM64_SW_TTBR0_PAN=n):
 *               `cat /dev/memdev_bug` WORKS — the bug hides — until an
 *               invalid pointer arrives: then it is an oops ("Unable to
 *               handle kernel paging request") instead of the -EFAULT
 *               that copy_to_user() would have returned, because only the
 *               uaccess routines have exception-table fixups.
 *           Without the __force casts below, sparse (`make C=1`) reports
 *           "incorrect type in argument (different address spaces)";
 *           the casts silence it — which is what makes them a lie.
 *           Find it with: sparse; a CPU/kernel with PAN; the memdev_tool
 *           `efault` command (read into a PROT_NONE page).
 * Build   : make -C linux-device-drivers/01-char-devices-and-file-operations/kernel BUGS=1
 * Run     : VM or spare board only: sudo insmod memdev_bug.ko ; cat /dev/memdev_bug
 *           sudo ./memdev_tool /dev/memdev_bug efault     <- oops here
 * Read    : ../README.md  (section 3.5 "Crossing the user/kernel boundary")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/string.h>

static const char msg[] = "this text was memcpy'd into user space without copy_to_user\n";
static char sink[64];

static ssize_t mb_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	size_t n;

	if (*ppos >= sizeof(msg) - 1)
		return 0;
	n = min_t(size_t, count, sizeof(msg) - 1 - *ppos);
	/* BUG: no access_ok(), no fault handling, bypasses PAN/SMAP rules. */
	memcpy((void __force *)buf, msg + *ppos, n);
	*ppos += n;
	return n;
}

static ssize_t mb_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	size_t n = min(count, sizeof(sink));

	/* BUG: the same in the other direction. */
	memcpy(sink, (const void __force *)buf, n);
	return count;
}

static const struct file_operations mb_fops = {
	.owner	= THIS_MODULE,
	.read	= mb_read,
	.write	= mb_write,
};

static struct miscdevice mb_dev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "memdev_bug",
	.fops	= &mb_fops,
	.mode	= 0666,
};

static int __init memdev_bug_init(void)
{
	pr_warn("loaded an intentionally broken driver: /dev/memdev_bug\n");
	return misc_register(&mb_dev);
}

static void __exit memdev_bug_exit(void)
{
	misc_deregister(&mb_dev);
}

module_init(memdev_bug_init);
module_exit(memdev_bug_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("BROKEN on purpose: direct __user pointer dereference");
