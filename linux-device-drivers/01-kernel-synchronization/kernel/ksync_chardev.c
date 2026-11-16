// SPDX-License-Identifier: GPL-2.0
/*
 * ksync_chardev.c — /dev/ksync: how a driver implements blocking read(),
 *                   O_NONBLOCK and poll() with a wait queue
 *
 * Concept : A byte FIFO (kfifo) sits between "the hardware" and user space.
 *           Producers: write() from user space, and optionally a timer that
 *           plays the role of a device interrupt (tick_ms=...). Consumer:
 *           read(). The FIFO is shared with softirq context, so it is
 *           protected by a spinlock taken with spin_lock_bh() in process
 *           context. Readers that find it empty sleep on a wait queue;
 *           every producer calls wake_up_interruptible() after adding data.
 *           poll()/select()/epoll use the SAME wait queue via poll_wait().
 * Build   : make drivers        (or: make -C linux-device-drivers/01-kernel-synchronization/kernel)
 * Run     : sudo insmod ksync_chardev.ko [tick_ms=500]
 *           sudo ./build/linux-device-drivers/01-kernel-synchronization/user/ksync_poll
 *           echo hi | sudo tee /dev/ksync ; sudo cat /dev/ksync   (Ctrl-C to stop)
 *           sudo rmmod ksync_chardev
 * Read    : ../README.md  (section 3.5 "Waiting: completions and wait queues")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#define KSYNC_FIFO_SIZE 4096	/* must be a power of two for kfifo */
#define KSYNC_CHUNK 256		/* bounce buffer: max bytes per read/write call */

static unsigned int tick_ms;
module_param(tick_ms, uint, 0444);
MODULE_PARM_DESC(tick_ms, "if > 0, a timer pushes \"tick N\\n\" every tick_ms ms (fake IRQ source)");

/*
 *  user write() --+                                   +--> user read()
 *                 |   spin_lock_bh(&lock)             |    (sleeps on wq
 *                 +-->  [ kfifo, 4 KiB ]  ------------+     when empty)
 *  timer (softirq)+   spin_lock(&lock)                |
 *                     wake_up_interruptible(&wq) -----+--> poll()/epoll
 */
static struct ksync_dev {
	spinlock_t lock;			/* protects fifo */
	DECLARE_KFIFO(fifo, char, KSYNC_FIFO_SIZE);
	wait_queue_head_t wq;			/* readers AND writers wait here */
	struct timer_list tick;
	unsigned long ticks;			/* only touched by tick_fn */
} kdev;

/* Locked helpers: used as wait_event conditions, so they must not sleep. */
static unsigned int fifo_len(void)
{
	unsigned int n;

	spin_lock_bh(&kdev.lock);
	n = kfifo_len(&kdev.fifo);
	spin_unlock_bh(&kdev.lock);
	return n;
}

static unsigned int fifo_avail(void)
{
	unsigned int n;

	spin_lock_bh(&kdev.lock);
	n = kfifo_avail(&kdev.fifo);
	spin_unlock_bh(&kdev.lock);
	return n;
}

/* The "interrupt handler": softirq context, must not sleep. */
static void tick_fn(struct timer_list *t)
{
	char line[32];
	unsigned int len;

	len = scnprintf(line, sizeof(line), "tick %lu\n", ++kdev.ticks);

	/*
	 * Softirqs do not nest on one CPU and no hardirq takes the lock:
	 * plain spin_lock() is enough here.
	 */
	spin_lock(&kdev.lock);
	if (kfifo_avail(&kdev.fifo) >= len)
		kfifo_in(&kdev.fifo, line, len);	/* else drop: "overrun" */
	spin_unlock(&kdev.lock);

	wake_up_interruptible(&kdev.wq);		/* fine from softirq */
	mod_timer(&kdev.tick, jiffies + msecs_to_jiffies(tick_ms));
}

static ssize_t ksync_read(struct file *file, char __user *buf, size_t count,
			  loff_t *ppos)
{
	char kbuf[KSYNC_CHUNK];
	unsigned int n;
	int ret;

	if (!count)
		return 0;
	count = min_t(size_t, count, sizeof(kbuf));

	for (;;) {
		spin_lock_bh(&kdev.lock);
		n = kfifo_out(&kdev.fifo, kbuf, count);
		spin_unlock_bh(&kdev.lock);
		if (n)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		/*
		 * Sleep until data arrives or a signal comes. Returns
		 * -ERESTARTSYS on a signal: the syscall layer turns that into
		 * EINTR or transparently restarts the read (SA_RESTART).
		 * Another reader may take the data first -> loop and re-check.
		 */
		ret = wait_event_interruptible(kdev.wq, fifo_len() > 0);
		if (ret)
			return ret;
	}

	wake_up_interruptible(&kdev.wq);	/* room freed: wake writers */

	/*
	 * copy_to_user() may fault and sleep, so it is done AFTER dropping
	 * the spinlock, from the stack bounce buffer. (If it fails the bytes
	 * are lost — acceptable for a demo; a real driver could peek first
	 * with kfifo_out_peek() and skip only after a successful copy.)
	 */
	if (copy_to_user(buf, kbuf, n))
		return -EFAULT;
	return n;
}

static ssize_t ksync_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	char kbuf[KSYNC_CHUNK];
	unsigned int n;
	int ret;

	if (!count)
		return 0;
	count = min_t(size_t, count, sizeof(kbuf));
	if (copy_from_user(kbuf, buf, count))	/* may sleep: no lock held */
		return -EFAULT;

	for (;;) {
		spin_lock_bh(&kdev.lock);
		n = kfifo_in(&kdev.fifo, kbuf, count);	/* partial write OK */
		spin_unlock_bh(&kdev.lock);
		if (n)
			break;

		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(kdev.wq, fifo_avail() > 0);
		if (ret)
			return ret;
	}

	wake_up_interruptible(&kdev.wq);	/* data added: wake readers */
	return n;
}

/*
 * poll() does NOT sleep itself. It registers the caller on our wait queue
 * (poll_wait) and reports what is possible RIGHT NOW. The core poll/epoll
 * code sleeps and calls us again after any wake_up on kdev.wq.
 */
static __poll_t ksync_poll(struct file *file, poll_table *wait)
{
	__poll_t mask = 0;

	poll_wait(file, &kdev.wq, wait);

	spin_lock_bh(&kdev.lock);
	if (!kfifo_is_empty(&kdev.fifo))
		mask |= EPOLLIN | EPOLLRDNORM;
	if (!kfifo_is_full(&kdev.fifo))
		mask |= EPOLLOUT | EPOLLWRNORM;
	spin_unlock_bh(&kdev.lock);
	return mask;
}

static const struct file_operations ksync_fops = {
	.owner	= THIS_MODULE,		/* no rmmod while a file is open */
	.read	= ksync_read,
	.write	= ksync_write,
	.poll	= ksync_poll,
	.llseek	= noop_llseek,
};

static struct miscdevice ksync_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ksync",		/* -> /dev/ksync */
	.fops	= &ksync_fops,
	.mode	= 0600,
};

static int __init ksync_chardev_init(void)
{
	int err;

	/*
	 * Everything the fops and the timer touch must exist BEFORE
	 * misc_register(): the device can be opened the moment it returns.
	 */
	spin_lock_init(&kdev.lock);
	INIT_KFIFO(kdev.fifo);
	init_waitqueue_head(&kdev.wq);
	timer_setup(&kdev.tick, tick_fn, 0);

	err = misc_register(&ksync_misc);
	if (err) {
		pr_err("misc_register failed: %d\n", err);
		return err;
	}
	if (tick_ms)
		mod_timer(&kdev.tick, jiffies + msecs_to_jiffies(tick_ms));

	pr_info("/dev/ksync ready (fifo %d bytes, tick_ms=%u)\n",
		KSYNC_FIFO_SIZE, tick_ms);
	return 0;
}

static void __exit ksync_chardev_exit(void)
{
	/* Reverse order of init. No file can be open here (fops.owner). */
	timer_shutdown_sync(&kdev.tick);
	misc_deregister(&ksync_misc);
	pr_info("unloaded after %lu ticks\n", kdev.ticks);
}

module_init(ksync_chardev_init);
module_exit(ksync_chardev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("/dev/ksync: blocking read, O_NONBLOCK and poll() with a wait queue");
