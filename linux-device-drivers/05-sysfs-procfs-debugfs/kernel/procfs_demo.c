// SPDX-License-Identifier: GPL-2.0
/*
 * procfs_demo.c — /proc/esh_procfs/{items,stats,ctl}: seq_file iteration
 *                 over a list, single_open() for a small file, and a
 *                 writable control entry
 *
 * Concept : A proc file's read() must cope with output bigger than one
 *           buffer and with readers that read a few bytes at a time.
 *           seq_file does that for you: you write start/next/stop/show
 *           over YOUR data, it fills pages, remembers the position and
 *           calls start() again for the next read(). "items" lists an
 *           internal linked list that way (bigger than a page with the
 *           default 200 items). "stats" is small, so single_open() with
 *           one show() is enough. "ctl" accepts commands written to it
 *           (add NAME VALUE / del ID / clear).
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/05-sysfs-procfs-debugfs/kernel)
 * Run     : sudo insmod procfs_demo.ko [nitems=200]
 *           cat /proc/esh_procfs/items /proc/esh_procfs/stats
 *           echo "add sensor0 77" | sudo tee /proc/esh_procfs/ctl
 *           sudo ./build/linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_procfs_test
 *           sudo rmmod procfs_demo
 * Read    : ../README.md  (section 3.7 "procfs and seq_file")
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define ITEM_NAME_LEN 24
#define ITEMS_MAX 100000

static int nitems = 200;
module_param(nitems, int, 0444);
MODULE_PARM_DESC(nitems, "items created at load (0..10000, default 200)");

struct esh_item {
	struct list_head node;
	unsigned int id;
	char name[ITEM_NAME_LEN];
	u64 value;
};

static LIST_HEAD(items);
static DEFINE_MUTEX(items_lock);	/* protects items, next_id, nr_items, stats */
static unsigned int next_id = 1;
static unsigned int nr_items;
static struct {
	unsigned long adds, dels, seq_starts, items_opens;
} stats;

static struct proc_dir_entry *esh_dir;

/* Caller holds items_lock. Owner of the new item: the items list. */
static int item_add_locked(const char *name, u64 value)
{
	struct esh_item *it;

	if (nr_items >= ITEMS_MAX)
		return -ENOSPC;
	it = kzalloc(sizeof(*it), GFP_KERNEL);
	if (!it)
		return -ENOMEM;
	it->id = next_id++;
	strscpy(it->name, name, sizeof(it->name));
	it->value = value;
	list_add_tail(&it->node, &items);
	nr_items++;
	stats.adds++;
	return 0;
}

static void items_clear_locked(void)
{
	struct esh_item *it, *tmp;

	list_for_each_entry_safe(it, tmp, &items, node) {
		list_del(&it->node);
		kfree(it);
		nr_items--;
		stats.dels++;
	}
}

/* ------------------------------------------------------------------------ */
/* items: seq_operations                                                     */
/* ------------------------------------------------------------------------ */
/*
 * One read() system call (user buffer of any size):
 *
 *   start(pos) -> show(v) -> next(v, &pos) -> show -> ... -> stop()
 *     ^ lock      fills seq_file's page buffer             ^ unlock
 *
 * When the page is full, seq_file stops, copies to user space, and the
 * NEXT read() calls start(pos) again, so start() must be able to find
 * element number pos. The lock is held only from start() to stop(), never
 * across two read() calls: the list may change in between, and a reader
 * may see an item twice or miss one. That is the procfs contract
 * ("a snapshot per read()", not per file).
 *
 * seq_list_start_head() treats position 0 as the list head itself, which
 * we use to print a header line.
 */
static void *items_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&items_lock);
	stats.seq_starts++;
	return seq_list_start_head(&items, *pos);
}

static void *items_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, &items, pos);
}

static void items_stop(struct seq_file *m, void *v)
{
	mutex_unlock(&items_lock);
}

static int items_show(struct seq_file *m, void *v)
{
	const struct esh_item *it;

	if (v == &items) {		/* position 0: the head -> header */
		seq_printf(m, "%4s %-24s %8s\n", "id", "name", "value");
		return 0;
	}
	it = list_entry(v, struct esh_item, node);
	seq_printf(m, "%4u %-24s %8llu\n", it->id, it->name, it->value);
	return 0;
}

static const struct seq_operations items_seq_ops = {
	.start = items_start,
	.next = items_next,
	.stop = items_stop,
	.show = items_show,
};

static int items_open(struct inode *inode, struct file *file)
{
	mutex_lock(&items_lock);
	stats.items_opens++;
	mutex_unlock(&items_lock);
	return seq_open(file, &items_seq_ops);
}

/* proc_ops (5.6+), not file_operations: procfs has its own, smaller table. */
static const struct proc_ops items_proc_ops = {
	.proc_open = items_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = seq_release,
};

/* ------------------------------------------------------------------------ */
/* stats: single_open — one show() prints the whole (small) file             */
/* ------------------------------------------------------------------------ */
static int stats_show(struct seq_file *m, void *unused)
{
	mutex_lock(&items_lock);
	seq_printf(m, "items:      %u\n", nr_items);
	seq_printf(m, "next_id:    %u\n", next_id);
	seq_printf(m, "adds:       %lu\n", stats.adds);
	seq_printf(m, "dels:       %lu\n", stats.dels);
	seq_printf(m, "opens:      %lu\n", stats.items_opens);
	seq_printf(m, "seq_starts: %lu\n", stats.seq_starts);
	mutex_unlock(&items_lock);
	return 0;
}

static int stats_open(struct inode *inode, struct file *file)
{
	/*
	 * single_open allocates one buffer and calls stats_show() once; if
	 * the output does not fit, seq_file retries with a bigger buffer.
	 * Fine for a few lines, wasteful for thousands (use seq_operations).
	 */
	return single_open(file, stats_show, NULL);
}

static const struct proc_ops stats_proc_ops = {
	.proc_open = stats_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------------ */
/* ctl: writable entry                                                       */
/* ------------------------------------------------------------------------ */
static int ctl_show(struct seq_file *m, void *unused)
{
	seq_puts(m, "commands: add NAME VALUE | del ID | clear\n");
	return 0;
}

static int ctl_open(struct inode *inode, struct file *file)
{
	return single_open(file, ctl_show, NULL);
}

static ssize_t ctl_write(struct file *file, const char __user *ubuf,
			 size_t count, loff_t *ppos)
{
	char cmd[64], name[ITEM_NAME_LEN];
	struct esh_item *it;
	unsigned long long value;
	unsigned int id;
	int err = -EINVAL;

	if (count == 0 || count >= sizeof(cmd))
		return -EINVAL;
	/* User memory: never dereference ubuf directly. */
	if (copy_from_user(cmd, ubuf, count))
		return -EFAULT;
	cmd[count] = '\0';
	strim(cmd);			/* drop echo's trailing '\n' */

	mutex_lock(&items_lock);
	/* %23s: field width = sizeof(name) - 1, or sscanf overflows name */
	if (sscanf(cmd, "add %23s %llu", name, &value) == 2) {
		err = item_add_locked(name, value);
	} else if (sscanf(cmd, "del %u", &id) == 1) {
		err = -ENOENT;
		list_for_each_entry(it, &items, node) {
			if (it->id == id) {
				list_del(&it->node);
				kfree(it);
				nr_items--;
				stats.dels++;
				err = 0;
				break;	/* stop: "it" is freed, don't advance */
			}
		}
	} else if (!strcmp(cmd, "clear")) {
		items_clear_locked();
		err = 0;
	}
	mutex_unlock(&items_lock);
	return err ? err : count;
}

static const struct proc_ops ctl_proc_ops = {
	.proc_open = ctl_open,
	.proc_read = seq_read,
	.proc_write = ctl_write,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

/* ------------------------------------------------------------------------ */

static int __init procfs_demo_init(void)
{
	int i, err = 0;

	if (nitems < 0 || nitems > 10000) {
		pr_err("nitems must be 0..10000\n");
		return -EINVAL;
	}

	mutex_lock(&items_lock);
	for (i = 0; i < nitems && !err; i++) {
		char name[ITEM_NAME_LEN];

		snprintf(name, sizeof(name), "item-%d", i);
		err = item_add_locked(name, (u64)i * i);
	}
	mutex_unlock(&items_lock);
	if (err)
		goto err_items;

	esh_dir = proc_mkdir("esh_procfs", NULL);
	if (!esh_dir) {
		err = -ENOMEM;
		goto err_items;
	}
	/*
	 * proc_create() returns NULL on failure. Files appear immediately,
	 * so all data they show must be ready before this point.
	 */
	if (!proc_create("items", 0444, esh_dir, &items_proc_ops) ||
	    !proc_create("stats", 0444, esh_dir, &stats_proc_ops) ||
	    !proc_create("ctl", 0644, esh_dir, &ctl_proc_ops)) {
		err = -ENOMEM;
		goto err_dir;
	}
	pr_info("created /proc/esh_procfs/{items,stats,ctl} with %d items\n", nitems);
	return 0;

err_dir:
	proc_remove(esh_dir);		/* removes the dir and what was created in it */
err_items:
	mutex_lock(&items_lock);
	items_clear_locked();
	mutex_unlock(&items_lock);
	return err;
}

static void __exit procfs_demo_exit(void)
{
	/*
	 * proc_remove() (= remove_proc_subtree) waits for every open/read/
	 * write already running in our proc_ops to finish, and later calls
	 * fail. Only then may the data they use be freed.
	 */
	proc_remove(esh_dir);
	mutex_lock(&items_lock);
	items_clear_locked();
	mutex_unlock(&items_lock);
	pr_info("unloaded\n");
}

module_init(procfs_demo_init);
module_exit(procfs_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("procfs: seq_file list, single_open stats, writable ctl entry");
