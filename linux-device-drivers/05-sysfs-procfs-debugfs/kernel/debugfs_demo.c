// SPDX-License-Identifier: GPL-2.0
/*
 * debugfs_demo.c — /sys/kernel/debug/esh_debugfs: the debugfs helpers for
 *                  plain variables (u32, bool, x32, atomic_t), a read-only
 *                  blob, and a custom file with its own read and write
 *
 * Concept : debugfs is the kernel's scratch pad for developers: no ABI
 *           promise, no "one value per file" rule, root only. One call per
 *           variable gives you a file that reads and writes it directly
 *           (debugfs_create_u32 & co). A sampler (delayed work) runs every
 *           period_ms while "enabled" is 1, counts samples and how many
 *           exceeded "threshold", so you can watch the knobs take effect.
 *           "state" is a custom file: seq_file read, and write "reset".
 *           debugfs_remove() on the directory removes everything inside.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/05-sysfs-procfs-debugfs/kernel)
 * Run     : sudo insmod debugfs_demo.ko [period_ms=100]
 *           sudo ls -l /sys/kernel/debug/esh_debugfs
 *           sudo cat /sys/kernel/debug/esh_debugfs/state
 *           echo 1 | sudo tee /sys/kernel/debug/esh_debugfs/enabled
 *           sudo rmmod debugfs_demo
 * Read    : ../README.md  (section 3.8 "debugfs")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/debugfs.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

static unsigned int period_ms = 100;
module_param(period_ms, uint, 0444);
MODULE_PARM_DESC(period_ms, "sampler period in ms (10..10000, default 100)");

/*
 *  /sys/kernel/debug/esh_debugfs/
 *  |-- enabled     bool    rw  sampler on/off ("Y"/"N", accepts 1/0)
 *  |-- threshold   u32     rw  decimal
 *  |-- flags       x32     rw  hex, "0x0000000f"
 *  |-- samples     atomic  r-  samples taken
 *  |-- over        atomic  r-  samples > threshold
 *  |-- banner      blob    r-  fixed bytes
 *  `-- state       custom  rw  multi-line summary; write "reset"
 *
 * The helpers read and write the variables WITHOUT locking: fine for
 * single aligned words, which is all they are for. Anything that needs
 * consistency between fields goes in a custom file (like "state").
 */
static bool enabled;
static u32 threshold = 900;	/* sample is a random 0..999 value */
static u32 flags = 0x5;
static atomic_t samples = ATOMIC_INIT(0);
static atomic_t over = ATOMIC_INIT(0);
static u32 last_sample;

static const char banner_text[] = "esh_debugfs: debugfs demo for the BeagleBone AI-64\n";
static struct debugfs_blob_wrapper banner = {
	.data = (void *)banner_text,	/* read-only file (0444): never written */
	.size = sizeof(banner_text) - 1,	/* without the NUL */
};

static struct dentry *esh_dir;
static struct delayed_work sampler;

/* A fake sensor read, every period_ms, in process context (workqueue). */
static void sampler_fn(struct work_struct *work)
{
	if (READ_ONCE(enabled)) {
		u32 v = get_random_u32_below(1000);

		WRITE_ONCE(last_sample, v);
		atomic_inc(&samples);
		if (v > READ_ONCE(threshold))
			atomic_inc(&over);
	}
	/* Re-arm; cancel_delayed_work_sync() in exit stops the cycle. */
	schedule_delayed_work(&sampler, msecs_to_jiffies(period_ms));
}

/* ---- the custom file ---------------------------------------------------- */
static int state_show(struct seq_file *m, void *unused)
{
	int s = atomic_read(&samples), o = atomic_read(&over);

	seq_printf(m, "enabled:   %s\n", str_yes_no(READ_ONCE(enabled)));
	seq_printf(m, "period_ms: %u\n", period_ms);
	seq_printf(m, "threshold: %u\n", READ_ONCE(threshold));
	seq_printf(m, "flags:     0x%08x\n", READ_ONCE(flags));
	seq_printf(m, "samples:   %d\n", s);
	seq_printf(m, "over:      %d (%d%%)\n", o, s ? o * 100 / s : 0);
	seq_printf(m, "last:      %u\n", READ_ONCE(last_sample));
	return 0;
}

static int state_open(struct inode *inode, struct file *file)
{
	return single_open(file, state_show, inode->i_private);
}

static ssize_t state_write(struct file *file, const char __user *ubuf,
			   size_t count, loff_t *ppos)
{
	char cmd[16];

	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	if (strcmp(strim(cmd), "reset"))
		return -EINVAL;
	atomic_set(&samples, 0);
	atomic_set(&over, 0);
	return count;
}

/*
 * Plain file_operations (debugfs is not procfs). For a read-only seq_file
 * the DEFINE_SHOW_ATTRIBUTE(state) macro would generate all of this; we
 * need .write too, so it is spelled out.
 */
static const struct file_operations state_fops = {
	.owner = THIS_MODULE,
	.open = state_open,
	.read = seq_read,
	.write = state_write,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init debugfs_demo_init(void)
{
	if (period_ms < 10 || period_ms > 10000) {
		pr_err("period_ms must be 10..10000\n");
		return -EINVAL;
	}

	/*
	 * No error checks on the debugfs calls, on purpose: they return an
	 * ERR_PTR on failure (or if debugfs is disabled/locked down), every
	 * debugfs function accepts that ERR_PTR as a parent and does nothing,
	 * and a driver must keep working without its debug files. Never make
	 * probe() fail because debugfs did.
	 */
	esh_dir = debugfs_create_dir("esh_debugfs", NULL);
	debugfs_create_bool("enabled", 0644, esh_dir, &enabled);
	debugfs_create_u32("threshold", 0644, esh_dir, &threshold);
	debugfs_create_x32("flags", 0644, esh_dir, &flags);
	debugfs_create_atomic_t("samples", 0444, esh_dir, &samples);
	debugfs_create_atomic_t("over", 0444, esh_dir, &over);
	debugfs_create_blob("banner", 0444, esh_dir, &banner);
	debugfs_create_file("state", 0644, esh_dir, NULL, &state_fops);

	INIT_DELAYED_WORK(&sampler, sampler_fn);
	schedule_delayed_work(&sampler, msecs_to_jiffies(period_ms));

	pr_info("created /sys/kernel/debug/esh_debugfs (sampler every %u ms, enable it with the 'enabled' file)\n",
		period_ms);
	return 0;
}

static void __exit debugfs_demo_exit(void)
{
	/*
	 * Files first: debugfs_remove() waits for running file operations
	 * (debugfs "file removal protection"), so no reader touches the
	 * variables afterwards. Then stop the work: _sync waits for a running
	 * sampler_fn and blocks its re-arming.
	 */
	debugfs_remove(esh_dir);	/* recursive since 5.x */
	cancel_delayed_work_sync(&sampler);
	pr_info("unloaded after %d samples\n", atomic_read(&samples));
}

module_init(debugfs_demo_init);
module_exit(debugfs_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("debugfs: u32/bool/x32/atomic/blob helpers and a custom file");
