# 00 — Kernel Module Basics

> The entry lesson of the driver track: what a loadable kernel module is
> (and what kernel code is *not* allowed to do), its life from `insmod` to
> `rmmod`, logging with `printk`, module parameters, exporting symbols from
> one module to another, Kbuild, `vermagic`/signing/taint, and how to build
> a module on the PC and load it on the BeagleBone AI-64. Five small
> self-testing modules prove each point.

---

## 1. Why this exists

A driver is kernel code. You could build it into the kernel image and
reboot every time you change a line — or you can build it as a **loadable
module** (`.ko`) and link it into the *running* kernel with one command:

```
edit -> make -> scp kmb_hello.ko board: -> insmod -> dmesg -> rmmod -> edit ...
          ~5 s                                          no reboot
```

That loop is how every driver in this track is developed. It is also how a
distribution ships drivers: the BeagleBone image's kernel (`6.18.x-k3`)
comes with **3 122** modules (`/lib/modules/$(uname -r)/kernel/**.ko.xz`) —
the R5F and C66x remote-processor loaders (`ti_k3_r5_remoteproc`,
`ti_k3_dsp_remoteproc`), the CAN controllers (`m_can_platform`), the camera
CSI-2 receiver (`j721e-csi2rx`), the McASP audio interface — and only the
ones the hardware needs get loaded.

But a module is not a program. It runs in the kernel's address space with
the kernel's privileges, under rules that are very different from user
space (no libc, no floating point, a 16 KiB stack, no memory protection).
Before writing real drivers you need those rules, the module lifecycle, and
the build/load tooling — that is this lesson.

---

## 2. Mental model

**A module is a plug-in card pushed into a running machine.** The machine
(the kernel) has a public connector (the *exported symbol table*). A card
can only use pins that are on the connector (exported symbols), it can add
its own pins for later cards (`EXPORT_SYMBOL_GPL`), and the machine refuses
a card built for a different connector revision (`vermagic`). Once inserted
the card is *inside* the machine: if it shorts, the whole machine goes down.

```
 kernel virtual address space (one, shared by everything below)
 +---------------------------------------------------------------------+
 | vmlinux: core kernel          exported symbols (~ printk, kmalloc,  |
 |   .text .data                 misc_register, ...) ---------------+  |
 |                                                                  |  |
 | module area (vmalloc'd)                                          |  |
 |  +-------------------+   calls   +---------------------+         |  |
 |  | kmb_lib.ko        | <-------- | kmb_consumer.ko     | --------+  |
 |  | EXPORT_SYMBOL_GPL |  resolved | U kmb_lib_fletcher16|  calls      |
 |  |  kmb_lib_*        |  at insmod| U printk            |  printk     |
 |  +-------------------+           +---------------------+             |
 +---------------------------------------------------------------------+
      ^ insmod = finit_module(fd, "params", 0)       ^ rmmod = delete_module()
      |                                              |
 /sys/module/kmb_lib/      holders/kmb_consumer  refcnt  parameters/  taint
```

Everything in that box can read and write everything else. A wild pointer
in `kmb_consumer` can corrupt `vmlinux`'s data. There is no "segfault and
the process dies" — there is an **oops** (the kernel kills the current task
and is tainted from then on) or a **panic**.

---

## 3. How it works

### 3.1 What kernel code is — and isn't

| You are used to (user space) | In a module | Why |
|---|---|---|
| libc: `printf`, `malloc`, `strtol`, `qsort`, `pthread_*` | none of it. The kernel has its own: `printk`, `kmalloc`, `kstrtoint`, `sort`, `kthread_*`, `mutex_*` | libc *calls* the kernel; the kernel cannot call itself through syscalls |
| `float`, `double` anywhere | **no floating point** (the build passes `-mgeneral-regs-only` on arm64, `-mno-sse` on x86) | FP/NEON registers hold the interrupted *user* task's values and are not saved on kernel entry. Use integers / fixed point (`kmb_multi_stats.c`), or bracket with `kernel_neon_begin()`/`end()` |
| 8 MiB stack that grows on demand | **16 KiB fixed stack** per task (`THREAD_SIZE`, arm64 and x86-64), shared with whatever called you | an overflow hits a guard page (`CONFIG_VMAP_STACK`) → oops. Big buffers go on the heap (`kmalloc`) |
| a crash kills your process | a crash is an oops/panic of the whole system | no memory protection between kernel parts |
| your symbols are private to your process | every non-`static` global shares one kernel namespace with ~100 000 symbols | keep everything `static`, prefix the rest (`kmb_`) |
| `errno` | functions **return** `-ENOMEM`, `-EINVAL`, ...; pointers carry errors as `ERR_PTR(-E...)` | no thread-local errno in the kernel; the syscall layer sets user `errno` from your negative return |
| page faults are invisible | user memory may only be touched with `copy_from_user` & co. ([01](../01-char-devices-and-file-operations/)) | user pages may be absent, swapped out, or malicious |
| sleep whenever you like | some contexts must never sleep (IRQ, spinlock held) ([03](../03-kernel-synchronization/)) | there is no task to put to sleep |
| C++, exceptions, any style | C (GNU C11) + Rust; **kernel coding style**: tabs (8), 80–100 columns, `snake_case`, K&R braces, `goto` for error paths | `Documentation/process/coding-style.rst`; `scripts/checkpatch.pl` enforces it |

### 3.2 Lifecycle: insmod → init → live → rmmod → exit

```
 insmod kmb_hello.ko count=5          (or: modprobe kmb_hello count=5)
   |  finit_module(fd, "count=5", 0)  syscall, needs CAP_SYS_MODULE (root)
   v
 kernel module loader
   1. check ELF, arch, vermagic, signature (3.8)
   2. allocate module memory, copy sections, apply relocations
   3. resolve undefined symbols against exported ones  -> "Unknown symbol" (3.6)
   4. parse parameters: count=5 -> calls the param's set()   -> "-EINVAL" (3.4)
   5. state = COMING, create /sys/module/kmb_hello/
   6. call module_init function  kmb_hello_init()
        returns 0  -> state LIVE, free .init.text/.init.data
        returns <0 -> undo 5..2, module is gone, insmod prints strerror(-ret) (3.5)
   v
 LIVE: its code runs whenever someone calls into it (fops, IRQ, timers...)
   v
 rmmod kmb_hello                       (delete_module syscall)
   - refuses with EBUSY while refcnt > 0 (open files, holders, try_module_get)
   - state GOING, call module_exit function kmb_hello_exit()
   - remove sysfs, free memory
```

- **`module_init(fn)` / `module_exit(fn)`** name the two entry points. For
  a built-in driver `module_init` becomes an *initcall* run at boot, and
  `module_exit` code is dropped.
- **`__init`** puts a function in `.init.text`, freed after a successful
  init (watch `MemFree` – or just never call an `__init` function later).
  **`__exit`** code is discarded when built in. `__initdata`/`__initconst`
  do the same for data (`kmb_banner` in `kmb_hello.c`).
- **No `module_exit` = cannot be unloaded** (rmmod returns `EBUSY`;
  only `rmmod -f` with `CONFIG_MODULE_FORCE_UNLOAD`, never do that).
- **Return codes**: `0` = success, negative errno = failure. `insmod` shows
  `strerror(-ret)`: `-ENOMEM` → "Cannot allocate memory", `-EINVAL` →
  "Invalid argument", `-ENODEV` → "No such device", `-EBUSY` → "Device or
  resource busy".

The tools:

| Command | Does | Notes |
|---|---|---|
| `insmod ./x.ko a=1` | load *this file* | no dependency handling; needs a path |
| `modprobe x a=1` | load module *by name* from `/lib/modules/$(uname -r)/` **plus its dependencies** | uses `modules.dep` written by `depmod`; options from `/etc/modprobe.d/*.conf` |
| `rmmod x` / `modprobe -r x` | unload (modprobe -r also unloads unused deps) | |
| `lsmod` | list loaded modules, size, refcount, users | formats `/proc/modules` |
| `modinfo x.ko` | show `.modinfo`: license, author, `depends:`, `vermagic:`, `parm:` | works on unloaded files |
| `depmod -a` | rebuild `modules.dep`, `modules.alias`, ... | run after copying modules into `/lib/modules/...` |

### 3.3 Logging: printk, levels, pr_fmt, dev_*

`printk` writes into the kernel's ring buffer; `dmesg` (or `journalctl -k`)
reads it. Every message has a **level**:

| Level | Macro | Use |
|---|---|---|
| 0 `KERN_EMERG` | `pr_emerg` | system unusable (goes to *every* terminal — don't use in drivers) |
| 1 `KERN_ALERT` | `pr_alert` | act immediately |
| 2 `KERN_CRIT` | `pr_crit` | critical HW/SW failure |
| 3 `KERN_ERR` | `pr_err`, `dev_err` | an operation failed |
| 4 `KERN_WARNING` | `pr_warn`, `dev_warn` | suspicious but continuing |
| 5 `KERN_NOTICE` | `pr_notice` | normal but significant |
| 6 `KERN_INFO` | `pr_info`, `dev_info` | informational (these lessons' PASS lines) |
| 7 `KERN_DEBUG` | `pr_debug`, `dev_dbg` | debug — compiled out or controlled by dynamic debug |

- **Console vs ring buffer**: all levels go into the buffer; only levels
  *below* `console_loglevel` (first number of `/proc/sys/kernel/printk`,
  often `4`) also appear on the serial console. On the BeagleBone's UART
  console you therefore see `pr_err` immediately but `pr_info` only in
  `dmesg` — or boot with `loglevel=8` / `dmesg -n 8`.
- **`pr_fmt`**: `#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt` *before the
  includes* prefixes every `pr_*` line of the file with the module name.
- **`dev_*(dev, ...)`** (`dev_err`, `dev_info`, `dev_dbg`) prefix with the
  driver and device name (`memdev memdev0: ...`) — use them as soon as you
  have a `struct device` ([06](../06-device-model-and-device-tree/)).
- **`pr_debug`/`dev_dbg`** are no-ops unless the file is compiled with
  `-DDEBUG` (`CFLAGS_kmb_multi_stats.o := -DDEBUG` in this lesson's
  Makefile) or **dynamic debug** (`CONFIG_DYNAMIC_DEBUG=y`, on in the board
  kernel) switches them on at run time:
  `insmod kmb_hello.ko dyndbg=+p` or
  `echo 'module kmb_hello +p' > /sys/kernel/debug/dynamic_debug/control`.
  Details and ftrace/`trace_printk` are in [09](../09-kernel-debugging-and-tracing/).
- **Never log in a hot path** without `pr_*_ratelimited()` / `dev_*_once()`:
  printk to a 115 200-baud serial console takes ~1 ms per line.
- Printing pointers: `%p` prints a hashed value (no address leaks), `%px`
  the raw one (avoid), `%pK` honours `kptr_restrict`. Sizes: `%zu`, `%lld`
  for `loff_t`/`s64`, `%llu` for `u64`.

### 3.4 Module parameters (`kmb_hello.c`)

```c
static int count = 3;                           /* default */
module_param(count, int, 0444);                 /* name, type, sysfs mode */
MODULE_PARM_DESC(count, "greeting lines (1..16)");
```

| Kind | Declaration | Command line | sysfs reads back |
|---|---|---|---|
| scalar | `module_param(count, int, 0444)` — `byte short ushort int uint long ulong bool invbool` | `count=5`, `count=0x10` | `5` |
| bool | `module_param(verbose, bool, 0644)` | `verbose=1` / `y` / `Y` / just `verbose` | `Y` / `N` |
| string (pointer) | `module_param(name, charp, 0444)` | `name=AI64`, `name="two words"` (quoted for the shell: `'name="two words"'`) | `AI64` |
| string (buffer) | `module_param_string(tag, tag, sizeof(tag), 0444)` | `tag=ai64` (too long → load fails) | `ai64` |
| array | `module_param_array(values, int, &nr_values, 0444)` | `values=1,2,3` (> 8 → load fails) | `1,2,3` |
| custom checks | `module_param_cb(level, &ops, &level, 0644)` | `level=5` | `5` |
| hidden | perm `0` | `secret=7` | *(no file)* |

- **The permission is the sysfs mode** of
  `/sys/module/kmb_hello/parameters/<name>`. `0` = no file, `0444` =
  read-only, `0644` = root can **change it while the module runs**. A
  writable parameter is shared data: the driver must re-read it
  (`READ_ONCE`) and must validate writes — which is why `level` has its own
  `kernel_param_ops.set()` that rejects anything outside 0..7 with `-EINVAL`
  (the shell shows `write error: Invalid argument`). Keep `charp` read-only:
  a write frees the old string under any code still using it.
- **Validate every load-time parameter in init** and return `-EINVAL`
  (see `count`, `name`). A parameter with a bad *format* (`count=abc`) is
  rejected by the core before init: ``kmb_hello: `abc' invalid for parameter
  `count'``. An **unknown** parameter only warns (`unknown parameter 'foo'
  ignored`) and the module loads anyway.
- **Persistent options**: `/etc/modprobe.d/kmb.conf` with
  `options kmb_hello count=5 name=AI64` (used by `modprobe`, not `insmod`).
  For a built-in driver, the kernel command line: `kmb_hello.count=5`.
- **`MODULE_*` metadata** (`MODULE_LICENSE`, `MODULE_AUTHOR`,
  `MODULE_DESCRIPTION`, `MODULE_VERSION`, `MODULE_PARM_DESC`,
  `MODULE_ALIAS`, `MODULE_DEVICE_TABLE`) ends up in the `.modinfo` section
  that `modinfo` prints. `MODULE_LICENSE("GPL")` is functional, not
  decoration: without a GPL-compatible license the kernel taints itself
  (`P`) and refuses to link `_GPL` exports; modpost also warns if
  `MODULE_DESCRIPTION` is missing.

### 3.5 When init fails: error codes and unwinding (`kmb_initfail.c`)

If `module_init` returns an error the module is unloaded immediately and
**`module_exit` is never called**. Everything init acquired must be released
by init itself, in reverse order. The kernel idiom is the **goto ladder**:

```c
    buf = kzalloc(...);            if (!buf)  { err = -ENOMEM; goto err_out; }
    wq  = alloc_ordered_workqueue; if (!wq)   { err = -ENOMEM; goto err_buf; }
    proc = proc_create_single(...);if (!proc) { err = -ENOMEM; goto err_wq;  }
    if (selftest_fails)            {            err = -EIO;    goto err_proc; }
    return 0;

err_proc:  proc_remove(proc);     /* each label undoes ONE step ...        */
err_wq:    destroy_workqueue(wq); /* ... and execution continues downward, */
err_buf:   kfree(buf);            /* so goto err_wq undoes steps 2 and 1   */
err_out:   return err;
```

```
 step:      1 buf     2 wq      3 proc    4 check
 fail at 3: [ok]      [ok]      [FAIL]
 unwind:    kfree <-- destroy <-- goto err_wq
```

`kmb_initfail.ko fail_at=N` injects a failure at step N. The log shows
exactly which resources were undone, `/proc/kmb_initfail` is gone again,
`lsmod` does not list the module and there is nothing to `rmmod`. The
**order** matters: the work item writes into `buf`, so the workqueue is
destroyed (which waits for queued work) *before* `buf` is freed — the
reverse order would be a use-after-free.

Later lessons replace most ladders with **`devm_*` managed resources**
(freed automatically when a device's probe fails or it is unbound,
[06](../06-device-model-and-device-tree/)) and `<linux/cleanup.h>`
(`__free(kfree)`, scope-based). The ladder is still everywhere in
`module_init` code and you must be able to read it.

### 3.6 Exporting symbols between modules (`kmb_lib.c`, `kmb_consumer.c`)

```
 kmb_lib.ko                          kmb_consumer.ko
 u16 kmb_lib_fletcher16(...) {..}    ... = kmb_lib_fletcher16("abcde", 5);
 EXPORT_SYMBOL_GPL(kmb_lib_fletcher16);       |
        |                                     | nm: "U kmb_lib_fletcher16"
        v                                     v
 __ksymtab_gpl: {name, &func, ns}  <--- loader looks the name up at insmod
```

- `EXPORT_SYMBOL(f)` lets any module use `f`; **`EXPORT_SYMBOL_GPL(f)`**
  only modules with a GPL-compatible license. Not exported = private to
  the module even if not `static`.
- Loading a user creates a **dependency**: `lsmod` shows
  `kmb_lib ... 1 kmb_consumer`, `/sys/module/kmb_lib/holders/kmb_consumer`
  exists, and `rmmod kmb_lib` fails with
  `rmmod: ERROR: Module kmb_lib is in use by: kmb_consumer`.
- Load order: `insmod` does not resolve dependencies — inserting the
  consumer first fails with **`Unknown symbol in module`** (dmesg:
  `kmb_consumer: Unknown symbol kmb_lib_fletcher16 (err -2)`).
  `modprobe` gets it right because **`depmod`** read the `depends:` field
  (`modinfo kmb_consumer.ko` → `depends: kmb_lib`) into `modules.dep`.
- Build: modpost checks undefined symbols against `Module.symvers` of the
  kernel and of the same `M=` directory. A consumer in *another* directory
  needs `KBUILD_EXTRA_SYMBOLS=/path/to/kmb_lib/Module.symvers`, otherwise
  `ERROR: modpost: "kmb_lib_fletcher16" [...kmb_consumer.ko] undefined!`.
- **Symbol namespaces** (`EXPORT_SYMBOL_NS_GPL(sym, ns)` +
  `MODULE_IMPORT_NS(ns)`) group exports (`DMA_BUF`, `USB_STORAGE`, ...);
  forgetting the import is a modpost error. The spelling changed in 6.13
  (string literal `"KMB"` instead of the token `KMB`), and the host (6.8)
  and board (6.18) are on opposite sides, so `kmb_lib.h` defines `KMB_NS`
  with one `LINUX_VERSION_CODE` check — the standard way out-of-tree code
  copes with API changes.

### 3.7 Kbuild: obj-m, multi-file modules, flags, M=

Out-of-tree modules are built **by the kernel's own build system** run
against a configured kernel tree (`KDIR`). Each `kernel/Makefile` in this
repo has two personalities (see the comments in it):

```make
ifneq ($(KERNELRELEASE),)          # 2. read by Kbuild inside $(KDIR)
obj-m       := kmb_hello.o kmb_lib.o kmb_consumer.o kmb_multi.o kmb_initfail.o
kmb_multi-y := kmb_multi_main.o kmb_multi_stats.o   # two .c -> one .ko
ccflags-y   := -DKMB_HIST_BUCKETS=8                 # every file in this dir
CFLAGS_kmb_multi_stats.o := -DDEBUG                 # one file
else                               # 1. run by you
KDIR ?= /lib/modules/$(shell uname -r)/build
modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules
endif
```

- `obj-m += x.o` → build `x.ko` from `x.c`. `x-y := a.o b.o` → build `x.ko`
  by linking `a.o` and `b.o`; there must be **no `x.c`**, and
  `module_init`/`MODULE_*` appear in one file only. Non-static functions
  are shared between the files of one module (`kmb_stats_add`) without
  being exported.
- `M=$(CURDIR)` tells Kbuild "build the external module in this
  directory"; output (`.ko`, `.o`, `.mod.c`, `Module.symvers`) lands next to
  the sources. `make clean` removes it.
- `W=1` adds extra warnings, `C=1` runs **sparse** (catches `__user`
  misuse, endianness bugs), `V=1` prints full commands.
- **Kernel headers vs kernel tree**: `/lib/modules/$(uname -r)/build`
  (Ubuntu package `linux-headers-$(uname -r)`) is a *stripped* tree:
  `.config`, generated headers, `Module.symvers`, scripts — enough to build
  modules, not the kernel. For the board you point `KDIR` at the full,
  configured *and built* 6.18 tree (it must contain `Module.symvers`, which
  is only written by a full build).

**In-tree** (part of the kernel source): put the file in e.g.
`drivers/misc/`, add a Kconfig entry and a Makefile line:

```
# drivers/misc/Kconfig
config KMB_HELLO
	tristate "Module basics demo"
	help
	  Say M to build kmb_hello as a module.

# drivers/misc/Makefile
obj-$(CONFIG_KMB_HELLO) += kmb_hello.o
```

`tristate` means `y` (built into `Image`: `obj-y`, initcall at boot, no
`rmmod`), `m` (module: `obj-m`, installed by `make modules_install`) or
`n`. In the beaglebone-ai64 workspace that is done with a kernel patch —
see its `kernel-dev` and `app-kernel-module` skills.

### 3.8 vermagic, modversions, signing, taint

A module contains code compiled against one kernel's **struct layouts,
config options and compiler**. Load it into another and it reads the wrong
offsets → memory corruption. So the loader checks:

| Check | What | Board 6.18 | Host Ubuntu 6.8 |
|---|---|---|---|
| **vermagic** | string in `.modinfo`: release + key config flags (SMP, preempt model, `mod_unload`, arch) | `6.18.38-arm64-k3-r45 SMP preempt_rt mod_unload aarch64` | `6.8.0-138-generic SMP preempt mod_unload modversions` |
| **modversions** (`CONFIG_MODVERSIONS`) | a CRC of each used symbol's prototype/struct types | off | on: mismatch → `disagrees about version of symbol ...` |
| **signature** (`CONFIG_MODULE_SIG`) | module signed with the kernel build's key | off | on but not forced: unsigned → loads, taints `E`, logs `module verification failed` once. With Secure Boot lockdown → `Key was rejected by service` |

`modinfo -F vermagic x.ko` vs `uname -r` on the target is the first thing
to compare when a load fails. "Same version number" is not enough: a
different `.config` (e.g. PREEMPT_RT vs not) changes struct layouts and
the vermagic. `insmod -f` (force) exists — using it is how you get random
memory corruption.

**Taint flags** record that the kernel is running code its developers
cannot vouch for. Bug reports from tainted kernels are often ignored.

| Flag | Meaning | You get it by |
|---|---|---|
| `O` | out-of-tree module loaded | every module in this repo |
| `E` | unsigned module on a signing kernel | loading these on Ubuntu (not on the board) |
| `P` | proprietary module | `MODULE_LICENSE` missing or non-GPL |
| `F` | module force-loaded | `insmod -f` |
| `D` | an oops happened | a kernel bug — reboot before trusting results |
| `W` | a `WARN()` fired | |

See them in an oops line (`Tainted: G           O`), in
`/sys/module/<m>/taint`, and decoded from `/proc/sys/kernel/tainted`
with `tools/debugging/kernel-chktaint`.

### 3.9 Cross-compiling and loading on the BeagleBone

```
 PC (x86-64)                                  BeagleBone AI-64 (aarch64)
 $BB/KERNEL  (6.18 tree, configured+built)
 $BB/dl/gcc-15.2.0-nolibc/... (same GCC)
 make -C .../kernel KDIR=$BB/KERNEL \
      ARCH=arm64 CROSS_COMPILE=...-   ---scp *.ko--->  sudo insmod kmb_hello.ko
 aarch64-linux-gnu-gcc -static user/  ---scp prog-->   ./kmb_params
                                                       dmesg / serial console
```

```sh
BB=~/beaglebone-ai64/sources/arm64-multiplatform          # adjust the path
L=linux-device-drivers/00-kernel-module-basics
make -C $L/kernel KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
modinfo -F vermagic $L/kernel/kmb_hello.ko   # must match `uname -r` on the board
aarch64-linux-gnu-gcc -O2 -static -o kmb_params $L/user/kmb_params.c
scp $L/kernel/*.ko kmb_params debian@<board-ip>:
make -C $L/kernel clean                      # back to host builds
```

- `ARCH=arm64` selects the architecture's Kbuild rules; `CROSS_COMPILE`
  is the toolchain *prefix* (`...-gcc`, `...-ld` are appended).
- **Use the same compiler that built the board kernel** (GCC 15 from the
  workspace). Ubuntu's `aarch64-linux-gnu-gcc` 11 fails on the 6.18 tree's
  flags (`unrecognized command-line option '-ftrivial-auto-var-init=zero'`).
- User programs need only a cross *libc* toolchain; `-static` avoids glibc
  version mismatches with the board's Debian.
- **Installing for modprobe** on the board: `sudo mkdir -p
  /lib/modules/$(uname -r)/extra && sudo cp kmb_*.ko $_ && sudo depmod -a`,
  then `sudo modprobe kmb_consumer` loads `kmb_lib` first. (Or on the PC:
  `make -C $KDIR M=$PWD modules_install INSTALL_MOD_PATH=/tmp/stage`.)

### 3.10 Common errors

| Message | Meaning | Fix |
|---|---|---|
| `insmod: ERROR: could not insert module x.ko: Invalid module format`, dmesg `version magic '6.8.0-138-generic SMP ...' should be '6.18.38-arm64-k3-r45 SMP preempt_rt ...'` | built for another kernel (or another config) | rebuild with `KDIR` = the tree of the *running* kernel; compare `modinfo -F vermagic` with `uname -r` |
| same, dmesg `Invalid architecture in ELF header: 62` | an x86-64 `.ko` on the arm64 board (62 = EM_X86_64) | you forgot `ARCH=arm64 CROSS_COMPILE=...` |
| `bash: ./kmb_params: cannot execute binary file: Exec format error` | user program built for the other CPU | `aarch64-linux-gnu-gcc` for the board |
| `Unknown symbol in module`, dmesg `Unknown symbol kmb_lib_fletcher16 (err -2)` | a symbol the module needs is not exported by anything loaded | load the provider first / use `modprobe`; GPL-only export + non-GPL license also ends here |
| dmesg `disagrees about version of symbol module_layout` (or `no symbol version for ...`) | `CONFIG_MODVERSIONS` CRC mismatch: other kernel build | rebuild against the exact running kernel |
| dmesg `module verification failed: signature and/or required key missing - tainting kernel` | unsigned module on a signing kernel (Ubuntu); loads anyway | harmless for learning; sign with `scripts/sign-file` to avoid |
| `Key was rejected by service` / `Required key not available` | Secure Boot lockdown: unsigned modules forbidden | sign with an enrolled MOK key, or disable Secure Boot on a test machine |
| `insmod: ERROR: ... Invalid argument` + dmesg ``kmb_hello: `abc' invalid for parameter `count'`` | bad parameter value | check `modinfo -p x.ko` |
| `insmod: ERROR: ... File exists` | module already loaded | `rmmod` it first |
| `insmod: ERROR: ... Operation not permitted` | not root (needs `CAP_SYS_MODULE`) or `modules_disabled=1` | `sudo` |
| `rmmod: ERROR: Module x is in use` / `is in use by: y` | refcount > 0: open file, holder module | close the file / remove `y` first |
| build: `ERROR: modpost: "f" [x.ko] undefined!` / `ERROR: modpost: ... uses GPL-only symbol` | provider's `Module.symvers` not visible / license | `KBUILD_EXTRA_SYMBOLS=`, `MODULE_LICENSE("GPL")` |
| build: `warning: the compiler differs from the one used to build the kernel` | host: Ubuntu headers built with gcc-12 | `sudo apt install gcc-12` (the lesson Makefile then picks it) |

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `insmod` of a small module | ~1–10 ms | read file, allocate, relocate, resolve symbols, run init (signature check adds a hash over the file) |
| symbol resolution per undefined symbol | O(log n) over ~10⁴–10⁵ exports | sorted export tables, binary search; modules: `CONFIG_MODULES_TREE_LOOKUP` (on the board) |
| calling an exported function | same as any call (after linking) | relocations are patched once, at load; long branches may go through a PLT veneer on arm64 |
| module memory | ≥ 1 page per section kind (text, rodata, data) + `struct module` | sections are page-aligned for W^X permissions |
| reading a parameter in sysfs | one `get()` call | |
| `printk` | ~1 µs into the ring buffer; ~1 ms per line on a 115 200 baud console if above `console_loglevel` | hence ratelimiting |
| `pr_debug` when disabled (dynamic debug) | one patched-out branch (static key) | essentially free |
| kernel stack | 16 KiB per task, fixed | no growth; `CONFIG_VMAP_STACK` guard page |

---

## 5. C vs C++ — and kernel vs user space

The kernel is written in C (GNU C11) plus, since 6.1, Rust; there is no C++
in the kernel (no exceptions, RTTI or runtime support), so all module code
is C. The user-space equivalents:

| User space | Kernel module | Notes |
|---|---|---|
| `int main(int argc, char **argv)` | `module_init(fn)` | returns 0 or `-errno`; must return, cannot block forever |
| `return` from main / `atexit()` | `module_exit(fn)` | only after a *successful* init |
| `getopt`, `argv` | `module_param*()` | parsed by the core before init |
| `printf` / `syslog(LOG_ERR, ...)` | `pr_err`, `dev_err` | levels map 1:1 to syslog levels |
| `dlopen("libx.so")` | `insmod x.ko` | the kernel is the dynamic linker |
| `__attribute__((visibility("default")))`, `.so` exports | `EXPORT_SYMBOL_GPL` | `static` = not visible outside the file |
| `ldd prog` | `modinfo -F depends x.ko` | |
| `ldconfig` + `ld.so.cache` | `depmod` + `modules.dep` | |
| `errno = ENOMEM; return -1` | `return -ENOMEM` | user `errno` is set from your return by the syscall layer |
| `malloc` / `free` | `kmalloc`/`kfree`, `kvmalloc`/`kvfree` ([02](../02-kernel-memory-and-data-structures/)) | always check for NULL; GFP flags say whether you may sleep |
| `assert()` | `WARN_ON()` (log + continue), `BUG_ON()` (kill the task — avoid) | |
| `double` | integers / fixed point | `kmb_multi_stats.c` |
| C++ RAII | goto ladder, `devm_*`, `__free()`/`guard()` from `<linux/cleanup.h>` | |

---

## 6. Where it is used in the real world

- **Distribution kernels** are mostly modules: the board's kernel ships
  3 122 of them, compressed (`.ko.xz`, `CONFIG_MODULE_COMPRESS_XZ`);
  `udev` loads the right ones at boot via `modalias` matching
  (`MODULE_DEVICE_TABLE`, lesson [06](../06-device-model-and-device-tree/)).
- **TDA4VM coprocessors**: `ti_k3_r5_remoteproc` and `ti_k3_dsp_remoteproc`
  are modules that load firmware into the R5F and C66x cores; `rpmsg_char`
  exposes their message channels — all `=m` in the board config.
- **Vendor/out-of-tree drivers**: GPU kernel drivers (e.g. Imagination's
  `pvrsrvkm` on TI SoCs), Wi-Fi drivers, DKMS packages — built exactly like
  this lesson against kernel headers, rebuilt on every kernel update
  because of vermagic.
- **Module parameters** in production: `options snd_hda_intel power_save=1`,
  `options usbcore autosuspend=-1`, `options brcmfmac roamoff=1` in
  `/etc/modprobe.d/`; `blacklist <module>` to stop autoloading.
- **Symbol exports between modules**: `snd` → `snd_pcm` → `snd_soc_core`
  → `snd_soc_davinci_mcasp`; `videodev` → `v4l2` drivers;
  `drm` → `drm_kms_helper` → `tidss`. `lsmod` shows the "Used by" chains.
- **Secure Boot / signing**: every module of a signed distro kernel is
  signed at build time (`CONFIG_MODULE_SIG_ALL`); with lockdown, unsigned
  out-of-tree modules are rejected.

---

## 7. Common mistakes and pitfalls

1. **Loading test modules on your workstation.** A bug is an oops of your
   desktop, not a crashed process. Use the board or a VM ([../README.md](../README.md#safety)).
2. **Not unwinding on init failure** — or unwinding in the wrong order.
   `module_exit` will not run; leaked `/proc` entries or class names make
   the *next* `insmod` fail with `-EEXIST`.
3. **Calling an `__init` function after init**, or keeping a pointer to
   `__initdata`: that memory is freed (oops on access).
4. **Big arrays on the stack** (`char buf[8192]` in a function): half of the
   16 KiB stack gone. `-Wframe-larger-than=2048` (on in the kernel build)
   warns.
5. **Floating point** in a module: arm64 builds with
   `-mgeneral-regs-only`, so `double` fails to compile; on other
   architectures it may compile and silently corrupt user FP state.
6. **Non-static globals without a prefix**: name clashes with other modules
   (`count` exported by two modules → second load fails) and confusing
   oopses.
7. **Writable `charp` parameters** or writable parameters read once at
   init: a run-time write is silently ignored or races. Use `0444`, or a
   `_cb` parameter with validation and `READ_ONCE`.
8. **Wrong `KDIR` / compiler** for the board: vermagic mismatch, or the
   build fails with unknown compiler flags. Same tree, same GCC.
9. **Forgetting `MODULE_LICENSE("GPL")`**: `_GPL` symbols become "Unknown
   symbol", and the kernel is tainted `P`.
10. **Mismatched load/unload order** with dependencies: insmod the provider
    first, rmmod it last — or use `modprobe`/`modprobe -r`.
11. **`pr_emerg`/`pr_alert` for normal messages**: they are broadcast to all
    terminals. Info is `pr_info`, problems `pr_err`/`pr_warn`.
12. **Using `LINUX_VERSION_CODE` checks for everything**: in-tree code never
    does that (it is always built against one version). Keep version checks
    to genuine API changes and few places (`KMB_NS` in `kmb_lib.h`).

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | two-personality Kbuild makefile: `obj-m`, multi-file `kmb_multi-y`, `ccflags-y`, `CFLAGS_<obj>`, `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `W=1`, host gcc auto-pick | `make drivers` |
| [kernel/kmb_hello.c](kernel/kmb_hello.c) | init/exit, `__init`/`__initconst`/`__exit`, `pr_fmt`, printk levels, `pr_debug` + dynamic debug, every parameter kind (int, charp, bool, hidden, array, string buffer, `module_param_cb` with range check), stack depth vs `THREAD_SIZE` | `insmod kmb_hello.ko name=AI64 values=1,2,3 verbose=1 level=5` |
| [kernel/kmb_lib.c](kernel/kmb_lib.c), [kernel/kmb_lib.h](kernel/kmb_lib.h) | `EXPORT_SYMBOL_GPL`, state behind an exported API, `EXPORT_SYMBOL_NS_GPL` with the 6.13 namespace-spelling change handled in one place | `insmod kmb_lib.ko` |
| [kernel/kmb_consumer.c](kernel/kmb_consumer.c) | calling another module's exports, `MODULE_IMPORT_NS`, dependency / holders / "Unknown symbol" when loaded alone | `insmod kmb_consumer.ko text=hi` (after kmb_lib) |
| [kernel/kmb_multi_main.c](kernel/kmb_multi_main.c), [kernel/kmb_multi_stats.c](kernel/kmb_multi_stats.c), [kernel/kmb_multi_stats.h](kernel/kmb_multi_stats.h) | two source files → one `kmb_multi.ko`; per-dir and per-file flags; integer-only statistics (no FP), `div_u64` | `insmod kmb_multi.ko samples=5000` |
| [kernel/kmb_initfail.c](kernel/kmb_initfail.c) | 4-step init (kzalloc, workqueue, /proc file, self-check) with a goto unwind ladder; `fail_at=N` injects a failure | `insmod kmb_initfail.ko fail_at=3` |
| [user/kmb_params.c](user/kmb_params.c) | reads `/sys/module/kmb_hello/` (initstate, refcnt, taint, parameters), checks ranges; as root writes `level` and checks the EINVAL rejection; checks `kmb_lib/holders/` | `make drivers`, then `sudo ./build/linux-device-drivers/00-kernel-module-basics/user/kmb_params` |

**Build** (from the repo root):

```sh
make drivers                                                      # host: modules + user/ programs
make -C linux-device-drivers/00-kernel-module-basics/kernel W=1   # extra warnings
make -C linux-device-drivers/00-kernel-module-basics/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-   # board (3.9)
make drivers-clean                                                # remove *.ko, *.o, .*.cmd ...
```

All modules are **build-tested only**: on the host (Ubuntu 6.8 headers,
gcc-12, zero warnings with and without `W=1`, checkpatch-clean apart from
the file-name-in-header note this repo's conventions require and the one
deliberate `LINUX_VERSION_CODE` check in `kmb_lib.h`) and cross-built
against the board's `6.18.38-arm64-k3` PREEMPT_RT tree with its own GCC 15.
Loading them is **your** step, on the board or in a VM — see
[../README.md](../README.md#safety).

**Run** (board or VM; `sudo dmesg -w` in a second terminal or on the serial
console):

```sh
sudo insmod kmb_hello.ko name=AI64 values=1,2,3 verbose=1 level=5
grep -r . /sys/module/kmb_hello/parameters
sudo ./kmb_params
echo 9 | sudo tee /sys/module/kmb_hello/parameters/level   # tee: write error: Invalid argument
sudo rmmod kmb_hello
```

Expected kernel log (the stack figure varies):

```
kmb_hello: hello from a loadable module (kernel stack THREAD_SIZE = 16384 bytes)
kmb_hello: hello, AI64! (1/3)
kmb_hello: hello, AI64! (2/3)
kmb_hello: hello, AI64! (3/3)
kmb_hello: INFO params: name="AI64" tag="ai64" verbose=Y level=5 values given=3
kmb_hello: PASS params    count=3 in 1..16, level in 0..7, 3 of 8 array slots, tag NUL-terminated
kmb_hello: PASS array     values[0..3) sum = 6
kmb_hello:   values[0] = 1
kmb_hello:   values[1] = 2
kmb_hello:   values[2] = 3
kmb_hello: PASS setter    "7" accepted; "8", "-1", "abc" -> -EINVAL
kmb_hello: PASS stack     ~1100 of 16384 bytes used in insmod's call chain
kmb_hello: done: all checks passed
kmb_hello: goodbye, AI64 (level was 5 at unload)
```

`kmb_params` (as root, with the export pair also loaded):

```
PASS initstate: live
INFO refcnt   : 0 (users of kmb_hello)
PASS taint    : "O" (O = out-of-tree module)          # "OE" on Ubuntu (unsigned)
PASS secret   : perm 0 parameter has no file in parameters/
PASS ranges   : count=3 in 1..16, level=5 in 0..7
PASS name     : "AI64" (charp)
PASS tag      : "ai64" fits the 16-byte buffer
PASS verbose  : Y (bool shows as Y/N)
PASS values   : 3 element(s) (array of max 8)
PASS reject   : echo 9 > level -> EINVAL, value stays 5
PASS accept   : echo 6 > level -> reads back 6
PASS readonly : count (0444) cannot be written, even by root (Permission denied)
PASS holders  : kmb_lib is held by kmb_consumer (refcnt 1)
all kmb_hello sysfs checks passed
```

Export pair, dependency and the "Unknown symbol" error:

```sh
sudo insmod kmb_consumer.ko        # insmod: ERROR: could not insert module kmb_consumer.ko: Unknown symbol in module
sudo insmod kmb_lib.ko
sudo insmod kmb_consumer.ko
lsmod | grep kmb_                  # kmb_lib   ...  1 kmb_consumer
sudo rmmod kmb_lib                 # rmmod: ERROR: Module kmb_lib is in use by: kmb_consumer
sudo rmmod kmb_consumer kmb_lib
```

```
kmb_consumer: Unknown symbol kmb_lib_calls (err -2)          <- one line per missing
kmb_consumer: Unknown symbol kmb_lib_fletcher16 (err -2)        symbol, any order
kmb_consumer: Unknown symbol kmb_lib_version (err -2)
kmb_lib: PASS selftest  3 Fletcher-16 vectors; exports ready for other modules
kmb_consumer: INFO using kmb_lib 1.0; fletcher16("BeagleBone AI-64") = 0x280a
kmb_consumer: PASS call      kmb_lib_fletcher16("abcde") = 0xc8f0 (known answer)
kmb_consumer: PASS state     kmb_lib call counter 3 -> 5 (state lives in kmb_lib)
kmb_consumer: done: all checks passed; kmb_lib now cannot be removed before us
kmb_consumer: unloaded; kmb_lib may be removed now
kmb_lib: unloaded after 5 checksum calls
```

Multi-file module (`insmod kmb_multi.ko`, default `samples=1000`):

```
kmb_multi: INFO one module from two objects: kmb_multi_main.o + kmb_multi_stats.o, 8 buckets
kmb_multi: stats: 8 buckets of width 10              <- pr_debug, printed because of -DDEBUG
kmb_multi: PASS stats     n=80 min=0 max=79 mean=39.50, every bucket = 10
kmb_multi: PASS overflow  value 100000 counted in the last bucket
kmb_multi: stats: 8 buckets of width 13
kmb_multi: INFO demo      1000 samples: min=0 max=99 mean=47.50 (no floating point)
kmb_multi: done: all checks passed
```

Init failure and unwinding:

```sh
sudo insmod kmb_initfail.ko fail_at=3   # insmod: ERROR: could not insert module kmb_initfail.ko: Cannot allocate memory
lsmod | grep kmb_initfail               # (nothing) - nothing to rmmod
ls /proc/kmb_initfail                   # No such file or directory
sudo insmod kmb_initfail.ko fail_at=9   # ... Invalid argument
sudo insmod kmb_initfail.ko && cat /proc/kmb_initfail && sudo rmmod kmb_initfail
```

```
kmb_initfail: step 1: buffer of 4096 bytes allocated
kmb_initfail: step 2: workqueue created
kmb_initfail: step 3: injected failure (-12)
kmb_initfail: unwind: workqueue destroyed
kmb_initfail: unwind: buffer freed
kmb_initfail: PASS unwind    init returns -12: module not loaded, nothing leaked, no rmmod needed
kmb_initfail: fail_at=9 (0..4) or buf_size=4096 (64..65536) out of range
kmb_initfail: step 1: buffer of 4096 bytes allocated
kmb_initfail: step 2: workqueue created
kmb_initfail: step 3: /proc/kmb_initfail created
kmb_initfail: step 4: self-check ok (checksum 522240, work ran on CPU 1)
kmb_initfail: PASS init      all 4 steps acquired; rmmod releases them in reverse
kmb_initfail: unloaded: released /proc entry, workqueue, buffer
```

(`fail_at=4` additionally prints `unwind: /proc/kmb_initfail removed`
first and `insmod` reports "Input/output error".) Any `FAIL` line, a
`done: N check(s) FAILED`, or any `BUG:`/`WARNING:`/`Oops` means something
is wrong.

---

## 9. Exercises

**Easy**
1. Load `kmb_hello` with `count=0`, `count=abc`, `values=1,2,3,4,5,6,7,8,9`
   and `tag=a_very_long_tag_value`. Which failures come from init, which
   from the parameter parser? Read each dmesg line.
2. `modinfo kmb_hello.ko` on the host build and on the cross build. Which
   fields differ and why? Try to insmod the host build on the board.
3. Load with `dyndbg=+p` and find the `pr_debug` line. Then load without
   it and turn it on via `/sys/kernel/debug/dynamic_debug/control` — why is
   nothing printed? (Hint: when does that line run?)
4. `cat /proc/sys/kernel/tainted` before and after loading a module. Decode
   the number.

**Medium**
5. Add a `ulong` parameter `delay_ms` (0..1000, writable) with
   `module_param_cb` and make `kmb_hello` print a line every `delay_ms`
   with a `delayed_work` — re-reading the value each time. Clean up in exit.
6. Move `kmb_consumer.c` to another directory with its own Makefile. Make
   modpost find the exports with `KBUILD_EXTRA_SYMBOLS`.
7. Remove `MODULE_IMPORT_NS` from the consumer and build. Then change its
   license to `"Proprietary"`. Record both modpost messages.
8. Add a fifth step to `kmb_initfail` (e.g. a misc device) with its own
   label, and a `fail_at=5`. Check every `fail_at` value 0..5 leaves no
   trace (`/proc`, `/dev`, `lsmod`, `kmemleak` on a debug kernel).
9. Install the lesson's modules into `/lib/modules/$(uname -r)/extra` on the
   board, run `depmod -a`, and load the consumer with `modprobe`. Look at
   the line `modules.dep` gained.

**Hard**
10. Put `kmb_hello` in-tree in the board kernel (Kconfig `tristate` under
    `drivers/misc`, Makefile line), build it once as `=m` and once as `=y`.
    For `=y`, pass `kmb_hello.count=5` on the kernel command line and find
    the output in the boot log. Where did `kmb_hello_exit` go?
11. Write a recursive function in a module and measure how deep it goes
    before the stack guard fires — **in a VM only**. Read the oops: which
    line says "stack overflow"/"kernel stack guard page"?
12. Sign `kmb_hello.ko` for a kernel with `CONFIG_MODULE_SIG_FORCE=y` in a
    VM (`scripts/sign-file sha512 signing_key.pem signing_key.x509
    kmb_hello.ko`) and show that the unsigned copy is rejected.

---

## 10. Remember forever

```
Module = ELF linked into the RUNNING kernel: no libc, no FP, 16 KiB stack,
         no protection, one shared namespace (static + prefix everything).
module_init(fn) -> 0 or -errno; on error EXIT IS NEVER CALLED -> goto ladder,
         undo in reverse.  module_exit(fn) only after successful init.
__init/__initdata freed after init; __exit dropped when built in.
pr_fmt before includes; pr_err/warn/info/debug; pr_debug = dynamic debug.
module_param(name, type, perm): perm = sysfs mode; 0644 -> validate & READ_ONCE.
EXPORT_SYMBOL_GPL -> dependency; insmod provider first; modprobe uses depmod.
obj-m := x.o ; x-y := a.o b.o ; make -C $KDIR M=$PWD [ARCH= CROSS_COMPILE=]
vermagic must match the running kernel exactly (version + config + same gcc).
Taint O = out-of-tree, E = unsigned, P = proprietary, D = oopsed -> reboot.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name five things you can do in a user-space program but not in a kernel
   module, and the reason for each.
2. Walk through what the kernel does between `insmod x.ko` and the module
   being `LIVE`. At which step are parameters parsed? Symbols resolved?
3. What happens to a module whose init returns `-ENOMEM`? Is exit called?
   What must init have done before returning?
4. Write the goto ladder for an init that allocates a buffer, registers a
   char device and creates a class. Why does the order of the labels matter?
5. What do `__init` and `__exit` do for a module and for built-in code?
6. Difference between `insmod` and `modprobe`? What does `depmod` produce?
7. A parameter has permission `0644`. What two things must the driver code
   do because of that?
8. `charp` vs `module_param_string`: who owns the memory, and which one can
   fail at load time?
9. Difference between `EXPORT_SYMBOL` and `EXPORT_SYMBOL_GPL`? What does a
   symbol namespace add?
10. Why does `rmmod kmb_lib` fail while `kmb_consumer` is loaded? Where can
    you see the dependency in sysfs?
11. What is in `vermagic`? Why is "same kernel version number" not enough
    for a module to be safe to load?
12. What is `Module.symvers`, who writes it, and why does the board kernel
    tree need a full build before you can build modules against it?
13. You get `Invalid module format` on the board. List the first three
    things you check.
14. What do taint flags `O`, `E`, `P` and `D` mean? Why do kernel developers
    care?
15. Which printk levels reach the BeagleBone's serial console by default,
    and how do you see the others?
16. How do you build one module from three `.c` files, add a `-D` flag for
    all of them, and `-DDEBUG` for only one?
