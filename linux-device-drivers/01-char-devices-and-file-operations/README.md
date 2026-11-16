# 01 — Char Devices and File Operations

> How a driver becomes a file in `/dev`: device numbers, `struct cdev`,
> classes and udev, misc devices, and the `file_operations` a driver fills
> in — `open`/`release`/`read`/`write`/`llseek`/`unlocked_ioctl` — with the
> `*ppos` convention, safe user-memory access (`copy_to_user` & co.), ioctl
> design with a header shared between driver and user space, and several
> openers at once. The lesson's driver `memdev` is a set of RAM-backed
> devices you can `echo` into, `cat`, seek in and resize by ioctl.

---

## 1. Why this exists

"Everything is a file" is how user space talks to hardware on Linux. A
program that wants to read a GPIO line, a UART, an I²C bus, a camera, an
R5F remote core on the TDA4VM or your custom FPGA block does the same
thing: `open("/dev/something")`, then `read`, `write`, `ioctl`, `poll`,
`mmap`, `close`. The **character device** is the kernel object that turns
those system calls into calls to *your* driver's functions.

You already know the user side from
[linux-internals/00-syscalls-and-file-io](../../linux-internals/00-syscalls-and-file-io/).
This lesson is the other end of the same syscalls: what the kernel does
between your `read(fd, buf, n)` and the driver, which numbers connect
`/dev/memdev0` to a module, and the rules a driver must follow so that a
buggy or hostile program cannot crash the kernel through it.

---

## 2. Mental model

**A device node is a phone-book entry with a number, not a file with
data.** `/dev/memdev0` stores no bytes; it stores a *type* (`c` = char) and
a *number* (major 236, minor 0 — the major is picked at load time). When you
open it, the kernel looks the number up in its char-device table, finds the
`struct cdev` your driver registered, and from then on every syscall on that
file descriptor calls your `file_operations`.

```
 user space          open("/dev/memdev0", O_RDWR)   read(fd, buf, 64)
 ----------------------------|-------------------------------|-----------
 VFS                         v                               v
   inode of /dev/memdev0: S_IFCHR, i_rdev = 236:0    fd -> struct file
        |  chrdev_open(): look up 236:0 in cdev_map        f_op = &memdev_fops
        v                                                  private_data -> memdev_file
   struct cdev (embedded in struct memdev[0]) --- ops ---> memdev_fops
                                                              .open    = memdev_open
 driver                                                       .read    = memdev_read
   struct memdev[0] { lock, data, size, len, ... }            .write   = memdev_write
   struct memdev[1] ...                                       .llseek  = memdev_llseek
                                                              .unlocked_ioctl = memdev_ioctl
```

Three objects to keep apart:

| Object | One per | Holds |
|---|---|---|
| `struct inode` | device *node* (filesystem object) | `i_rdev` (the dev_t), `i_cdev` (our cdev, once opened) |
| `struct file` | `open()` call (shared by `dup()`/`fork()`) | `f_pos`, `f_flags` (`O_NONBLOCK`, `O_APPEND`...), `f_mode`, **`private_data`** |
| file descriptor | process table slot | just an index pointing at a `struct file` |

---

## 3. How it works

### 3.1 Device numbers: major, minor, `dev_t`

A `dev_t` is 32 bits: 12-bit **major** (which driver) + 20-bit **minor**
(which device of that driver). `MAJOR(dev)`, `MINOR(dev)`,
`MKDEV(major, minor)` convert. `cat /proc/devices` lists the char majors
in use; `ls -l /dev/memdev0` shows `236, 0` where a size would be.

| API | Use |
|---|---|
| `alloc_chrdev_region(&dev, first_minor, count, "memdev")` | **dynamic major** — the right default (`memdev.c`) |
| `register_chrdev_region(MKDEV(240, 0), count, name)` | a *fixed* major — only for numbers assigned in `Documentation/admin-guide/devices.txt` (240–254 are "local/experimental") |
| `register_chrdev(major, name, &fops)` | legacy one-call API: grabs all 256 minors and makes its own cdev; avoid in new code |
| `unregister_chrdev_region(dev, count)` | give the numbers back (exit and error paths) |

### 3.2 `struct cdev`: connecting numbers to `file_operations`

```c
cdev_init(&dev->cdev, &memdev_fops);   /* embed the cdev in your per-device struct */
dev->cdev.owner = THIS_MODULE;
err = cdev_add(&dev->cdev, MKDEV(major, i), 1);   /* minor i is LIVE from here */
```

- After `cdev_add` succeeds an `open()` can arrive **immediately** (another
  CPU, or a node created earlier by hand with `mknod`). Initialise
  everything the fops touch *before* it; `memdev.c` even runs its load-time
  self-test on minor 0 before `cdev_add`.
- In `open`, `container_of(inode->i_cdev, struct memdev, cdev)` gets you back
  from the embedded cdev to your device struct — the standard trick for "which
  of my N devices is this?".
- `cdev_del` removes the mapping but does **not** wait for open files. That
  is safe here because `fops.owner = THIS_MODULE` pins the module while a
  file is open (rmmod fails with EBUSY). Drivers whose devices can disappear
  while open (hot-unplug) need reference counting — lesson
  [06](../06-device-model-and-device-tree/).
- `cdev_device_add()` combines `cdev_add` + `device_add` for drivers that
  manage a `struct device` themselves.

### 3.3 Classes, udev and `/dev`

The cdev does not create a file in `/dev`. That is done by user space
(**udev**, part of systemd) and the kernel's **devtmpfs**, triggered by a
*uevent*:

```
 class_create("memdev")            -> /sys/class/memdev/
 device_create(class, NULL, 236:0, drvdata, "memdev%u", 0)
        -> /sys/class/memdev/memdev0/{dev = "236:0", uevent, ...}
        -> devtmpfs: mknod /dev/memdev0 c 236 0, mode from class->devnode()
        -> uevent ACTION=add MAJOR=236 MINOR=0 DEVNAME=memdev0
             -> udevd applies /etc/udev/rules.d/*.rules (owner, group, mode, symlinks)
 device_destroy(class, 236:0)      -> node removed, ACTION=remove
```

- Watch it: `udevadm monitor --kernel --udev` while loading the module;
  `udevadm info /dev/memdev0` afterwards.
- `class_create(name)` lost its `THIS_MODULE` argument in 6.4; both the host
  (6.8) and board (6.18) kernels use the one-argument form. New in-tree code
  prefers a `static const struct class` with `class_register()`.
- **Permissions**: the default node is `root:root 0600`. `memdev.c` sets
  `0666` through the class `devnode()` callback so the test runs without
  sudo (`memdev_misc.c` uses `miscdevice.mode`). A real driver keeps the
  default and lets the system decide with a **udev rule**:

```
# /etc/udev/rules.d/99-memdev.rules
SUBSYSTEM=="memdev", KERNEL=="memdev[0-9]*", GROUP="dialout", MODE="0660"
SUBSYSTEM=="memdev", KERNEL=="memdev0", SYMLINK+="scratchpad"
# reload: sudo udevadm control --reload && sudo udevadm trigger -s memdev
```

  (`SUBSYSTEM` matches the class name; on the BeagleBone's Debian image the
  default user is in `dialout`, `gpio`, `i2c`, `spi` ... groups for exactly
  this reason.)
- Without udev/devtmpfs (tiny initramfs, BusyBox `mdev`): look the number
  up in `/proc/devices` and `mknod /dev/memdev0 c 236 0` by hand.

### 3.4 Misc devices: one call instead of four (`memdev_misc.c`)

```c
static struct miscdevice memdev_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR, .name = "memdev_misc",
	.fops  = &mm_fops,           .mode = 0666,
};
misc_register(&memdev_misc_dev);   /* /dev/memdev_misc, major 10 */
misc_deregister(&memdev_misc_dev);
```

All misc devices share **major 10**; the misc core owns the cdev and the
`misc` class and creates `/dev/<name>`. In the misc core's open,
`file->private_data` is set to your `struct miscdevice *` before your
`.open` runs (embed it in your device struct and use `container_of`).

| Use misc when | Use the full cdev path when |
|---|---|
| one node (or a few) per driver | many minors of one kind (`/dev/memdev0..7`, `/dev/ttyS*`) |
| control/side-channel interfaces: `/dev/watchdog`, `/dev/kvm`, `/dev/fuse`, `/dev/uinput`, [`/dev/ksync`](../03-kernel-synchronization/kernel/ksync_chardev.c) | you need your own class in `/sys/class/<name>` or a fixed major |

### 3.5 `file_operations` and crossing the user/kernel boundary

```c
static const struct file_operations memdev_fops = {
	.owner          = THIS_MODULE,      /* open file => module can't be unloaded */
	.open           = memdev_open,      /* allocate per-open state */
	.release        = memdev_release,   /* LAST close of the struct file */
	.read           = memdev_read,
	.write          = memdev_write,
	.llseek         = memdev_llseek,
	.unlocked_ioctl = memdev_ioctl,
	.compat_ioctl   = compat_ptr_ioctl, /* 32-bit programs on a 64-bit kernel */
};
```

Leaving a member NULL gives a default: no `.read` → `EINVAL`; no
`.unlocked_ioctl` → `ENOTTY`; no `.llseek` → `lseek` fails with `ESPIPE`,
as on a pipe. Choose explicitly: your own `llseek` (`memdev`), a libfs
helper (`generic_file_llseek_size`, `fixed_size_llseek`, `default_llseek`),
`noop_llseek` ("succeeds, does nothing", `/dev/ksync`), or
`stream_open()`/`nonseekable_open()` in `open` for FIFO-like devices (also
disables `pread`/`pwrite`).

**`open` / `release` and `private_data`.** `memdev_open` allocates a
`struct memdev_file` (pointer to the device + per-open counters) and stores
it in `file->private_data`; every other fop starts with
`struct memdev_file *mf = file->private_data;`. `release` runs once per
`struct file` — on the last `close()` after any `dup()`/`fork()` — and
frees it. That is why `memdev_test`'s `dup()` test sees *shared* counters
while a second `open()` gets its own.

**The `*ppos` convention.** `read`/`write` get `loff_t *ppos`:

```
 read(fd, buf, n)      -> .read(file, buf, n, &file->f_pos)   position = the file's
 pread(fd, buf, n, off)-> .read(file, buf, n, &local_off)     a COPY: f_pos unchanged
 rule: use *ppos, advance it by what you return; return 0 = EOF; never touch
       file->f_pos directly in read/write
```

`memdev_read`: if `*ppos >= len` return 0 (EOF), else copy
`min(count, len - *ppos)` bytes and add that to `*ppos`. A **short read is
normal** (`cat` keeps calling until it gets 0). `memdev_write` returns a
**short write** when the buffer is nearly full and **`-ENOSPC`** when there
is no room at all (returning 0 would make `echo`/`dd` retry forever).
`memdev_misc.c` gets the same semantics from the libfs helpers
`simple_read_from_buffer()`, `simple_write_to_buffer()` and
`generic_file_llseek_size()`.

**`llseek`.** `SEEK_SET`/`SEEK_CUR`/`SEEK_END` relative to 0, `f_pos` and
`len`; positions outside `[0, size]` → `-EINVAL`. `memdev_new_pos()` checks
the offset against the base *before* adding, so `lseek(fd, LLONG_MAX,
SEEK_END)` cannot overflow (signed overflow is undefined behaviour — in the
kernel, too).

**User pointers.** `buf` in `read`/`write` and `arg` in `ioctl` are
addresses in the *calling process*, marked `__user`. The driver must
**never dereference them** (`*buf`, `memcpy`, `strcpy`), because the address

- may be unmapped, `PROT_NONE`, or swapped out,
- may point *into the kernel* (a malicious program passing a kernel
  address to make your driver overwrite kernel memory),
- is blocked by the CPU while in kernel mode on CPUs with **SMAP** (x86)
  or **PAN** (arm64 ARMv8.1+).

| Function | Copies | Returns |
|---|---|---|
| `copy_to_user(void __user *to, const void *from, n)` | kernel → user | **bytes NOT copied** (0 = success) → return `-EFAULT` if non-zero |
| `copy_from_user(void *to, const void __user *from, n)` | user → kernel | same |
| `put_user(x, ptr)` / `get_user(x, ptr)` | one scalar, size from the pointer type | 0 or `-EFAULT` |
| `access_ok(ptr, n)` | only checks "is this range in user space" | done for you inside the functions above |
| `strncpy_from_user`, `memdup_user`, `memdup_user_nul` | strings, allocate + copy | negative errno or length / `ERR_PTR` |

These functions use exception-table fixups: a bad address makes the copy
return early instead of oopsing. They may also **sleep** (to fault a page
in), so never call them with a spinlock held — a mutex is fine
([03](../03-kernel-synchronization/)). `CONFIG_HARDENED_USERCOPY` (on in the
board kernel) additionally checks that the kernel side is a sane object
(not spanning slab objects or the stack frame).

**What goes wrong without them — `memdev_bug.c` (`make BUGS=1`).** Its
`read` `memcpy`s directly into the user buffer. On a PC with SMAP or an
ARMv8.1+ core with PAN even a valid `cat` oopses. On the **BeagleBone AI-64
it works**: the Cortex-A72 is ARMv8.0 and has no PAN, and the board kernel
has `CONFIG_ARM64_SW_TTBR0_PAN=n`. The bug stays hidden until a bad pointer
arrives (`memdev_tool /dev/memdev_bug efault`), and then it is a kernel
oops instead of an `EFAULT`. Sparse (`make C=1`) reports the address-space
mismatch — unless someone silenced it with `__force`, as that file does.

### 3.6 ioctl design (`memdev_ioctl.h`)

`read`/`write` move data; **`ioctl`** is for *control*: "clear", "what is
your size", "resize to N", "give me your statistics". One header defines the
ABI and is included by both sides:

```c
/* kernel/memdev_ioctl.h — uses only <linux/ioctl.h>, <linux/types.h> */
#define MEMDEV_IOC_MAGIC     0xE1
#define MEMDEV_IOC_CLEAR     _IO(MEMDEV_IOC_MAGIC, 0)
#define MEMDEV_IOC_GET_SIZE  _IOR(MEMDEV_IOC_MAGIC, 1, __u64)
#define MEMDEV_IOC_SET_SIZE  _IOW(MEMDEV_IOC_MAGIC, 2, __u64)
#define MEMDEV_IOC_GET_STATS _IOR(MEMDEV_IOC_MAGIC, 3, struct memdev_stats)

/* user/memdev_test.c */
#include "../kernel/memdev_ioctl.h"
```

```
  cmd = _IOR(0xE1, 3, struct memdev_stats)
   31-30  29..16          15..8      7..0
  [ 10 ][ 56 = sizeof ] [ 0xE1 ]  [ 3 ]
   read   arg size       magic      number
```

Rules that keep an ioctl ABI sane:

1. **Encode direction and size** with `_IO/_IOR/_IOW/_IOWR` (directions are
   from *user space's* view: `_IOR` = user reads). Tools like `strace` decode
   them, and a changed struct size changes the number — old binaries get
   `ENOTTY` instead of corrupt data.
2. **Pick a magic** not in `Documentation/userspace-api/ioctl/ioctl-number.rst`
   (0xE1 is free there) and register it for an upstream driver.
3. **Fixed-width types only** (`__u32`, `__u64`), no pointers, no `long`, no
   implicit padding. Then the layout is identical for a 32-bit (armhf)
   program on the 64-bit kernel and `.compat_ioctl = compat_ptr_ioctl` is
   enough. If you need a user pointer inside a struct, use a `__u64` and
   `u64_to_user_ptr()`.
4. **Validate everything**: unknown magic or number → **`-ENOTTY`** (not
   `-EINVAL`; glibc shows "Inappropriate ioctl for device"); bad values →
   `-EINVAL`; bad pointer → `-EFAULT` (from `get_user`/`copy_to_user`).
5. **Zero structs before copying them out** (`memset(&st, 0, sizeof(st))`) —
   padding bytes would leak kernel stack contents to user space.
6. Prefer **sysfs** for single human-readable values and **ioctl** for
   binary, atomic, multi-field operations ([05](../05-sysfs-procfs-debugfs/)).

### 3.7 Many openers, one buffer

```
 process A: fd=open(memdev0)  -> struct file A, memdev_file A --+
 process B: fd=open(memdev0)  -> struct file B, memdev_file B --+--> struct memdev[0]
 thread  C: same fd as A      -> struct file A (shared f_pos)    |     lock, data, len
                                                                 +--   reads, writes
```

Each fop can run on both A72 cores at once, plus a concurrent `ioctl`
resize swapping `data` underneath a `read`. `memdev` serialises all of it
with **one mutex per device** (a sleeping lock — we call
`copy_to_user`/`kvzalloc`, which may sleep, while holding it):

- `mutex_lock_interruptible()` in `read`/`write`: a process waiting for the
  lock can be killed / interrupted (`-ERESTARTSYS`).
- `memdev_resize()` allocates the new buffer **before** taking the lock and
  frees the old one **after** dropping it — the critical section is a memcpy
  and a pointer swap.
- `O_APPEND` is evaluated under the lock, so two concurrent appenders cannot
  pick the same offset: `memdev_test` runs two threads × 200 records and
  checks none is torn or lost.
- **Exclusive open** on one minor (`excl_minor=1`): `test_and_set_bit()` in
  `open`, `clear_bit()` in `release` — atomic, no lock — and a second opener
  gets **`-EBUSY`**. Typical for hardware that cannot be shared (a UART
  in raw mode, a programmer). `O_EXCL` on a device node means something else
  (block devices only).
- `O_TRUNC` (`echo hi > /dev/memdev0`) is **ignored by the VFS for devices**;
  `memdev_open` implements it (len = 0) so the shell behaves as expected.
  The flag is only visible in `.open`: the VFS clears `O_CREAT`, `O_EXCL`,
  `O_NOCTTY` and `O_TRUNC` from `f_flags` right after `open` returns.
- **Blocking I/O, `O_NONBLOCK` and `poll()`** (read waits for data instead of
  returning 0) are implemented with a wait queue in
  [`03-kernel-synchronization`'s `/dev/ksync`](../03-kernel-synchronization/kernel/ksync_chardev.c)
  — read that next. The lock choices are explained there in depth.

---

## 4. Complexity table

| Operation | Time | Why |
|---|---|---|
| `open` | O(1) + one `kzalloc` | cdev lookup is a hash of the dev_t (`kobj_map`) |
| `read`/`write` of n bytes | O(n) copy + mutex | `copy_*_user` is a memcpy with fault handling (~1–4 GB/s on the A72) |
| syscall overhead per call | ~0.3–1 µs on the A72 | mode switch + VFS; use big buffers, not 1-byte reads |
| `llseek` | O(1) | arithmetic + range check |
| `ioctl` CLEAR | O(size) | memset of the whole buffer |
| `ioctl` SET_SIZE | O(new size) alloc+zero + O(min(len, new)) copy | lock held only for the copy |
| `ioctl` GET_STATS | O(1) | 56-byte `copy_to_user` |
| space | `size` bytes per minor (kmalloc ≤ a few pages, vmalloc above) + ~200 B `struct memdev` + 24 B per open | `kvzalloc` chooses |

---

## 5. C vs C++ — and kernel vs user space

The driver is C; the user side could be either. Mapping the two ends of
each system call ([linux-internals/00](../../linux-internals/00-syscalls-and-file-io/)):

| User space call | C++ / library | Driver side | Notes |
|---|---|---|---|
| `open(path, flags)` | `std::fstream`, `std::ifstream` | `.open(inode, file)` | allocate per-open state; `-EBUSY`, `-ENODEV`, `-EPERM` |
| `close(fd)` | destructor / `close()` | `.release(inode, file)` | only on the *last* close of the struct file |
| `read`/`pread` | `istream::read` | `.read(file, buf, n, ppos)` | `pread` passes a copy of the offset |
| `write`/`pwrite` | `ostream::write` | `.write(file, buf, n, ppos)` | |
| `lseek` | `seekg`/`seekp` | `.llseek(file, off, whence)` | |
| `ioctl(fd, cmd, arg)` | (no wrapper) | `.unlocked_ioctl(file, cmd, arg)` | "unlocked": no Big Kernel Lock any more — your locking |
| `poll`/`epoll` | asio, libuv | `.poll(file, pt)` | [03](../03-kernel-synchronization/) |
| `mmap` | — | `.mmap(file, vma)` | [08](../08-dma-and-mmap/) |
| `FILE *` | `std::fstream` object | `struct file` | holds position and flags |
| `errno` | `std::system_error` | negative return value | `-ENOTTY` → errno `ENOTTY` |
| `memcpy` | `std::copy` | **`copy_to_user` / `copy_from_user`** | the one thing with no user-space analogue |

---

## 6. Where it is used in the real world

- **`drivers/char/mem.c`**: `/dev/null`, `/dev/zero`, `/dev/full` (whose
  write returns `-ENOSPC`, just like `memdev`), `/dev/mem`, `/dev/random` —
  one major (1), minors chosen by a table, the purest example of this lesson.
- **`/dev/i2c-N`** (`drivers/i2c/i2c-dev.c`) and **`/dev/spidevB.C`**
  (`drivers/spi/spidev.c`): `alloc_chrdev_region`/`register_chrdev` + a class
  + `device_create` per bus/chip, `read`/`write` for raw transfers and
  `ioctl` (`I2C_RDWR`, `SPI_IOC_MESSAGE(n)`) for structured ones —
  [07](../07-gpio-i2c-spi-drivers/).
- **`/dev/gpiochipN`** (`drivers/gpio/gpiolib-cdev.c`): ioctls return *new
  file descriptors* for requested lines — a modern ioctl ABI with `__u64`
  fields and padding rules exactly as in 3.6.
- **TDA4VM remote cores**: `rpmsg_char` (a module on the board) creates
  `/dev/rpmsg_ctrlN` (ioctl `RPMSG_CREATE_EPT_IOCTL`) and `/dev/rpmsgN`
  endpoints (read/write = messages to the R5F/C66x firmware).
- **Misc devices** everywhere: `/dev/watchdog`, `/dev/kvm`, `/dev/fuse`,
  `/dev/uinput`, `/dev/loop-control`, `/dev/rfkill`, `/dev/ksync` in lesson 03.
- **Exclusive open**: watchdogs (`/dev/watchdog` returns `EBUSY` to a second
  opener), many TTY and programmer drivers.
- **udev rules** in every distribution: `/usr/lib/udev/rules.d/50-udev-default.rules`
  sets `GROUP="dialout"` for TTYs, `GROUP="video"` for `/dev/video*`;
  BeagleBoard images add rules for `gpio`, `i2c`, `spi`, `remoteproc` groups.

---

## 7. Common mistakes and pitfalls

1. **Dereferencing a `__user` pointer** (`memcpy(buf, ...)`, `*arg`). Works
   on the A72 (no PAN), oopses on SMAP/PAN CPUs and on bad pointers, and is
   a security hole everywhere. Always `copy_*_user`/`get_user`/`put_user`;
   run `make C=1` (sparse).
2. **Treating `copy_to_user`'s return value as an error code.** It returns
   the number of bytes *not* copied. `return copy_to_user(...)` hands a
   positive byte count to user space as if it were a read size.
3. **Ignoring `*ppos`** (always reading from offset 0): `cat` loops forever
   because read never returns 0.
4. **Returning 0 from `write` when full**: `echo`/`dd` retry endlessly.
   Return `-ENOSPC` (or block, [03](../03-kernel-synchronization/)).
5. **`-EINVAL` for unknown ioctls** instead of `-ENOTTY`. Some libraries
   probe ioctls and depend on `ENOTTY`.
6. **Struct ABI with `long`, pointers or padding** → 32-bit programs on the
   64-bit board kernel see a different layout. Fixed-width fields, explicit
   padding, `compat_ptr_ioctl`.
7. **Leaking kernel memory** through uninitialised struct padding or a
   partially filled buffer in `copy_to_user`. `memset` first or use
   `= {}`/`kzalloc`.
8. **Making the device live too early**: `cdev_add`/`misc_register` before
   the buffer, lock or wait queue is initialised. The first `open` can come
   from another CPU before `cdev_add` even returns.
9. **Wrong teardown order** or forgetting `fops.owner = THIS_MODULE`: rmmod
   while a file is open → the next `read` jumps into freed module code.
10. **Holding a spinlock across `copy_to_user`** — it may sleep. Use a
    mutex, or copy to a bounce buffer first (as `/dev/ksync` does).
11. **Assuming one opener.** Two processes, a `fork()` or two threads on one
    fd all reach your fops concurrently. Lock shared state; keep per-open
    state in `private_data`.
12. **Hard-coding the major** or creating nodes with `mknod` in scripts —
    the dynamic major changes between boots. Let udev create nodes; look
    numbers up in `/proc/devices` or `/sys/class/<cls>/<dev>/dev`.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile; `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `BUGS=1`, `W=1`, `clean` | `make drivers` |
| [kernel/memdev_ioctl.h](kernel/memdev_ioctl.h) | the ioctl ABI shared by driver and user programs: magic, `_IO/_IOR/_IOW`, fixed-width `struct memdev_stats` | included by `memdev.c` and `user/*.c` |
| [kernel/memdev.c](kernel/memdev.c) | `alloc_chrdev_region`, `class_create` + `devnode` mode, `cdev_add`, `device_create`; per-minor `kvzalloc` buffer + mutex; `open`/`release` with `private_data`, O_TRUNC, exclusive minor (`EBUSY`); `read`/`write` with `*ppos`, EOF, short write, `ENOSPC`, O_APPEND; `llseek` with overflow-safe bounds; `ioctl` CLEAR/GET_SIZE/SET_SIZE/GET_STATS, `ENOTTY`, `compat_ptr_ioctl`; load-time self-test; unwind ladder | `insmod memdev.ko [nr_devs=4 buf_size=4096 max_size=1048576 excl_minor=1]` |
| [kernel/memdev_misc.c](kernel/memdev_misc.c) | the same idea as a misc device (`misc_register`, major 10) using `simple_read_from_buffer`/`simple_write_to_buffer`/`generic_file_llseek_size` | `insmod memdev_misc.ko` |
| [kernel/memdev_bug.c](kernel/memdev_bug.c) | **intentionally broken** (only with `BUGS=1`): `memcpy` to/from `__user` pointers — works on the A72, oopses with PAN/SMAP or bad pointers | `make -C …/kernel BUGS=1`, `insmod memdev_bug.ko` (VM / spare board) |
| [user/memdev_test.c](user/memdev_test.c) | self-checking test: rw/EOF, lseek + EINVAL, pread/pwrite vs f_pos, resize + short write + ENOSPC, ENOTTY/EFAULT (PROT_NONE page), per-open vs `dup()` state, O_TRUNC/O_APPEND, 2 threads appending, exclusive minor EBUSY, misc device | `./build/linux-device-drivers/01-char-devices-and-file-operations/user/memdev_test` |
| [user/memdev_tool.c](user/memdev_tool.c) | small CLI: `info`, `clear`, `size N`, `write TEXT`, `read`, `efault` on any memdev node | `./build/.../user/memdev_tool /dev/memdev0 info` |

**Build** (from the repo root):

```sh
make drivers                                                               # host: modules + user/ programs
make -C linux-device-drivers/01-char-devices-and-file-operations/kernel W=1      # extra warnings
make -C linux-device-drivers/01-char-devices-and-file-operations/kernel BUGS=1   # + memdev_bug.ko
make -C linux-device-drivers/01-char-devices-and-file-operations/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-     # board
aarch64-linux-gnu-gcc -O2 -static -o memdev_test \
     linux-device-drivers/01-char-devices-and-file-operations/user/memdev_test.c -lpthread
aarch64-linux-gnu-gcc -O2 -static -o memdev_tool \
     linux-device-drivers/01-char-devices-and-file-operations/user/memdev_tool.c
make drivers-clean
```

(`BB` is the beaglebone-ai64 workspace's `sources/arm64-multiplatform`
directory, see [../README.md](../README.md#building) and
[00 §3.9](../00-kernel-module-basics/README.md#39-cross-compiling-and-loading-on-the-beaglebone).)

All modules are **build-tested only** on the host (Ubuntu 6.8, zero warnings
with and without `W=1`, checkpatch-clean apart from the file-name-in-header
note this repo's conventions require) and cross-built against the board's
`6.18.38-arm64-k3` PREEMPT_RT tree with its own GCC 15 toolchain. The user
programs build with zero warnings under the repo's flags and sanitizers.
Loading the modules is **your** step, on the board or in a VM — see
[../README.md](../README.md#safety).

**Run** (board or VM, `sudo dmesg -w` in a second terminal):

```sh
sudo insmod memdev.ko && sudo insmod memdev_misc.ko
ls -l /dev/memdev*                 # crw-rw-rw- 1 root root 236, 0 ... /dev/memdev0  (major varies)
grep memdev /proc/devices          # 236 memdev
cat /sys/class/memdev/memdev0/dev  # 236:0
echo hi > /dev/memdev0 ; cat /dev/memdev0          # hi
echo there >> /dev/memdev0 ; cat /dev/memdev0      # hi \n there
./memdev_tool info                 # /dev/memdev0: size 4096 B, len 9 B, reads 2, writes 2, openers 1
dd if=/dev/zero of=/dev/memdev0 bs=1k count=8      # dd: error writing '/dev/memdev0': No space left on device
./memdev_tool size 65536 ; ./memdev_tool clear
./memdev_tool /dev/memdev_misc info  # ioctl(MEMDEV_IOC_GET_STATS): Inappropriate ioctl for device
exec 3</dev/memdev1                                # keep the exclusive minor open on fd 3
cat /dev/memdev1                                   # cat: /dev/memdev1: Device or resource busy
exec 3<&-                                          # close it: free again
./memdev_test
sudo rmmod memdev_misc memdev
```

Expected kernel log:

```
memdev: PASS llseek    bounds 0..size accepted, outside -> -EINVAL, no overflow
memdev: PASS resize    10 -> 4 bytes keeps "0123", grow back zero-fills, 0 / >max -> -EINVAL
memdev: major 236, minors 0..3 -> /dev/memdev0..3, 4096 bytes each (max 1048576), exclusive minor 1
memdev_misc: /dev/memdev_misc ready: major 10, minor 123, 4096 bytes     (minor varies)
memdev_misc: unloaded
memdev: unloaded
```

Expected `memdev_test` output:

```
INFO /dev/memdev0: 4 minors, max_size 1048576, exclusive minor 1
PASS rw       : write 11, read back 11, then read() = 0 (EOF)
PASS seek     : SEEK_END/CUR/SET, <0 and >size -> EINVAL, write at size -> ENOSPC
PASS pread    : pwrite/pread at 0 while f_pos stays 3 (*ppos is a copy)
PASS resize   : 16 B -> short write 16 + ENOSPC, shrink keeps prefix, 0 / 1048576+1 -> EINVAL
PASS errno    : unknown ioctl -> ENOTTY; bad pointer to ioctl/read/write -> EFAULT
PASS per-open : 2nd open() has its own counters, dup() shares the struct file
PASS trunc    : O_TRUNC empties, O_APPEND writes at the end -> "abcd"
PASS threads  : 2 x 200 O_APPEND records, none lost or torn (driver mutex)
PASS excl     : /dev/memdev1 second open -> EBUSY, free again after close
PASS misc     : /dev/memdev_misc read/write/llseek via libfs helpers, no ioctl -> ENOTTY
all memdev checks passed
```

Exit code 0 = all passed, 1 = a check failed (the line says which), 2 =
module not loaded (or loaded with `excl_minor=0`, which the test cannot
use). `strace -e trace=ioctl ./memdev_tool info` shows the decoded command
(`_IOC(_IOC_READ, 0xe1, 0x3, 0x38)`).

The broken driver, **in a VM or on a spare board only**:

```sh
make -C .../kernel BUGS=1 ...; sudo insmod memdev_bug.ko
cat /dev/memdev_bug                    # board (A72, no PAN): prints the text - the bug hides
                                       # PC/VM with SMAP or PAN: oops already here
./memdev_tool /dev/memdev_bug efault   # board: "Unable to handle kernel paging request" oops,
                                       # memdev_tool is killed; reboot afterwards (kernel tainted D)
./memdev_tool /dev/memdev0 write x && ./memdev_tool /dev/memdev0 efault
                                       # correct driver: "read into a PROT_NONE page -> EFAULT"
```

---

## 9. Exercises

**Easy**
1. Load `memdev` twice with different `nr_devs` and compare
   `/proc/devices`, `/sys/class/memdev/` and `ls -l /dev/memdev*`. Does the
   major change? Load another module that allocates a char major first.
2. Run `strace cat /dev/memdev0` and `strace dd if=/dev/memdev0 bs=3
   of=/dev/null`. Match every `read()` return value to the code path in
   `memdev_read`.
3. Write the udev rule from 3.3, set `memdev_devnode` back to the default
   (delete the callback), and check owner/group/mode and the `scratchpad`
   symlink.
4. Why does `echo hi > /dev/memdev_misc; cat /dev/memdev_misc` work without
   any ioctl, but `memdev_tool /dev/memdev_misc info` fail? Which errno and
   why exactly that one?

**Medium**
5. Add `MEMDEV_IOC_FILL _IOW(MAGIC, 4, struct memdev_fill { __u64 off, len;
   __u8 byte; __u8 pad[7]; })`. Validate every field, reject non-zero
   padding, extend `memdev_test`. Why the explicit `pad`?
6. Make `memdev` allocate the buffer lazily on the first write (`size`
   stays, `data == NULL` until needed). What changes in `read` and `llseek`?
7. Replace the exclusive-open bit with "one writer, many readers": open with
   `FMODE_WRITE` is exclusive, read-only opens are unlimited.
8. Build a 32-bit ARM (`arm-linux-gnueabihf-gcc -static`) `memdev_test` and
   run it on the 64-bit board kernel (`CONFIG_COMPAT=y`). Then change
   `struct memdev_stats` to use `unsigned long` for one field and watch it
   break. Explain with `_IOC_SIZE`.

**Hard**
9. Make `read` block when `*ppos == len` instead of returning EOF (a
   "tail -f" device), with `O_NONBLOCK` and `.poll` support — reuse the
   wait-queue pattern from [03's /dev/ksync](../03-kernel-synchronization/kernel/ksync_chardev.c).
10. Convert `memdev` to `cdev_device_add()` with a `struct device` per minor
    embedded in `struct memdev`, a `release` callback, and reference
    counting so a minor can be removed (e.g. via an ioctl on another minor)
    while a file is still open on it.
11. Add `.mmap` so user space can map the buffer directly (hint:
    `remap_vmalloc_range` for vmalloc'd memory; what happens to existing
    mappings on SET_SIZE?) — see [08](../08-dma-and-mmap/).
12. Load `memdev_bug.ko` in a QEMU arm64 VM started with `-cpu max` (has
    PAN) and on the board, `cat` it on both, and explain the difference
    from the two oops reports (or the missing one).

---

## 10. Remember forever

```
dev_t = major (driver) + minor (device): alloc_chrdev_region -> cdev_init/cdev_add
      -> class_create + device_create -> uevent -> udev/devtmpfs -> /dev/node.
misc_register = all of that for one node on major 10.
Init everything BEFORE cdev_add/misc_register; teardown in reverse; fops.owner = THIS_MODULE.
inode = the node; struct file = one open (f_pos, f_flags, private_data); fd -> file.
read/write: use *ppos, advance it, return bytes; 0 = EOF; full -> -ENOSPC, never 0.
NEVER touch __user pointers: copy_to_user/copy_from_user (return bytes NOT copied
      -> -EFAULT), get_user/put_user; they may sleep -> mutex, not spinlock.
ioctl: _IO/_IOR/_IOW/_IOWR(magic, nr, type), fixed-width fields, shared header,
      unknown -> -ENOTTY, bad value -> -EINVAL, bad pointer -> -EFAULT, memset structs.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What is in a `dev_t`? How do you find the major of a dynamically
   registered driver from a shell?
2. List the four calls that take a driver from "module loaded" to
   "`/dev/memdev0` exists", and what each one does. Which part is done by
   user space?
3. When does a misc device make more sense than the full cdev path, and
   what does `misc_register` do for you?
4. Explain `struct inode` vs `struct file` vs a file descriptor. What does
   `dup()` share, what does a second `open()` not share?
5. Why must everything be initialised before `cdev_add()`? Why is
   `cdev_del()` followed by freeing the buffer safe in `memdev`'s exit?
6. Describe the `*ppos` convention. What is different for `pread()`?
7. What must `read` return at end of file? What must `write` return when the
   device is full, and what happens if it returns 0?
8. Give three reasons a driver must not dereference a `__user` pointer. Why
   does `memdev_bug` "work" on the BeagleBone AI-64?
9. What does `copy_to_user` return? Write the correct error check.
10. Why can't you call `copy_from_user` with a spinlock held?
11. Decode `_IOR(0xE1, 3, struct memdev_stats)` field by field. Why is the
    direction "R" when the *driver* writes the data?
12. What makes an ioctl struct 32/64-bit safe? When is
    `compat_ptr_ioctl` enough?
13. `-ENOTTY` vs `-EINVAL` vs `-EFAULT` for an ioctl: when is each right?
14. Why `memset` a struct before `copy_to_user`?
15. Two processes write to `/dev/memdev0` with `O_APPEND` at the same time.
    What could go wrong without the mutex, and why is a mutex (not a
    spinlock) the right lock here?
16. How do you make a second `open()` fail with `EBUSY`, race-free?
17. Write a udev rule that gives group `dialout` read/write access to all
    `memdev` nodes and adds a symlink for minor 0.
