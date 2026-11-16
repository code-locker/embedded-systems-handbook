# 05: sysfs, procfs and debugfs

> A driver has to show its state and accept settings, and a character
> device's `read()`/`write()` are for the data stream, not for knobs. The
> kernel has three pseudo-filesystems for this: **sysfs** (one value
> per file, a stable ABI tied to the device model), **procfs** (process
> information and legacy system-wide files, with `seq_file` for big
> output) and **debugfs** (anything for developers, no rules, no
> promises). This lesson builds each one in a module: kobject and device
> attributes with `poll()` support via `sysfs_notify()` and a uevent for
> udev, a `seq_file` listing, and the debugfs one-liners. Two user
> programs test them.

---

## 1. Why this exists

On the BeagleBone AI-64 you constantly ask the kernel questions and turn
knobs from the shell:

```sh
cat /sys/class/thermal/thermal_zone0/temp            # sysfs: one value
echo 1 > /sys/class/leds/beaglebone:green:usr0/brightness
cat /proc/interrupts                                 # procfs: a table
sudo cat /sys/kernel/debug/gpio                      # debugfs: debug dump
sudo cat /sys/kernel/debug/clk/clk_summary
```

None of those files exist on disk. Each `read()` calls a function in a
driver that formats the answer at that moment; each `write()` calls a
function that parses it. Writing such functions is part of nearly every
driver: a sensor exposes its sampling rate, an FPGA bridge its status
register, a motor controller its PID gains. Picking the wrong interface has
long-term costs. A sysfs file is a **userspace ABI** that you may never
break, and debugfs can't be relied on by products at all. So you need to
know which interface fits which job, and the rules each one imposes.

---

## 2. Mental model

**A building's front desk, archive and workshop.** **sysfs** is the front
desk: one labelled pigeonhole per fact (`temp`, `brightness`), organised
by what the device *is* and where it is plugged in. Visitors rely on the
labels never changing. **procfs** is the archive: big ledgers (process
tables, interrupt counts) you read top to bottom. **debugfs** is the
workshop in the basement: engineers only, tools lying around, rearranged
every week.

```
  user space:  cat / echo / poll() / udev rules / your app
  ------------------------------------------------------------- VFS
     /sys  (sysfs)              /proc  (procfs)        /sys/kernel/debug (debugfs)
       |  kernfs                  |                       |
   kobject = directory        proc_dir_entry          dentry from debugfs_create_*
   attribute = file           proc_ops +              file_operations or a
   show()/store()             seq_file iterator       helper for a variable
       |                          |                       |
   your driver's state  <----  the same driver, three windows on it
```

The rule of thumb: **sysfs for stable settings and state of a device,
procfs only for process-related or legacy system-wide data, debugfs for
everything a developer wants to see or poke, and a char device / ioctl /
netlink for data streams and complex transactions.**

---

## 3. How it works

### 3.1 Which interface for what

| Interface | Use it for | Format rules | ABI? | Typical path |
|---|---|---|---|---|
| **sysfs attribute** | one property of a kernel object: a setting, a status, a counter | **one value per file** (or an array of one type), text, < `PAGE_SIZE` | **yes**: document in `Documentation/ABI/`, never break | `/sys/class/<cls>/<dev>/<attr>` |
| sysfs binary attribute | raw bytes: EEPROM contents, firmware, config space | binary, any size, offset reads | yes | `/sys/bus/i2c/devices/0-0050/eeprom` |
| **procfs** | per-process info (`/proc/<pid>/`), legacy system tables | free text, any size via `seq_file` | yes (for existing files); new non-process files are frowned upon | `/proc/interrupts`, `/proc/meminfo` |
| **debugfs** | developer debug state, register dumps, fault injection, tuning knobs | anything | **no**: may change any release, may be absent | `/sys/kernel/debug/<drv>/` |
| configfs | user space *creating* kernel objects (USB gadget functions, iSCSI targets, nvmet) | directory per object, `mkdir` creates | yes | `/sys/kernel/config/usb_gadget/` |
| char device `read`/`write` | data streams (samples, frames, messages) | your binary or text protocol | yes | `/dev/esh` ([01](../01-char-devices-and-file-operations/)) |
| `ioctl` | commands with structured arguments, atomic multi-field ops | `struct` via `_IOWR` | yes | V4L2, DRM, I2C-dev |
| `mmap` | bulk data without copies | shared pages | yes | frame buffers ([08](../08-dma-and-mmap/)) |
| netlink / generic netlink | async events, multicast, big structured messages | TLV attributes | yes | networking, nl80211, uevents, thermal |
| uevent | "a device appeared / changed / went away" to udev | `KEY=VALUE` env list | yes | `udevadm monitor` |

Things to avoid: a sysfs file that prints a table, a sysfs file you write
a command language into, a new `/proc/mydriver` for a device driver, a
product depending on debugfs, or `ioctl` just to read one integer.

### 3.2 kobject, kset, ktype and the sysfs tree

Every directory in sysfs is a **kobject** (`<linux/kobject.h>`):

```
 struct kobject  { name, parent, kset, ktype, kref, sd (kernfs node) }
   - parent    -> where the directory is
   - kref      -> refcount; ktype->release() frees the containing struct
   - ktype     -> release(), sysfs_ops (show/store dispatch), default_groups
   - kset      -> a group of kobjects that also sends uevents for its members

 /sys
 |-- devices/           THE tree: every struct device, by physical topology
 |   |-- platform/bus@100000/...       (the AI-64's SoC buses)
 |   `-- virtual/misc/esh_sysfs/      <- our misc device (no hardware parent)
 |-- class/             symlinks grouped by function -> into devices/
 |   `-- misc/esh_sysfs -> ../../devices/virtual/misc/esh_sysfs
 |-- bus/               per bus type: devices/ (symlinks) + drivers/
 |   `-- i2c/drivers/at24/   bind/unbind files
 |-- module/<name>/     every module: parameters/, refcnt, sections/
 |-- kernel/            kernel-wide knobs, our /sys/kernel/esh_sysfs
 |-- firmware/devicetree/base/   the live device tree
 `-- fs/, block/, power/, dev/char/<maj:min> -> devices/...
```

You rarely create raw kobjects. A `struct device` embeds one, and drivers
add attributes to devices. The device model ([06](../06-device-model-and-device-tree/))
creates the directories, the `class/` and `bus/` symlinks, and the uevents.
A bare kobject (Part 1 of `sysfs_demo`) is for kernel-wide things that
are not devices: `/sys/kernel/mm/transparent_hugepage/`,
`/sys/kernel/debug` itself (an empty mount point), `/sys/fs/ext4/`.

**Kobject rules:** never embed a kobject in a struct and `kfree` that struct
directly: the last `kobject_put()` calls `release()`, which may happen
later (somebody may still have the file open). `kobject_create_and_add()`
gives you a ready-made kobject with a release that frees it. Never call
`kfree()` on it.

### 3.3 sysfs attributes: show, store and the rules  (`sysfs_demo.c` part 1)

```c
static ssize_t value_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
        return sysfs_emit(buf, "%d\n", st.value);    /* one value + newline */
}
static ssize_t value_store(struct kobject *kobj, struct kobj_attribute *attr,
                           const char *buf, size_t count)
{
        int v, err = kstrtoint(buf, 0, &v);           /* strict parse */
        if (err)
                return err;
        if (v < -1000 || v > 1000)
                return -EINVAL;                        /* range check */
        st.value = v;
        return count;                                  /* consumed everything */
}
static struct kobj_attribute value_attr = __ATTR_RW(value);  /* mode 0644 */
```

| Rule | Why |
|---|---|
| **One value per file**, followed by `\n` | scripts do `cat`, `read x < file`; the documented sysfs convention (`Documentation/filesystems/sysfs.rst`) |
| `show` gets a zeroed `PAGE_SIZE` buffer: use **`sysfs_emit()` / `sysfs_emit_at()`** | they check the buffer is a sysfs page and can't overflow it; `sprintf` is the old bug source |
| `store` gets a NUL-terminated copy of what was written (≤ `PAGE_SIZE - 1`), usually with a trailing `\n` | `kstrtoint`/`kstrtoul`/`kstrtobool` accept one trailing newline and reject junk and overflow; `sysfs_streq`/`sysfs_match_string` compare ignoring it |
| `store` returns **`count`** on success, `-errno` on failure | the errno is what `write()` returns to user space (`echo: write error: Invalid argument`). Returning less than `count` makes user space call `write()` again with the rest |
| one `write()` = one `store()` | partial writes aren't supported: write each value in a single call |
| **range-check** everything | user space can write anything; this is an ABI |
| locking: `show`/`store` run in process context and **can run concurrently** with each other and with your driver | protect shared state (a mutex is fine: they may sleep) |
| modes: `0444` RO, `0644` RW, `0200` WO; **never world-writable** | `VERIFY_OCTAL_PERMISSIONS` rejects `0666` at compile time |
| enum attributes print all choices with the current one in brackets | `off [slow] fast`, the convention of e.g. `/sys/block/*/queue/scheduler` and `/sys/kernel/mm/transparent_hugepage/enabled` |

Helpers: `__ATTR_RW(name)` needs `name_show` + `name_store`,
`__ATTR_RO(name)` needs `name_show`, `__ATTR_WO(name)` needs `name_store`.
Put the attributes in a NULL-terminated array in an `attribute_group` and
create them together with `sysfs_create_group()`. A group can have a
`.name` (makes a subdirectory) and an `.is_visible()` callback (hide
attributes the hardware doesn't have).

**ABI documentation.** Every new sysfs file in mainline needs an entry in
`Documentation/ABI/testing/sysfs-…`:

```
What:		/sys/kernel/esh_sysfs/value
Date:		November 2026
Contact:	code-locker
Description:	(RW) Demo integer, -1000..1000. Writing anything else
		returns -EINVAL.
```

### 3.4 Device attributes and the `groups` race  (`sysfs_demo.c` part 2)

In a driver the attributes hang on a `struct device`:

```c
static ssize_t counter_show(struct device *dev, struct device_attribute *attr, char *buf);
static ssize_t counter_store(struct device *dev, struct device_attribute *attr,
                             const char *buf, size_t count);
static DEVICE_ATTR_RW(counter);          /* -> struct device_attribute dev_attr_counter */
static DEVICE_ATTR_RO(info);
static DEVICE_ATTR_WO(reset);
static struct attribute *esh_dev_attrs[] = {
        &dev_attr_counter.attr, &dev_attr_info.attr, &dev_attr_reset.attr, NULL };
ATTRIBUTE_GROUPS(esh_dev);               /* -> esh_dev_groups[] */

static struct miscdevice esh_misc = { ..., .groups = esh_dev_groups };
```

In a real driver `dev_get_drvdata(dev)` gives the per-device state inside
`show`/`store`.

**Why `.groups` and not `device_create_file()`?** Registering a device
sends a `KOBJ_ADD` uevent. udev reacts at once: it runs rules such as
`ATTR{counter}=="0", RUN+=…` or sets permissions on attribute files. With
the old pattern

```
 misc_register()   ---- KOBJ_ADD uevent ---->  udev: reads ATTR{counter} ... no such file!
 device_create_file(dev, &dev_attr_counter)    (file appears a moment too late)
```

the files appear *after* the uevent: a race that fails sometimes, usually
at boot. Passing the groups to the core (`miscdevice.groups`,
`device_create_with_groups()`, `class->dev_groups`, `driver->dev_groups`)
creates the files **before** the uevent, and removes them for you on
unregister.

### 3.5 `sysfs_notify()` and `poll()` on an attribute

A sysfs file can be polled. The protocol is unusual, so learn it once:

```
 user space                                   kernel
 fd = open("event_count")
 read(fd, ...)        -> "4\n"                (the read ARMS notification)
 poll({fd, POLLPRI|POLLERR}) ... sleeps
                                              trigger_store():
                                                sysfs_notify(kobj, NULL, "event_count")
 wakes: revents = POLLPRI | POLLERR
 lseek(fd, 0, SEEK_SET)                       (sysfs content is always at offset 0)
 read(fd, ...)        -> "5\n"                (re-read, re-arms)
```

- Ask for **`POLLPRI | POLLERR`**, not `POLLIN`. A sysfs file is always
  "readable", so `POLLIN` returns immediately.
- `sysfs_notify(kobj, dir, attr)` may be called from any context (it
  defers the wake-up), so interrupt handlers use it to report "status
  changed": GPIO `value` edges (the old sysfs GPIO interface), power-supply
  status, MD RAID `sync_action`, thermal trip points. In a device driver:
  `sysfs_notify(&dev->kobj, NULL, "attr")`, or keep the `kernfs_node` from
  `sysfs_get_dirent()` and call `sysfs_notify_dirent()` from hard-IRQ paths.
- `select()` uses the exceptional set (`exceptfds`), `epoll` uses `EPOLLPRI`.

`esh_sysfs_test` checks this exactly: arm, a 100 ms timeout with no
event, then a thread writes `trigger` 200 ms later and `poll()` wakes up.

### 3.6 Binary attributes and uevents

**Binary attributes** (`struct bin_attribute`, `sysfs_create_bin_file`)
are for raw data: `read(filp, kobj, attr, buf, off, count)` at an offset,
size fixed in `attr->size` (or 0 = unknown). `sysfs_demo` exposes a 48-byte
little-endian struct as `raw`. Real users: `eeprom` (at24), `config`
(PCI), `firmware/.../data`, `nvmem`. Note that the callback's `attr`
argument became `const` during 6.13–6.17, which is why
`sysfs_demo.c` has a `LINUX_VERSION_CODE` guard (checkpatch warns about
it; it is needed to build for both 6.8 and 6.18).

**Uevents** tell user space that a kobject was added, removed, or
changed. They go out over a netlink socket (and formerly via
`/sbin/hotplug`) to `systemd-udevd`, which matches rules in
`/etc/udev/rules.d/`:

```c
char *envp[] = { "ESH_SOURCE=trigger", "ESH_EVENT=5", NULL };
kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp);   /* ADD/REMOVE are sent by the core */
```

A uevent needs a kobject that belongs to a **kset** (devices do, through
their class or bus). The bare `/sys/kernel/esh_sysfs` kobject doesn't, so
`sysfs_demo` sends its uevent on the misc device instead. Watch them:

```sh
udevadm monitor --kernel --property --subsystem-match=misc
echo 1 | sudo tee /sys/kernel/esh_sysfs/trigger
# KERNEL[1234.567890] change   /devices/virtual/misc/esh_sysfs (misc)
# ACTION=change
# DEVPATH=/devices/virtual/misc/esh_sysfs
# SUBSYSTEM=misc
# ESH_SOURCE=trigger
# ESH_EVENT=5
# DEVNAME=/dev/esh_sysfs
# SEQNUM=4711
# MAJOR=10
# MINOR=123
```

A udev rule that reacts (e.g. `/etc/udev/rules.d/99-esh.rules`):

```
SUBSYSTEM=="misc", KERNEL=="esh_sysfs", MODE="0666"
SUBSYSTEM=="misc", KERNEL=="esh_sysfs", ACTION=="change", ENV{ESH_EVENT}=="?*", \
    RUN+="/usr/bin/logger esh event $env{ESH_EVENT}"
```

`udevadm info -a /dev/esh_sysfs` shows every attribute a rule can match on
(`ATTR{counter}`, `ATTR{info}`).

### 3.7 procfs and `seq_file`  (`procfs_demo.c`)

`/proc` began as "processes as files" and is still mainly that:
`/proc/<pid>/{status,maps,fd/,stack,sched}` is the kernel exporting each
task's data, generated on read (`esh_procfs_test` checks `/proc/self/status`
shows its own pid). The system-wide files (`/proc/interrupts`,
`meminfo`, `buddyinfo`, `slabinfo`) are a legacy ABI. **New driver
files go to sysfs or debugfs, not procfs.** You still need to read and
maintain procfs code, and `seq_file` is used by debugfs files too.

Since 5.6 a proc file takes a `struct proc_ops`, not `file_operations`:

```c
static const struct proc_ops items_proc_ops = {
        .proc_open = items_open, .proc_read = seq_read,
        .proc_lseek = seq_lseek, .proc_release = seq_release,
};
dir = proc_mkdir("esh_procfs", NULL);
proc_create("items", 0444, dir, &items_proc_ops);
proc_remove(dir);                                  /* recursive */
```

**Why `seq_file`?** A naive `read()` that `sprintf`s into the user's buffer
breaks when the output is bigger than the buffer, or when user space reads
7 bytes at a time: you would have to remember where in your list you were.
`seq_file` keeps a kernel buffer and the position, and drives *your*
iterator:

```
 read(fd, buf, 4096)                       start(pos=0)  -> lock, return header token
                                           show(header)
                                           next -> item 1, show, next -> item 2 ...
                                           buffer full -> stop() -> unlock
                                           copy 4096 bytes to user
 read(fd, buf, 4096)                       start(pos=143) -> lock, find item 143
                                           show ... next ... stop() -> unlock
 read(...) == 0                            EOF when next() returns NULL
```

| Callback | Job |
|---|---|
| `start(m, pos)` | take the lock, return element number `*pos` (or `SEQ_START_TOKEN` for a header, or NULL = EOF) |
| `next(m, v, pos)` | advance `*pos`, return the next element or NULL |
| `stop(m, v)` | release the lock (called even if `start` returned NULL) |
| `show(m, v)` | print one element with `seq_printf`/`seq_puts`/`seq_putc` |

`seq_list_start_head()` / `seq_list_next()` implement start/next for a
`list_head` (position 0 = the head, used here for the header line). The
lock is held from `start` to `stop` inside one `read()`, never across
reads, so a reader may see the list change between two `read()` calls.

**`single_open(file, show, data)`** is for small files: one `show()`
prints everything (`stats`). If it doesn't fit, `seq_file` retries with a
bigger buffer, which gets wasteful for big output. `DEFINE_SHOW_ATTRIBUTE(name)`
generates the open and fops for a read-only `single_open` file
(`DEFINE_PROC_SHOW_ATTRIBUTE` for `proc_ops`).

**Writable entry** (`ctl`): add `.proc_write`, `copy_from_user()` into a
bounded buffer, NUL-terminate, `strim()`, parse (`sscanf` with field
widths!), return `count` or `-errno`. `procfs_demo` accepts
`add NAME VALUE`, `del ID`, `clear`.

```sh
cat /proc/esh_procfs/items | head -3
#   id name                        value
#    1 item-0                          0
#    2 item-1                          1
cat /proc/esh_procfs/stats
echo "add sensor0 77" | sudo tee /proc/esh_procfs/ctl
echo "del 1" | sudo tee /proc/esh_procfs/ctl
echo "bogus" | sudo tee /proc/esh_procfs/ctl     # tee: /proc/esh_procfs/ctl: Invalid argument
```

### 3.8 debugfs  (`debugfs_demo.c`)

debugfs is mounted at `/sys/kernel/debug` (`mount -t debugfs none
/sys/kernel/debug`; systemd does it at boot) and is **root-only**
(`0700`). Anything goes, nothing is promised.

```c
dir = debugfs_create_dir("esh_debugfs", NULL);          /* NULL = top level */
debugfs_create_bool("enabled", 0644, dir, &enabled);    /* "Y"/"N", accepts 1/0/y/n */
debugfs_create_u32("threshold", 0644, dir, &threshold); /* decimal */
debugfs_create_x32("flags", 0644, dir, &flags);         /* "0x00000005" */
debugfs_create_atomic_t("samples", 0444, dir, &samples);
debugfs_create_blob("banner", 0444, dir, &blob);        /* fixed bytes */
debugfs_create_file("state", 0644, dir, priv, &state_fops); /* your own fops */
debugfs_remove(dir);                                    /* recursive (5.x+) */
```

- **Don't check the return values.** If debugfs is disabled or fails, you
  get an `ERR_PTR`, and every debugfs call accepts it as a parent and does
  nothing. A driver must work without its debug files. (Since 5.x most
  `debugfs_create_*` helpers return `void`, which enforces this.)
- The variable helpers read and write **without locking**. That's fine for
  single words. Use a custom file (`state`) when values must be consistent
  with each other.
- **Removal is safe:** `debugfs_remove()` waits for running file operations
  on those files and makes later ones fail ("file removal protection"),
  so you can free the data right after.
- Other helpers: `debugfs_create_u8/u16/u64/x8/x16/x64/size_t/ulong`,
  `debugfs_create_regset32` (dump a block of MMIO registers by name, very
  handy on the TDA4VM), `debugfs_create_devm_seqfile`, `debugfs_create_symlink`,
  `debugfs_lookup`.
- **Lockdown / Secure Boot:** with `lockdown=integrity` (Ubuntu with Secure
  Boot), files that could change kernel memory are restricted; with
  `lockdown=confidentiality` debugfs is essentially unusable. On the board
  (no lockdown) it is fully available. On your PC: `cat /sys/kernel/security/lockdown`.
  It is also common to build product kernels with `CONFIG_DEBUG_FS=n` or
  boot with `debugfs=off`.

How debugfs fits with the other debugging tools (dynamic debug, ftrace,
kprobes) is in [09 kernel debugging and tracing](../09-kernel-debugging-and-tracing/).

```sh
sudo ls -l /sys/kernel/debug/esh_debugfs
sudo cat /sys/kernel/debug/esh_debugfs/state
echo 1   | sudo tee /sys/kernel/debug/esh_debugfs/enabled
echo 500 | sudo tee /sys/kernel/debug/esh_debugfs/threshold
sleep 2; sudo cat /sys/kernel/debug/esh_debugfs/state     # samples ~20, over ~50%
echo reset | sudo tee /sys/kernel/debug/esh_debugfs/state
```

### 3.9 Module parameters are sysfs too

`module_param(period_ms, uint, 0444)` creates
`/sys/module/debugfs_demo/parameters/period_ms`. Mode `0644` makes it
writable at run time (the module isn't told, so it must re-read the
variable, or use `module_param_cb()` with a `set` callback that validates
the value). `/sys/module/<name>/` also has `refcnt`, `holders/`,
`sections/` (used by gdb to load symbols, see [09](../09-kernel-debugging-and-tracing/)).

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `cat` a sysfs attribute | 1 syscall chain + 1 `show()` | buffer is one page, formatted per `read()` from offset 0 |
| `echo` to an attribute | 1 `store()` per `write()` | no partial writes |
| `sysfs_notify()` | O(pollers) wake-ups, deferred | kernfs notify work |
| `poll()` on an attribute | O(1) per wait | kernfs open-file event counter |
| procfs `seq_file` read of n items | O(n) per full read, plus O(pos) per `read()` call to re-find the position with list helpers | `start(pos)` walks from the head unless you keep a cursor (the demo's `seq_starts` counter shows how often) |
| `single_open` file | O(output) per read, may re-run `show()` with a larger buffer | retry on overflow |
| debugfs variable helper read/write | O(1) | direct access, no lock |
| creating/removing files | O(files) | kernfs nodes; removal waits for active users |
| uevent | O(listeners) + netlink + udev rule processing (ms) | not for high-rate events |

---

## 5. C vs C++, and kernel vs user space

There is no C++ in the kernel. From user space these interfaces look
like ordinary files: the same C code (`open`/`read`/`write`/`poll`) or
`std::ifstream` reads them. Some differences matter:

| Aspect | Regular file | sysfs / procfs / debugfs |
|---|---|---|
| content | stored bytes | generated by a kernel callback on each read |
| size (`stat`) | real | sysfs: 4096 or the bin size; proc: 0; read until EOF |
| partial writes | fine | sysfs: one `write()` = one value; write whole values |
| `write()` errors | disk errors | whatever `store()` returned: `EINVAL`, `EBUSY`, `ERANGE` |
| `poll()` | always ready | sysfs: `POLLPRI` after `sysfs_notify()` |
| `std::ofstream << 42` | fine | works if the stream flushes in one `write()` (flush/close before checking) |

Typical user-space C (as in `esh_sysfs_test.c`):

```c
int fd = open("/sys/kernel/esh_sysfs/value", O_WRONLY);
if (write(fd, "123\n", 4) < 0) perror("value");   /* EINVAL if out of range */
close(fd);
```

Libraries such as `libudev`/`sd-device` (C) wrap sysfs walking and uevent
monitoring. Python scripts on the board usually just open the files.

---

## 6. Where it is used in the real world

- **sysfs on the AI-64:** `/sys/class/leds/*/brightness` and `trigger`
  (the enum convention: `none [heartbeat] mmc0`), `/sys/class/thermal/`,
  `/sys/class/remoteproc/remoteprocN/{state,firmware}` to start the R5F
  and C7x cores with new firmware, `/sys/bus/iio/devices/iio:deviceN/`
  for ADCs, `/sys/class/pwm/`, `/sys/devices/system/cpu/cpu*/cpufreq/`,
  `/sys/bus/platform/drivers/*/bind|unbind`.
- **sysfs_notify:** power-supply status, GPIO sysfs edges (legacy),
  `md` RAID, thermal trips, `rtc` alarms, hwmon alarms.
- **uevents:** every `/dev` node is created by udev/devtmpfs from them;
  USB hot-plug; `remoteproc` crash reports; network link changes reach
  user space through netlink as well.
- **procfs:** `ps`, `top`, `htop` read `/proc/<pid>/stat`; `free` reads
  `meminfo`; `/proc/device-tree` is a symlink to sysfs.
- **seq_file everywhere:** `/proc/interrupts`, `/proc/slabinfo`, and most
  debugfs dumps: `/sys/kernel/debug/gpio`, `clk/clk_summary`,
  `pinctrl/*/pins`, `regmap/*/registers`, `dma_buf/bufinfo`.
- **debugfs:** `dynamic_debug/control`, `tracing` (formerly), `kmemleak`,
  `fault injection`, `regmap` register dumps, DRM state (`dri/0/state`),
  `ti_sci` firmware state on K3 SoCs.
- **configfs:** USB gadget (turning the AI-64's USB-C into a network +
  serial gadget), `nvmet`, `iio` software triggers.

---

## 7. Common mistakes and pitfalls

1. **Several values or a table in one sysfs file.** Use several files, or
   debugfs, or a proper interface.
2. **`sprintf(buf, …)` / `snprintf(buf, PAGE_SIZE, …)` in `show`.** Use
   `sysfs_emit()`; checkpatch and reviewers will ask for it.
3. **Returning 0 from `store`.** User space retries forever (`echo` hangs
   at 100% CPU). Return `count` or an error.
4. **No range check / `simple_strtol`** (silently accepts junk). Use
   `kstrto*()`, which rejects junk and overflow, and range-check.
5. **Assuming `show`/`store` are serialised.** They aren't. Lock.
6. **`device_create_file()` after registration**: the udev race. Use
   `groups`.
7. **Polling with `POLLIN`**, or forgetting `lseek(fd, 0)` + re-read after
   a wake-up (the next `poll()` returns at once).
8. **Uevent on a kobject without a kset**: silently fails (`-EINVAL`).
9. **Raw kobject lifetime**: `kfree()` on a kobject, or freeing the
   containing struct while a sysfs file is open. Use `kobject_put()` and a
   `release()`.
10. **Wrong teardown order**: freeing data, or unregistering what `store()`
    uses, *before* removing the files. Remove files first; removal waits
    for running callbacks.
11. **A `seq_file` `start()` that doesn't take the lock `stop()` releases**
    (`stop` is called even when `start` returns NULL or an error).
12. **Holding a lock across `read()` calls** in a seq_file (impossible
    to do safely: user space may never call read again).
13. **`sscanf("%s")` without a width** in a write handler: kernel stack
    overflow.
14. **Products depending on debugfs.** It may be off (`CONFIG_DEBUG_FS=n`,
    `debugfs=off`, lockdown). Failing `probe()` because a debugfs call
    failed is a bug.
15. **World-writable files** (`0666`): rejected at build time for sysfs.
    For debugfs and procfs it's a security hole.
16. **New `/proc` files for device drivers**: rejected upstream; use sysfs
    or debugfs.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile; `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `W=1`, `clean` | `make drivers` |
| [kernel/sysfs_demo.c](kernel/sysfs_demo.c) | `kobject_create_and_add` under `/sys/kernel/esh_sysfs` with an attribute group: int (`kstrtoint`, range check), string, enum (`sysfs_match_string`, `[current]` display), WO `trigger` that `sysfs_notify()`s `event_count` and sends a `KOBJ_CHANGE` uevent; binary attribute `raw`; misc device `/dev/esh_sysfs` with `DEVICE_ATTR_RW/RO/WO` passed via `miscdevice.groups`; safe teardown order | `insmod sysfs_demo.ko` |
| [kernel/procfs_demo.c](kernel/procfs_demo.c) | `/proc/esh_procfs/`: `items` = `seq_operations` over a `list_head` (header via `seq_list_start_head`, counts `start()` calls), `stats` = `single_open`, `ctl` = writable entry (`add`/`del`/`clear`) | `insmod procfs_demo.ko [nitems=200]` |
| [kernel/debugfs_demo.c](kernel/debugfs_demo.c) | `/sys/kernel/debug/esh_debugfs/`: `bool`, `u32`, `x32`, `atomic_t`, `blob`, custom `state` file (seq_file read + write "reset"); a delayed-work sampler controlled by the knobs; recursive removal | `insmod debugfs_demo.ko [period_ms=100]` |
| [user/esh_sysfs_test.c](user/esh_sysfs_test.c) | writes/reads each attribute, checks `EINVAL` on bad input and RO/WO permissions, `poll(POLLPRI\|POLLERR)` woken by `sysfs_notify` with the read/poll/lseek/re-read protocol, device attributes + `/dev` read, receives the uevent on a `NETLINK_KOBJECT_UEVENT` socket | `sudo ./build/linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_sysfs_test` |
| [user/esh_procfs_test.c](user/esh_procfs_test.c) | reads `items` with 7-byte reads and one big read and compares, counts lines vs `stats`, checks ordering, `ctl` add/del/ENOENT/EINVAL (root), `/proc/self/status` shows its pid | `sudo ./build/linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_procfs_test` |

**Build** (from the repo root):

```sh
make drivers                                                       # host: modules + user/ programs
make -C linux-device-drivers/05-sysfs-procfs-debugfs/kernel W=1    # extra warnings
BB=<path to beaglebone-ai64>/sources/arm64-multiplatform           # board, see ../README.md#building
make -C linux-device-drivers/05-sysfs-procfs-debugfs/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
aarch64-linux-gnu-gcc -O2 -static -o esh_sysfs_test  linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_sysfs_test.c -lpthread
aarch64-linux-gnu-gcc -O2 -static -o esh_procfs_test linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_procfs_test.c
make drivers-clean
```

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1`; checkpatch `--strict` clean apart
from the file-name-in-header note and, in `sysfs_demo.c`, the
`LINUX_VERSION_CODE` warning for the 6.8/6.18 binary-attribute guard),
and cross-built with `W=1` against the board's `6.18.38-arm64-k3`
PREEMPT_RT tree with its GCC 15. The user programs build with the repo's
flags and sanitizers with zero warnings, and exit 2 with a hint when the
module isn't loaded. Loading the modules is **your** step, on the board or
in a VM; see [../README.md](../README.md#safety).

**Run** (board or VM):

```sh
sudo insmod sysfs_demo.ko
sudo ./esh_sysfs_test
sudo rmmod sysfs_demo

sudo insmod procfs_demo.ko nitems=200
sudo ./esh_procfs_test
sudo rmmod procfs_demo

sudo insmod debugfs_demo.ko
echo 1 | sudo tee /sys/kernel/debug/esh_debugfs/enabled; sleep 1
sudo cat /sys/kernel/debug/esh_debugfs/state
sudo rmmod debugfs_demo
```

Expected output (example; minors, counts and timings vary):

```
$ sudo dmesg | tail -2
sysfs_demo: created /sys/kernel/esh_sysfs/{value,name,mode,trigger,event_count,raw}
sysfs_demo: created /dev/esh_sysfs, attributes in /sys/class/misc/esh_sysfs/{counter,info,reset}

$ sudo ./esh_sysfs_test
PASS value: write/read, hex input, EINVAL for 5000 and "12abc"
PASS name/mode: string with length check, enum shown as "off [slow] fast"
PASS perms: event_count is read-only, trigger is write-only
PASS poll: slept 201 ms until sysfs_notify(), event_count 0 -> 1
PASS device attrs: counter rw, read(/dev) bumps it, reset wo, info: minor 123, opens 1
PASS uevent: ACTION=change SUBSYSTEM=misc ESH_EVENT=2 received over netlink
all sysfs_demo checks passed

$ sudo ./esh_procfs_test
PASS items: 200 items, 7839 bytes, identical with 1120 x 7-byte reads and 1 big read(s); seq start() ran 1125 times
     (output > one 4 KiB page: seq_file paged it for us)
PASS ctl: add -> item 201 listed, del -> gone, ENOENT twice, EINVAL for junk
PASS /proc/self/status: shows our pid 1873 (generated on read by the kernel)
all procfs_demo checks passed

$ sudo cat /sys/kernel/debug/esh_debugfs/state
enabled:   yes
period_ms: 100
threshold: 900
flags:     0x00000005
samples:   10
over:      1 (10%)
last:      412
```

Without the module the programs print the `insmod` hint and exit 2.
Without root, `esh_sysfs_test` exits 2 ("run as root") and
`esh_procfs_test` skips the `ctl` part.

---

## 9. Exercises

**Easy**
1. `ls -l /sys/kernel/esh_sysfs` and `stat` an attribute. Why is the size
   4096? Why does `ls -l /proc/esh_procfs` show size 0?
2. Read `/sys/kernel/esh_sysfs/raw` with `xxd`. Find `value` and `name` in
   the bytes. Why little-endian explicitly?
3. Run `udevadm monitor --kernel --property` while writing `trigger`, then
   while `rmmod`-ing the module. Which ACTIONs do you see?

**Medium**
4. Add a `threshold` attribute to the misc device and call
   `sysfs_notify(&dev->kobj, NULL, "alarm")` from a timer when a fake
   reading crosses it. Extend `esh_sysfs_test` to `poll()` on `alarm`.
5. Give the misc device's attribute group an `.is_visible()` that hides
   `reset` when a module parameter `readonly=1` is set.
6. Make `period_ms` in `debugfs_demo` writable at run time through
   `module_param_cb()` with a `set` callback that range-checks and
   reschedules the work.
7. Replace `seq_list_start_head` in `procfs_demo` with your own `start`
   that keeps a cursor, so continuing a read is O(1) instead of O(pos).
   Compare `seq_starts` and time for `nitems=10000` read with `dd bs=64`.

**Hard**
8. Write the `Documentation/ABI/testing/sysfs-kernel-esh_sysfs` file for
   every attribute, then run `scripts/get_abi.pl validate` from the
   kernel tree against it.
9. Turn the `/proc/esh_procfs/ctl` interface into a configfs subsystem:
   `mkdir /sys/kernel/config/esh/items/foo` creates an item, an attribute
   `value` sets it.
10. Use `debugfs_create_regset32` to dump real registers on the board:
    `ioremap` the TDA4VM's GPIO0 block (address from the device tree /
    TRM), name 8 registers, and read them while toggling a GPIO from
    user space.

---

## 10. Remember forever

```
sysfs = ONE value per file + '\n', stable ABI (Documentation/ABI), device model
procfs = processes + legacy tables; seq_file (start/next/stop/show) or single_open
debugfs = anything, root only, no ABI; never check returns; debugfs_remove(dir) recursive
show: return sysfs_emit(buf, "%d\n", v);   store: kstrtoint + range check, return count
DEVICE_ATTR_RW(x) -> x_show/x_store; ATTRIBUTE_GROUPS(); pass .groups (no udev race)
sysfs_notify(kobj, NULL, "attr"); user: read, poll(POLLPRI|POLLERR), lseek 0, read
uevent: kobject_uevent_env(&dev->kobj, KOBJ_CHANGE, envp); needs a kset; udevadm monitor
Teardown: remove files first (waits for callbacks), then free what they use
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. For each, pick sysfs, procfs, debugfs, char device, ioctl or netlink:
   a sensor's sampling rate; a 30 fps video stream; a dump of 200 DMA
   descriptors for debugging; per-process memory maps; "link went down"
   events; a command with five arguments that must apply atomically.
2. What is a kobject? What do its parent, kset, ktype and kref do?
3. Draw the `/sys` top level. Why is `/sys/class/misc/esh_sysfs` a symlink?
4. List five rules for `show()` and `store()`.
5. What does `write()` return to user space when `store()` returns
   `-EINVAL`? When it returns 0?
6. Explain the race that `device_create_file()` after registration causes,
   and how `.groups` fixes it.
7. Describe the full user-space protocol for waiting on a sysfs attribute.
   Why `POLLPRI` and not `POLLIN`?
8. Why can't you send a uevent on a kobject created under `/sys/kernel`
   with no kset?
9. Walk through two consecutive `read()` calls on a `seq_file` with
   `start/next/stop/show`. Where is the lock taken and released?
10. When is `single_open` the wrong choice?
11. Why should driver code ignore the return values of debugfs functions?
12. In which order do you tear down sysfs files, a misc device and the data
    their callbacks use, and why is that order safe?
13. What does kernel lockdown do to debugfs?
