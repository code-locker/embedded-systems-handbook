// SPDX-License-Identifier: GPL-2.0
/*
 * sysfs_demo.c — sysfs attributes two ways: on a bare kobject under
 *                /sys/kernel/esh_sysfs, and on a misc device through its
 *                attribute groups; plus sysfs_notify() for poll() and a
 *                uevent that udev can react to
 *
 * Concept : sysfs is "one value per file". You give the kernel a show()
 *           that formats the value into a PAGE_SIZE buffer and a store()
 *           that parses what user space wrote. Part 1 creates a kobject
 *           (a directory) with an int, a string, an enum, a write-only
 *           "trigger" and a read-only "event_count" that user space can
 *           poll() on: writing trigger calls sysfs_notify(). It also has
 *           one binary attribute. Part 2 registers /dev/esh_sysfs with
 *           DEVICE_ATTR_RW/RO/WO attributes passed in miscdevice.groups,
 *           so they exist BEFORE the "add" uevent reaches udev.
 * Build   : make drivers
 *           (or: make -C linux-device-drivers/05-sysfs-procfs-debugfs/kernel)
 * Run     : sudo insmod sysfs_demo.ko
 *           ls /sys/kernel/esh_sysfs /sys/class/misc/esh_sysfs
 *           sudo ./build/linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_sysfs_test
 *           sudo rmmod sysfs_demo
 * Read    : ../README.md  (sections 3.2 to 3.6)
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/atomic.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define ESH_VALUE_MIN (-1000)
#define ESH_VALUE_MAX 1000
#define ESH_NAME_LEN 32

/*
 *  /sys/kernel/esh_sysfs/            <- kobject_create_and_add("esh_sysfs", kernel_kobj)
 *  |-- value        rw  int, -1000..1000
 *  |-- name         rw  string, < 32 chars
 *  |-- mode         rw  enum: "off [slow] fast"
 *  |-- trigger      -w  any write: event_count++, sysfs_notify, uevent
 *  |-- event_count  r-  poll() wakes up when it changes
 *  `-- raw          r-  binary attribute: struct esh_raw, 48 bytes
 *
 *  /sys/devices/virtual/misc/esh_sysfs/  (= /sys/class/misc/esh_sysfs)
 *  |-- counter      rw  unsigned long, read(/dev/esh_sysfs) increments it
 *  |-- info         r-  "minor N, opens M"
 *  |-- reset        -w  write 1 to zero the counter
 *  |-- dev, uevent, subsystem, power/   (made by the driver core)
 */
static const char * const esh_modes[] = { "off", "slow", "fast" };

static struct {
	struct mutex lock;		/* protects everything below */
	int value;
	char name[ESH_NAME_LEN];
	int mode;			/* index into esh_modes */
	unsigned long counter;
	unsigned int opens;
} st = {
	.lock = __MUTEX_INITIALIZER(st.lock),
	.name = "esh",
	.mode = 1,
};

static atomic_t event_count = ATOMIC_INIT(0);
static struct kobject *esh_kobj;

/* ------------------------------------------------------------------------ */
/* Part 1: attributes on a bare kobject                                     */
/* ------------------------------------------------------------------------ */
/*
 * show(): fill buf (one PAGE_SIZE page, zeroed) and return the length.
 *         Use sysfs_emit()/sysfs_emit_at(): they know buf is a page and
 *         never overflow it (sprintf/snprintf with PAGE_SIZE are the old way).
 * store(): buf holds what was written (NUL-terminated, at most PAGE_SIZE-1
 *         bytes, usually with a trailing '\n' from echo). Parse it, and
 *         return count (all consumed) or a negative errno that write()
 *         returns to user space. Returning less than count makes user
 *         space write the rest again: don't.
 */
static ssize_t value_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	int v;

	mutex_lock(&st.lock);
	v = st.value;
	mutex_unlock(&st.lock);
	return sysfs_emit(buf, "%d\n", v);
}

static ssize_t value_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	int v, err;

	/* kstrtoint accepts one trailing '\n', rejects "12abc" and overflow. */
	err = kstrtoint(buf, 0, &v);
	if (err)
		return err;
	if (v < ESH_VALUE_MIN || v > ESH_VALUE_MAX)
		return -EINVAL;		/* range check: sysfs is an ABI */
	mutex_lock(&st.lock);
	st.value = v;
	mutex_unlock(&st.lock);
	return count;
}

static ssize_t name_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	ssize_t n;

	mutex_lock(&st.lock);
	n = sysfs_emit(buf, "%s\n", st.name);
	mutex_unlock(&st.lock);
	return n;
}

static ssize_t name_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	size_t len = count;

	if (len && buf[len - 1] == '\n')	/* "echo foo" writes "foo\n" */
		len--;
	if (len == 0 || len >= ESH_NAME_LEN)
		return -EINVAL;
	mutex_lock(&st.lock);
	memcpy(st.name, buf, len);
	st.name[len] = '\0';
	mutex_unlock(&st.lock);
	return count;
}

/* The kernel convention for a choice: list all, bracket the current one. */
static ssize_t mode_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	int i, len = 0;

	mutex_lock(&st.lock);
	for (i = 0; i < ARRAY_SIZE(esh_modes); i++)
		len += sysfs_emit_at(buf, len, i == st.mode ? "[%s] " : "%s ",
				     esh_modes[i]);
	mutex_unlock(&st.lock);
	buf[len - 1] = '\n';		/* replace the last space */
	return len;
}

static ssize_t mode_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	/* sysfs_match_string: like match_string() but ignores a trailing '\n'. */
	int i = sysfs_match_string(esh_modes, buf);

	if (i < 0)
		return i;		/* -EINVAL */
	mutex_lock(&st.lock);
	st.mode = i;
	mutex_unlock(&st.lock);
	return count;
}

static ssize_t event_count_show(struct kobject *kobj, struct kobj_attribute *attr,
				char *buf)
{
	return sysfs_emit(buf, "%d\n", atomic_read(&event_count));
}

static struct miscdevice esh_misc;

static ssize_t trigger_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	char env_src[] = "ESH_SOURCE=trigger", env_count[32];
	char *envp[3];
	int n = atomic_inc_return(&event_count);

	/*
	 * Wake every poll()/select() waiting on event_count. Readers then
	 * lseek(0) + read() the new value. Cheap, safe from atomic context,
	 * and the standard way a driver says "this attribute changed".
	 */
	sysfs_notify(kobj, NULL, "event_count");

	/*
	 * A uevent goes to udev (netlink) and can run rules. It must be sent
	 * on a kobject that belongs to a kset (here: the misc device, in the
	 * "misc" class). Our bare kobject under /sys/kernel has no kset, so
	 * kobject_uevent() on it would fail with -EINVAL.
	 */
	snprintf(env_count, sizeof(env_count), "ESH_EVENT=%d", n);
	envp[0] = env_src;
	envp[1] = env_count;
	envp[2] = NULL;			/* the list is NULL-terminated */
	kobject_uevent_env(&esh_misc.this_device->kobj, KOBJ_CHANGE, envp);
	return count;
}

/* __ATTR(name, mode, show, store): the attribute file name is "name". */
static struct kobj_attribute value_attr = __ATTR_RW(value);
static struct kobj_attribute name_attr = __ATTR_RW(name);
static struct kobj_attribute mode_attr = __ATTR_RW(mode);
static struct kobj_attribute event_count_attr = __ATTR_RO(event_count);
static struct kobj_attribute trigger_attr = __ATTR_WO(trigger);

static struct attribute *esh_kobj_attrs[] = {
	&value_attr.attr,
	&name_attr.attr,
	&mode_attr.attr,
	&event_count_attr.attr,
	&trigger_attr.attr,
	NULL,				/* the list is NULL-terminated */
};

static const struct attribute_group esh_kobj_group = {
	.attrs = esh_kobj_attrs,
};

/*
 * Binary attribute: raw bytes, no "one text value" rule, may be bigger
 * than a page and is read at an offset (EEPROM contents, firmware blobs,
 * PCI config space). Kept small here. The callback's attr argument became
 * const during the 6.13..6.17 transition (read_new -> read). The guard is
 * verified for the two kernels this repo builds against: 6.8 (non-const)
 * and 6.18 (const).
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define ESH_BIN_ATTR_CONST const
#else
#define ESH_BIN_ATTR_CONST
#endif

struct esh_raw {
	__le32 version;
	__le32 value;
	__le32 events;
	__le32 mode;
	char name[ESH_NAME_LEN];
} __packed;

static ssize_t raw_read(struct file *filp, struct kobject *kobj,
			ESH_BIN_ATTR_CONST struct bin_attribute *attr,
			char *buf, loff_t off, size_t count)
{
	struct esh_raw raw = { .version = cpu_to_le32(1) };

	mutex_lock(&st.lock);
	raw.value = cpu_to_le32(st.value);
	raw.mode = cpu_to_le32(st.mode);
	strscpy(raw.name, st.name, sizeof(raw.name));
	mutex_unlock(&st.lock);
	raw.events = cpu_to_le32(atomic_read(&event_count));
	/* sysfs already clipped count to attr->size - off */
	return memory_read_from_buffer(buf, count, &off, &raw, sizeof(raw));
}

static struct bin_attribute raw_attr = {
	.attr = { .name = "raw", .mode = 0444 },
	.size = sizeof(struct esh_raw),
	.read = raw_read,
};

/* ------------------------------------------------------------------------ */
/* Part 2: a misc device with device attributes                             */
/* ------------------------------------------------------------------------ */
/*
 * DEVICE_ATTR_RW(counter) defines "struct device_attribute dev_attr_counter"
 * using counter_show() and counter_store(). Device attribute callbacks get
 * the struct device, from which a real driver finds its private data
 * (dev_get_drvdata(dev)). Here there is one global device.
 */
static ssize_t counter_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	unsigned long c;

	mutex_lock(&st.lock);
	c = st.counter;
	mutex_unlock(&st.lock);
	return sysfs_emit(buf, "%lu\n", c);
}

static ssize_t counter_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	unsigned long c;
	int err = kstrtoul(buf, 0, &c);

	if (err)
		return err;
	mutex_lock(&st.lock);
	st.counter = c;
	mutex_unlock(&st.lock);
	return count;
}
static DEVICE_ATTR_RW(counter);

static ssize_t info_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	unsigned int opens;

	mutex_lock(&st.lock);
	opens = st.opens;
	mutex_unlock(&st.lock);
	return sysfs_emit(buf, "minor %u, opens %u\n", MINOR(dev->devt), opens);
}
static DEVICE_ATTR_RO(info);

static ssize_t reset_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	bool doit;
	int err = kstrtobool(buf, &doit);	/* 1/0, y/n, on/off */

	if (err)
		return err;
	if (doit) {
		mutex_lock(&st.lock);
		st.counter = 0;
		mutex_unlock(&st.lock);
	}
	return count;
}
static DEVICE_ATTR_WO(reset);

static struct attribute *esh_dev_attrs[] = {
	&dev_attr_counter.attr,
	&dev_attr_info.attr,
	&dev_attr_reset.attr,
	NULL,
};
ATTRIBUTE_GROUPS(esh_dev);	/* defines esh_dev_groups[] = { &esh_dev_group, NULL } */

/* /dev/esh_sysfs: every read() returns "counter N\n" and bumps the counter. */
static int esh_open(struct inode *inode, struct file *file)
{
	mutex_lock(&st.lock);
	st.opens++;
	mutex_unlock(&st.lock);
	return 0;
}

static ssize_t esh_read(struct file *file, char __user *ubuf, size_t len,
			loff_t *ppos)
{
	char line[32];
	int n;

	if (*ppos)		/* one line per open file: then EOF */
		return 0;
	mutex_lock(&st.lock);
	n = scnprintf(line, sizeof(line), "counter %lu\n", st.counter++);
	mutex_unlock(&st.lock);
	return simple_read_from_buffer(ubuf, len, ppos, line, n);
}

static const struct file_operations esh_fops = {
	.owner = THIS_MODULE,
	.open = esh_open,
	.read = esh_read,
	.llseek = default_llseek,
};

/*
 * .groups: misc_register() -> device_create_with_groups() creates the
 * attribute files BEFORE the KOBJ_ADD uevent is sent. The old pattern
 *     misc_register(); device_create_file(dev, &dev_attr_counter);
 * creates them AFTER: udev may run its rule for "add" and find no file
 * (a race that only shows up sometimes, at boot).
 */
static struct miscdevice esh_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "esh_sysfs",
	.fops = &esh_fops,
	.groups = esh_dev_groups,
	.mode = 0444,
};

/* ------------------------------------------------------------------------ */

static int __init sysfs_demo_init(void)
{
	int err;

	/* The misc device first: trigger_store() sends uevents through it. */
	err = misc_register(&esh_misc);
	if (err)
		return err;

	/*
	 * kobject_create_and_add: allocate + init + add a kobject with a
	 * default ktype whose release() frees it. Parent kernel_kobj =
	 * /sys/kernel. NULL parent would put it at /sys/ top level: don't.
	 */
	esh_kobj = kobject_create_and_add("esh_sysfs", kernel_kobj);
	if (!esh_kobj) {
		err = -ENOMEM;
		goto err_misc;
	}
	err = sysfs_create_group(esh_kobj, &esh_kobj_group);
	if (err)
		goto err_kobj;
	err = sysfs_create_bin_file(esh_kobj, &raw_attr);
	if (err)
		goto err_group;

	pr_info("created /sys/kernel/esh_sysfs/{value,name,mode,trigger,event_count,raw}\n");
	pr_info("created /dev/%s, attributes in /sys/class/misc/%s/{counter,info,reset}\n",
		esh_misc.name, esh_misc.name);
	return 0;

err_group:
	sysfs_remove_group(esh_kobj, &esh_kobj_group);
err_kobj:
	kobject_put(esh_kobj);		/* never kfree() a kobject */
err_misc:
	misc_deregister(&esh_misc);
	return err;
}

static void __exit sysfs_demo_exit(void)
{
	/*
	 * Reverse order. Removing the files waits until every show()/store()
	 * already running has returned, so after this no trigger_store() can
	 * touch esh_misc — only then is it safe to deregister the device.
	 */
	sysfs_remove_bin_file(esh_kobj, &raw_attr);
	sysfs_remove_group(esh_kobj, &esh_kobj_group);
	kobject_put(esh_kobj);
	misc_deregister(&esh_misc);
	pr_info("unloaded\n");
}

module_init(sysfs_demo_init);
module_exit(sysfs_demo_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("code-locker");
MODULE_DESCRIPTION("sysfs attributes on a kobject and a misc device, sysfs_notify, uevents");
