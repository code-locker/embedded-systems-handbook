// SPDX-License-Identifier: GPL-2.0
/*
 * kmb_initfail.c — init that acquires 4 resources and can be told to fail
 *                  at any step, to show the goto unwind ladder
 *
 * Concept : If module_init returns a negative errno, the load fails, the
 *           module is freed and module_exit is NEVER called. So init must
 *           release everything it acquired before failing — in reverse
 *           order. The kernel idiom is the goto ladder: one label per
 *           resource, falling through to the earlier ones. Load with
 *           fail_at=N to make step N fail; the log shows exactly what is
 *           undone, /proc/kmb_initfail is gone again, and there is nothing
 *           to rmmod (lsmod does not list the module).
 * Build   : make drivers
 * Run     : sudo insmod kmb_initfail.ko               # all 4 steps, then rmmod
 *           sudo insmod kmb_initfail.ko fail_at=3     # "Cannot allocate memory"
 *           sudo insmod kmb_initfail.ko fail_at=4     # "Input/output error"
 *           sudo insmod kmb_initfail.ko fail_at=9     # "Invalid argument"
 * Read    : ../README.md  (section 3.5 "When init fails: error codes and unwinding")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/completion.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/workqueue.h>

#define KMB_STEPS 4

static unsigned int fail_at;
module_param(fail_at, uint, 0444);
MODULE_PARM_DESC(fail_at, "0 = succeed, 1..4 = make step N fail");

static unsigned int buf_size = 4096;
module_param(buf_size, uint, 0444);
MODULE_PARM_DESC(buf_size, "step 1 allocation size in bytes (64..65536)");

/*
 *  step 1  kzalloc buffer          -ENOMEM   <- needed by the work item
 *  step 2  ordered workqueue       -ENOMEM   <- runs the work item
 *  step 3  /proc/kmb_initfail      -ENOMEM   <- reads the buffer
 *  step 4  self-check via the work -EIO      <- uses 1+2
 *
 *  unwind = the same list bottom-up:
 *  err_proc: proc_remove   err_wq: destroy_workqueue   err_buf: kfree
 */
static struct kmb_initfail {
	u8 *buf;
	struct workqueue_struct *wq;
	struct proc_dir_entry *proc;
	struct work_struct work;
	struct completion done;
	u32 checksum;
	int work_cpu;
} kif;

/* Runs in a kworker thread (process context, may sleep). */
static void kmb_fill_work(struct work_struct *w)
{
	struct kmb_initfail *k = container_of(w, struct kmb_initfail, work);
	u32 i, sum = 0;

	for (i = 0; i < buf_size; i++) {
		k->buf[i] = (u8)i;		/* touches step 1's buffer */
		sum += k->buf[i];
	}
	k->checksum = sum;
	k->work_cpu = raw_smp_processor_id();
	complete(&k->done);
}

static int kmb_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "steps completed: %d\nbuffer: %u bytes, checksum %u\nwork ran on CPU %d\n",
		   KMB_STEPS, buf_size, kif.checksum, kif.work_cpu);
	return 0;
}

/* Tells the log which step failed and returns its error code. */
static int kmb_inject(unsigned int step, int err)
{
	if (fail_at != step)
		return 0;
	pr_warn("step %u: injected failure (%d)\n", step, err);
	return err;
}

static int __init kmb_initfail_init(void)
{
	u32 expect = 0, i;
	int err;

	/* Step 0: reject bad parameters before acquiring anything. */
	if (fail_at > KMB_STEPS || buf_size < 64 || buf_size > 65536) {
		pr_err("fail_at=%u (0..%d) or buf_size=%u (64..65536) out of range\n",
		       fail_at, KMB_STEPS, buf_size);
		return -EINVAL;		/* insmod: "Invalid argument" */
	}
	init_completion(&kif.done);
	INIT_WORK(&kif.work, kmb_fill_work);

	/* Step 1 */
	err = kmb_inject(1, -ENOMEM);
	if (!err) {
		kif.buf = kzalloc(buf_size, GFP_KERNEL);
		if (!kif.buf)
			err = -ENOMEM;
	}
	if (err)
		goto err_out;		/* nothing acquired yet */
	pr_info("step 1: buffer of %u bytes allocated\n", buf_size);

	/* Step 2 */
	err = kmb_inject(2, -ENOMEM);
	if (!err) {
		kif.wq = alloc_ordered_workqueue("kmb_initfail", 0);
		if (!kif.wq)
			err = -ENOMEM;
	}
	if (err)
		goto err_buf;
	pr_info("step 2: workqueue created\n");

	/* Step 3 */
	err = kmb_inject(3, -ENOMEM);
	if (!err) {
		kif.proc = proc_create_single("kmb_initfail", 0444, NULL,
					      kmb_proc_show);
		if (!kif.proc)
			err = -ENOMEM;
	}
	if (err)
		goto err_wq;
	pr_info("step 3: /proc/kmb_initfail created\n");

	/* Step 4: use resources 1 + 2 and verify the result. */
	queue_work(kif.wq, &kif.work);
	if (!wait_for_completion_timeout(&kif.done, msecs_to_jiffies(2000))) {
		err = -ETIMEDOUT;
		goto err_proc;
	}
	for (i = 0; i < buf_size; i++)
		expect += (u8)i;
	err = kmb_inject(4, -EIO);
	if (!err && kif.checksum != expect)
		err = -EIO;
	if (err)
		goto err_proc;
	pr_info("step 4: self-check ok (checksum %u, work ran on CPU %d)\n",
		kif.checksum, kif.work_cpu);

	pr_info("PASS init      all %d steps acquired; rmmod releases them in reverse\n",
		KMB_STEPS);
	return 0;

	/*
	 * The ladder. Each label undoes ONE step; execution then continues into
	 * labels below it, so a jump to label N undoes steps N..1.
	 * Order matters: the work item writes into buf, so the workqueue is
	 * destroyed (which waits for queued work) BEFORE buf is freed.
	 */
err_proc:
	proc_remove(kif.proc);
	pr_info("unwind: /proc/kmb_initfail removed\n");
err_wq:
	destroy_workqueue(kif.wq);
	pr_info("unwind: workqueue destroyed\n");
err_buf:
	kfree(kif.buf);
	pr_info("unwind: buffer freed\n");
err_out:
	pr_info("PASS unwind    init returns %d: module not loaded, nothing leaked, no rmmod needed\n",
		err);
	return err;
}

static void __exit kmb_initfail_exit(void)
{
	/* Only reached after a SUCCESSFUL init: undo all steps, reversed. */
	proc_remove(kif.proc);
	destroy_workqueue(kif.wq);
	kfree(kif.buf);
	pr_info("unloaded: released /proc entry, workqueue, buffer\n");
}

module_init(kmb_initfail_init);
module_exit(kmb_initfail_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("Module basics: failing init cleanly with a goto unwind ladder");
MODULE_VERSION("1.0");
