// SPDX-License-Identifier: GPL-2.0
/*
 * memdev.c — /dev/memdev0..N-1: a classic character driver with a
 *            resizable RAM buffer per minor, read/write/llseek/ioctl
 *
 * Concept : The full, "by hand" registration path of a char driver:
 *             alloc_chrdev_region()  reserve a major + nr_devs minors
 *             class_create()         /sys/class/memdev/ (udev listens)
 *             cdev_init/cdev_add()   minor -> our file_operations
 *             device_create()        uevent -> udev/devtmpfs make /dev/memdevN
 *           Each minor has its own buffer (kvzalloc, resizable by ioctl)
 *           guarded by a mutex; each open() gets a private struct in
 *           file->private_data with per-open counters. One minor
 *           (excl_minor=) admits a single opener at a time (-EBUSY).
 *           User pointers are only touched with copy_{to,from}_user /
 *           get_user / put_user. Unknown ioctls return -ENOTTY.
 * Build   : make drivers   (or: make -C <this lesson>/kernel)
 * Run     : sudo insmod memdev.ko [nr_devs=4 buf_size=4096 max_size=1048576 excl_minor=1]
 *           echo hi > /dev/memdev0 ; cat /dev/memdev0
 *           ./build/linux-device-drivers/01-char-devices-and-file-operations/user/memdev_test
 *           sudo rmmod memdev
 * Read    : ../README.md  (sections 3.1-3.7)
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/bitops.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "memdev_ioctl.h"

#define MEMDEV_MAX_DEVS 8
#define MEMDEV_MAX_SIZE (16UL << 20)	/* hard cap for max_size: 16 MiB */

static unsigned int nr_devs = 4;
module_param(nr_devs, uint, 0444);
MODULE_PARM_DESC(nr_devs, "number of minors /dev/memdev0..N-1 (1..8)");

static unsigned long buf_size = 4096;
module_param(buf_size, ulong, 0444);
MODULE_PARM_DESC(buf_size, "initial buffer size per device in bytes (1..max_size)");

static unsigned long max_size = 1UL << 20;
module_param(max_size, ulong, 0444);
MODULE_PARM_DESC(max_size, "largest size MEMDEV_IOC_SET_SIZE accepts (4096..16 MiB)");

static int excl_minor = 1;
module_param(excl_minor, int, 0444);
MODULE_PARM_DESC(excl_minor, "minor that allows only one open at a time, -1 = none");

/*
 * One per minor. The mutex protects everything below it that can change
 * after init; `busy` is a lock-free bit for the exclusive-open check.
 *
 *  struct memdev                          kvzalloc'd buffer
 *  +-----------+                          +---------------------+-------+
 *  | lock      |       data ------------> | valid data (len)    | zero  |
 *  | data,size |                          +---------------------+-------+
 *  | len       |                          0                    len    size
 *  | reads ... |
 *  | cdev  <---+--- inode->i_cdev (container_of gets us back to memdev)
 *  +-----------+
 */
struct memdev {
	struct mutex lock;	/* protects data..openers and the per-open counters */
	char *data;
	size_t size;		/* capacity */
	size_t len;		/* valid bytes: EOF for read(), base for SEEK_END */
	u64 reads;
	u64 writes;
	unsigned int openers;
	bool exclusive;
	unsigned long busy;	/* bit 0: exclusive minor is open */
	struct cdev cdev;
};

/* One per open(): what file->private_data points to. */
struct memdev_file {
	struct memdev *dev;
	u64 read_bytes;		/* protected by dev->lock */
	u64 write_bytes;
};

static dev_t memdev_base;		/* MAJOR + first minor (0) */
static struct class *memdev_class;
static struct memdev *memdevs;		/* array of nr_devs */

/* ---- helpers ------------------------------------------------------------- */

/*
 * New file position for llseek, or -EINVAL. `base` is 0 / f_pos / len and
 * already lies in [0, size], so base + off cannot overflow once off is
 * range-checked against it. Positions past `size` are refused (a real file
 * would allow holes; this buffer cannot grow by seeking).
 */
static loff_t memdev_new_pos(loff_t base, loff_t off, size_t size)
{
	if (off < -base || off > (loff_t)size - base)
		return -EINVAL;
	return base + off;
}

/*
 * Resize keeping min(len, new_size) bytes. The new buffer is allocated
 * BEFORE taking the lock (kvzalloc may sleep for a while and reclaim
 * memory) and the old one freed after dropping it: the critical section
 * is just memcpy + pointer swap.
 */
static int memdev_resize(struct memdev *dev, size_t new_size)
{
	char *nbuf, *old;

	if (new_size < 1 || new_size > max_size)
		return -EINVAL;
	/* kvzalloc: kmalloc for small sizes, vmalloc fallback for big ones. */
	nbuf = kvzalloc(new_size, GFP_KERNEL);
	if (!nbuf)
		return -ENOMEM;

	mutex_lock(&dev->lock);
	dev->len = min(dev->len, new_size);
	memcpy(nbuf, dev->data, dev->len);
	old = dev->data;
	dev->data = nbuf;
	dev->size = new_size;
	mutex_unlock(&dev->lock);

	kvfree(old);		/* kvfree: pairs with kvmalloc/kvzalloc */
	return 0;
}

/* ---- file operations ----------------------------------------------------- */

static int memdev_open(struct inode *inode, struct file *file)
{
	/* The inode belongs to /dev/memdevN; its i_cdev is OUR embedded cdev. */
	struct memdev *dev = container_of(inode->i_cdev, struct memdev, cdev);
	struct memdev_file *mf;

	/* Atomic test-and-set: two racing opens cannot both see "free". */
	if (dev->exclusive && test_and_set_bit(0, &dev->busy))
		return -EBUSY;

	mf = kzalloc(sizeof(*mf), GFP_KERNEL);
	if (!mf) {
		if (dev->exclusive)
			clear_bit(0, &dev->busy);
		return -ENOMEM;
	}
	mf->dev = dev;
	file->private_data = mf;	/* every other fop finds its state here */

	mutex_lock(&dev->lock);
	dev->openers++;
	/*
	 * `echo hi > /dev/memdev0` opens with O_WRONLY|O_TRUNC. The VFS only
	 * truncates regular files, so a device decides itself what O_TRUNC
	 * means; we make it behave like a file: forget the old contents.
	 */
	if ((file->f_flags & O_TRUNC) && (file->f_mode & FMODE_WRITE))
		dev->len = 0;
	mutex_unlock(&dev->lock);
	return 0;
}

/* Called on the LAST close of this struct file (not per close() if dup'ed). */
static int memdev_release(struct inode *inode, struct file *file)
{
	struct memdev_file *mf = file->private_data;
	struct memdev *dev = mf->dev;

	mutex_lock(&dev->lock);
	dev->openers--;
	mutex_unlock(&dev->lock);
	if (dev->exclusive)
		clear_bit(0, &dev->busy);
	kfree(mf);
	return 0;
}

/*
 * The *ppos convention: the VFS passes a pointer to the position to use
 * (&file->f_pos for read(), a local copy for pread()). We read from *ppos
 * and advance it by what we returned. Return 0 = end of file.
 */
static ssize_t memdev_read(struct file *file, char __user *buf, size_t count,
			   loff_t *ppos)
{
	struct memdev_file *mf = file->private_data;
	struct memdev *dev = mf->dev;
	ssize_t ret;
	size_t n;

	/*
	 * _interruptible: a process stuck waiting for the lock can be killed
	 * or interrupted (-ERESTARTSYS -> EINTR / automatic restart).
	 */
	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	if (*ppos >= dev->len) {
		ret = 0;				/* EOF */
		goto out;
	}
	n = min_t(size_t, count, dev->len - *ppos);

	/*
	 * copy_to_user, never memcpy: `buf` is a user address. It may be
	 * unmapped, swapped out (copy sleeps on the page fault — allowed,
	 * we hold a mutex, not a spinlock) or point into the kernel (the
	 * access_ok() inside rejects that). Returns bytes NOT copied.
	 */
	if (copy_to_user(buf, dev->data + *ppos, n)) {
		ret = -EFAULT;
		goto out;
	}
	*ppos += n;
	dev->reads++;
	mf->read_bytes += n;
	ret = n;
out:
	mutex_unlock(&dev->lock);
	return ret;
}

static ssize_t memdev_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct memdev_file *mf = file->private_data;
	struct memdev *dev = mf->dev;
	loff_t pos;
	ssize_t ret;
	size_t n;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;

	/* O_APPEND (`echo x >> /dev/memdev0`): always write at the end. */
	pos = (file->f_flags & O_APPEND) ? dev->len : *ppos;
	if (pos >= dev->size) {
		ret = -ENOSPC;		/* like a full disk: "No space left on device" */
		goto out;
	}
	n = min_t(size_t, count, dev->size - pos);	/* short write if it does not fit */

	if (copy_from_user(dev->data + pos, buf, n)) {
		ret = -EFAULT;
		goto out;
	}
	pos += n;
	if (pos > dev->len)
		dev->len = pos;
	*ppos = pos;
	dev->writes++;
	mf->write_bytes += n;
	ret = n;
out:
	mutex_unlock(&dev->lock);
	return ret;
}

static loff_t memdev_llseek(struct file *file, loff_t off, int whence)
{
	struct memdev_file *mf = file->private_data;
	struct memdev *dev = mf->dev;
	loff_t pos;

	mutex_lock(&dev->lock);		/* len/size must not change under us */
	switch (whence) {
	case SEEK_SET:
		pos = memdev_new_pos(0, off, dev->size);
		break;
	case SEEK_CUR:
		pos = memdev_new_pos(file->f_pos, off, dev->size);
		break;
	case SEEK_END:
		pos = memdev_new_pos(dev->len, off, dev->size);
		break;
	default:			/* SEEK_DATA/SEEK_HOLE: not supported */
		pos = -EINVAL;
	}
	if (pos >= 0)
		file->f_pos = pos;
	mutex_unlock(&dev->lock);
	return pos;
}

static long memdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct memdev_file *mf = file->private_data;
	struct memdev *dev = mf->dev;
	void __user *uarg = (void __user *)arg;	/* arg is a user pointer here */
	struct memdev_stats st;
	u64 v;
	int ret;

	/*
	 * Wrong magic or number: "not a typewriter" — the historical errno
	 * for "this ioctl is not for this kind of file". Not -EINVAL, which
	 * means "right command, bad argument".
	 */
	if (_IOC_TYPE(cmd) != MEMDEV_IOC_MAGIC || _IOC_NR(cmd) > MEMDEV_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case MEMDEV_IOC_CLEAR:
		mutex_lock(&dev->lock);
		memset(dev->data, 0, dev->size);
		dev->len = 0;
		mutex_unlock(&dev->lock);
		return 0;

	case MEMDEV_IOC_GET_SIZE:
		mutex_lock(&dev->lock);
		v = dev->size;
		mutex_unlock(&dev->lock);
		/* put_user: one scalar, type taken from the pointer. -EFAULT on fault. */
		return put_user(v, (u64 __user *)uarg);

	case MEMDEV_IOC_SET_SIZE:
		ret = get_user(v, (u64 __user *)uarg);
		if (ret)
			return ret;
		if (v < 1 || v > max_size)	/* validate EVERYTHING user space sends */
			return -EINVAL;
		return memdev_resize(dev, v);

	case MEMDEV_IOC_GET_STATS:
		memset(&st, 0, sizeof(st));	/* never leak kernel stack bytes */
		mutex_lock(&dev->lock);
		st.size = dev->size;
		st.len = dev->len;
		st.dev_reads = dev->reads;
		st.dev_writes = dev->writes;
		st.fd_read_bytes = mf->read_bytes;
		st.fd_write_bytes = mf->write_bytes;
		st.openers = dev->openers;
		st.exclusive = dev->exclusive;
		mutex_unlock(&dev->lock);
		if (copy_to_user(uarg, &st, sizeof(st)))
			return -EFAULT;
		return 0;
	}
	return -ENOTTY;		/* right magic, unknown number */
}

static const struct file_operations memdev_fops = {
	.owner		= THIS_MODULE,	/* open file pins the module: no rmmod */
	.open		= memdev_open,
	.release	= memdev_release,
	.read		= memdev_read,
	.write		= memdev_write,
	.llseek		= memdev_llseek,
	.unlocked_ioctl	= memdev_ioctl,
	/*
	 * 32-bit programs on this 64-bit kernel: our structs have the same
	 * layout in both ABIs, so only the pointer needs converting.
	 */
	.compat_ioctl	= compat_ptr_ioctl,
};

/*
 * devtmpfs/udev create the node with the mode returned here (default
 * 0600 root:root). 0666 lets the user-space test run without sudo — fine
 * for a RAM scratch device in a lesson; a real driver keeps the default
 * and ships a udev rule (GROUP="...", MODE="0660"), see README 3.3.
 */
static char *memdev_devnode(const struct device *dev, umode_t *mode)
{
	if (mode)
		*mode = 0666;
	return NULL;		/* NULL = keep the name "memdevN" */
}

/* ---- load-time self-test (runs on minor 0 BEFORE it goes live) ----------- */

static int memdev_selftest(struct memdev *dev)
{
	int failed = 0;

	/* llseek arithmetic, size 100: in-range, both edges, past both ends */
	if (memdev_new_pos(0, 100, 100) == 100 && memdev_new_pos(50, -50, 100) == 0 &&
	    memdev_new_pos(0, 101, 100) == -EINVAL &&
	    memdev_new_pos(10, -11, 100) == -EINVAL &&
	    memdev_new_pos(100, LLONG_MAX, 100) == -EINVAL) {
		pr_info("PASS llseek    bounds 0..size accepted, outside -> -EINVAL, no overflow\n");
	} else {
		pr_err("FAIL llseek    memdev_new_pos misbehaves\n");
		failed++;
	}

	/* resize keeps the prefix that fits, then grows with zeroes */
	memcpy(dev->data, "0123456789", 10);
	dev->len = 10;
	if (!memdev_resize(dev, 4) && dev->len == 4 && dev->size == 4 &&
	    !memcmp(dev->data, "0123", 4) &&
	    !memdev_resize(dev, buf_size) && dev->len == 4 && dev->size == buf_size &&
	    (buf_size < 5 || dev->data[4] == 0) &&
	    memdev_resize(dev, 0) == -EINVAL &&
	    memdev_resize(dev, max_size + 1) == -EINVAL) {
		pr_info("PASS resize    10 -> 4 bytes keeps \"0123\", grow back zero-fills, 0 / >max -> -EINVAL\n");
	} else {
		pr_err("FAIL resize    len=%zu size=%zu\n", dev->len, dev->size);
		failed++;
	}
	memset(dev->data, 0, dev->size);
	dev->len = 0;
	return failed;
}

/* ---- per-minor setup / teardown ------------------------------------------ */

static int memdev_setup_one(unsigned int i)
{
	struct memdev *dev = &memdevs[i];
	dev_t devt = MKDEV(MAJOR(memdev_base), i);
	struct device *d;
	int err;

	mutex_init(&dev->lock);
	dev->exclusive = (excl_minor == (int)i);
	dev->size = buf_size;
	dev->data = kvzalloc(buf_size, GFP_KERNEL);
	if (!dev->data)
		return -ENOMEM;

	/* Test internals while nobody can open the device yet. */
	if (i == 0 && memdev_selftest(dev))
		pr_err("selftest: check(s) FAILED\n");

	/*
	 * cdev_add makes the minor LIVE: an open() can arrive immediately,
	 * so everything the fops use is initialised above. device_create
	 * comes after it, so /dev/memdevN never exists without a cdev.
	 */
	cdev_init(&dev->cdev, &memdev_fops);
	dev->cdev.owner = THIS_MODULE;
	err = cdev_add(&dev->cdev, devt, 1);
	if (err)
		goto err_free;

	d = device_create(memdev_class, NULL, devt, dev, "memdev%u", i);
	if (IS_ERR(d)) {
		err = PTR_ERR(d);
		goto err_cdev;
	}
	return 0;

err_cdev:
	cdev_del(&dev->cdev);
err_free:
	kvfree(dev->data);
	return err;
}

static void memdev_destroy_one(unsigned int i)
{
	struct memdev *dev = &memdevs[i];

	device_destroy(memdev_class, MKDEV(MAJOR(memdev_base), i));
	cdev_del(&dev->cdev);
	kvfree(dev->data);
	mutex_destroy(&dev->lock);
}

/* ---- module init / exit -------------------------------------------------- */

static int __init memdev_init(void)
{
	unsigned int i;
	int err;

	if (nr_devs < 1 || nr_devs > MEMDEV_MAX_DEVS ||
	    max_size < 4096 || max_size > MEMDEV_MAX_SIZE ||
	    buf_size < 1 || buf_size > max_size ||
	    excl_minor < -1 || excl_minor >= (int)nr_devs) {
		pr_err("bad parameters: nr_devs=%u buf_size=%lu max_size=%lu excl_minor=%d\n",
		       nr_devs, buf_size, max_size, excl_minor);
		return -EINVAL;
	}

	memdevs = kcalloc(nr_devs, sizeof(*memdevs), GFP_KERNEL);
	if (!memdevs)
		return -ENOMEM;

	/*
	 * Dynamic major: the kernel picks a free one (see /proc/devices).
	 * register_chrdev_region(MKDEV(240, 0), ...) would ask for a fixed
	 * one — only for numbers listed in Documentation/admin-guide/devices.txt.
	 */
	err = alloc_chrdev_region(&memdev_base, 0, nr_devs, "memdev");
	if (err)
		goto err_free;

	memdev_class = class_create("memdev");	/* 6.4+: no THIS_MODULE arg */
	if (IS_ERR(memdev_class)) {
		err = PTR_ERR(memdev_class);
		goto err_region;
	}
	memdev_class->devnode = memdev_devnode;

	for (i = 0; i < nr_devs; i++) {
		err = memdev_setup_one(i);
		if (err)
			goto err_devs;
	}

	pr_info("major %d, minors 0..%u -> /dev/memdev0..%u, %lu bytes each (max %lu), exclusive minor %d\n",
		MAJOR(memdev_base), nr_devs - 1, nr_devs - 1, buf_size, max_size,
		excl_minor);
	return 0;

err_devs:
	while (i--)			/* undo only the minors that succeeded */
		memdev_destroy_one(i);
	class_destroy(memdev_class);
err_region:
	unregister_chrdev_region(memdev_base, nr_devs);
err_free:
	kfree(memdevs);
	return err;
}

static void __exit memdev_exit(void)
{
	unsigned int i;

	/* fops.owner guarantees no file is open: rmmod fails with EBUSY first. */
	for (i = 0; i < nr_devs; i++)
		memdev_destroy_one(i);
	class_destroy(memdev_class);
	unregister_chrdev_region(memdev_base, nr_devs);
	kfree(memdevs);
	pr_info("unloaded\n");
}

module_init(memdev_init);
module_exit(memdev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("/dev/memdevN: char device with read/write/llseek/ioctl on a resizable buffer");
