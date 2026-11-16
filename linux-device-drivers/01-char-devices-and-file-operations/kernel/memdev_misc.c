// SPDX-License-Identifier: GPL-2.0
/*
 * memdev_misc.c — the same idea as memdev.c in ~1/3 of the code:
 *                 a misc device /dev/memdev_misc with a fixed 4 KiB buffer
 *
 * Concept : A misc device is a char device on the shared major 10. One
 *           misc_register() call replaces alloc_chrdev_region + cdev_add +
 *           class_create + device_create, and /dev/<name> appears. Use it
 *           for "one device node per driver" (watchdog, /dev/ksync, most
 *           control interfaces); use the full cdev path when you need many
 *           minors, your own major or a class of your own.
 *           The fops also show the libfs helpers that implement the *ppos
 *           convention for a flat buffer: simple_read_from_buffer(),
 *           simple_write_to_buffer() and generic_file_llseek_size().
 * Build   : make drivers
 * Run     : sudo insmod memdev_misc.ko
 *           echo hello > /dev/memdev_misc ; cat /dev/memdev_misc
 *           ls -l /dev/memdev_misc        (crw-rw-rw- 10, <dynamic minor>)
 *           sudo rmmod memdev_misc
 * Read    : ../README.md  (section 3.4 "Misc devices")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>

#define MISC_SIZE 4096

/* Static storage: no allocation, nothing to free, lives as long as the module. */
static struct {
	struct mutex lock;		/* protects data and len */
	char data[MISC_SIZE];
	size_t len;			/* valid bytes */
} mm;

static int mm_open(struct inode *inode, struct file *file)
{
	/*
	 * The misc core's own open() stored &memdev_misc_dev in
	 * file->private_data before calling us. A driver with per-device
	 * state embeds the miscdevice in its struct and uses container_of.
	 */
	if ((file->f_flags & O_TRUNC) && (file->f_mode & FMODE_WRITE)) {
		mutex_lock(&mm.lock);
		mm.len = 0;		/* `echo x > /dev/memdev_misc` replaces */
		mutex_unlock(&mm.lock);
	}
	return 0;
}

static ssize_t mm_read(struct file *file, char __user *buf, size_t count,
		       loff_t *ppos)
{
	ssize_t ret;

	if (mutex_lock_interruptible(&mm.lock))
		return -ERESTARTSYS;
	/* Clamps to [*ppos, len), copy_to_user, advances *ppos, 0 at EOF. */
	ret = simple_read_from_buffer(buf, count, ppos, mm.data, mm.len);
	mutex_unlock(&mm.lock);
	return ret;
}

static ssize_t mm_write(struct file *file, const char __user *buf,
			size_t count, loff_t *ppos)
{
	ssize_t ret;

	if (mutex_lock_interruptible(&mm.lock))
		return -ERESTARTSYS;
	if (*ppos >= MISC_SIZE && count) {
		ret = -ENOSPC;	/* the helper would return 0, which `echo` retries forever */
	} else {
		/* Clamps to [*ppos, MISC_SIZE), copy_from_user, advances *ppos. */
		ret = simple_write_to_buffer(mm.data, MISC_SIZE, ppos, buf, count);
		if (ret > 0 && *ppos > mm.len)
			mm.len = *ppos;
	}
	mutex_unlock(&mm.lock);
	return ret;
}

static loff_t mm_llseek(struct file *file, loff_t off, int whence)
{
	loff_t ret;

	mutex_lock(&mm.lock);
	/* max position MISC_SIZE, SEEK_END relative to len: like memdev.c. */
	ret = generic_file_llseek_size(file, off, whence, MISC_SIZE, mm.len);
	mutex_unlock(&mm.lock);
	return ret;
}

static const struct file_operations mm_fops = {
	.owner	= THIS_MODULE,
	.open	= mm_open,
	.read	= mm_read,
	.write	= mm_write,
	.llseek	= mm_llseek,
};

static struct miscdevice memdev_misc_dev = {
	.minor	= MISC_DYNAMIC_MINOR,	/* major is always 10; minor picked for us */
	.name	= "memdev_misc",	/* -> /dev/memdev_misc and /sys/class/misc/memdev_misc */
	.fops	= &mm_fops,
	.mode	= 0666,			/* node mode, like memdev's devnode() callback */
};

static int __init memdev_misc_init(void)
{
	int err;

	mutex_init(&mm.lock);		/* before misc_register: open() may follow at once */
	err = misc_register(&memdev_misc_dev);
	if (err) {
		pr_err("misc_register failed: %d\n", err);
		return err;
	}
	pr_info("/dev/memdev_misc ready: major 10, minor %d, %d bytes\n",
		memdev_misc_dev.minor, MISC_SIZE);
	return 0;
}

static void __exit memdev_misc_exit(void)
{
	misc_deregister(&memdev_misc_dev);
	pr_info("unloaded\n");
}

module_init(memdev_misc_init);
module_exit(memdev_misc_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("/dev/memdev_misc: the memdev idea as a misc device with libfs helpers");
