---
name: kernel-module-lesson
description: Write, build and (on the board or a VM) run a linux-device-drivers/ lesson in the embedded-systems-handbook — out-of-tree module in <lesson>/kernel/ with a two-personality Kbuild Makefile, user/ test programs, self-testing modules that print PASS/FAIL to dmesg, host build against Ubuntu headers (right gcc), cross build against the BeagleBone AI-64 6.18.x-k3 PREEMPT_RT tree, W=1/checkpatch, and the never-insmod-on-the-host rule. Use when adding or changing kernel code in this repo or when a module build fails.
---

# Kernel module lesson

Reference implementation: `linux-device-drivers/03-kernel-synchronization/`.

## Layout

```
linux-device-drivers/NN-topic/
├── README.md        11 sections (docs/CONVENTIONS.md) + "expected dmesg output"
├── kernel/Makefile  Kbuild: obj-m list when KERNELRELEASE is set, else calls $(KDIR) with M=$(CURDIR)
├── kernel/*.c       GPL, SPDX header, module params range-checked
└── user/*.c         built by `make drivers` (ASan/UBSan), need the module loaded, NOT run by make test
```

Copy `03-kernel-synchronization/kernel/Makefile` and change the `obj-m` line.
Bug modules go behind `BUGS=1` so the default build never produces them.

## Build (tested)

```bash
make drivers                                            # repo root: every lesson, host kernel headers
make -C linux-device-drivers/<lesson>/kernel W=1        # extra warnings; must stay at zero
make -C linux-device-drivers/<lesson>/kernel BUGS=1     # include *_bug modules
$KDIR/scripts/checkpatch.pl --strict --no-tree -f linux-device-drivers/<lesson>/kernel/*.c
make drivers-clean                                      # leave no *.ko/*.mod*/*.cmd in the tree
```

Board (BeagleBone AI-64), using the beaglebone-ai64 workspace's kernel tree and **its own** GCC 15:

```bash
BB=<path to beaglebone-ai64>/sources/arm64-multiplatform
make -C linux-device-drivers/<lesson>/kernel \
     KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
```

## Run: only on the board or in a VM

Never `insmod` on the development PC. A locking bug in a lesson can hang or corrupt the
running desktop kernel.

```bash
sudo dmesg -w &                       # watch
sudo insmod ksync_spin.ko threads=4 iters=100000
sudo rmmod ksync_spin                 # init returns 0 even after a FAIL so it can be removed
```

Modules run their tests in `init` (kthreads + `kthread_stop`), print `PASS`/`FAIL` lines with
`pr_info`, and unwind every error path. Debug kernels for the bug modules need
`CONFIG_PROVE_LOCKING`, `CONFIG_DEBUG_ATOMIC_SLEEP` and `CONFIG_DEBUG_SPINLOCK` (and `CONFIG_KCSAN` for races).

## Symptom → cause

| Symptom | Cause / fix |
|---|---|
| host build warns "the compiler differs from the one used to build the kernel" | Ubuntu 6.8 headers were built with gcc-12. The lesson Makefile auto-picks it if installed (`sudo apt install gcc-12`) |
| `aarch64-linux-gnu-gcc: unrecognized ... -ftrivial-auto-var-init=zero` | Ubuntu's cross gcc 11 is too old for the 6.18 tree. Use the workspace GCC 15 prefix above |
| board: `Invalid module format` / vermagic mismatch | board runs another kernel. Compare `modinfo -F vermagic x.ko` with `uname -r` on the board |
| `spin_lock_irqsave` code "sleeps" on the board | board kernel is PREEMPT_RT, where `spinlock_t` is a sleeping lock. Use `raw_spinlock_t` for hard-IRQ (`irq_work`) paths |
| one host kernel and one board kernel need different code | host is 6.8, board is 6.18: guard with `LINUX_VERSION_CODE`. Known splits: `EXPORT_SYMBOL_NS*`/`MODULE_IMPORT_NS` take a string from 6.13 (define `NS` once in a header); `kvrealloc()` lost its old-size argument in 6.12; platform `.remove` returns void from 6.11 (`.remove_new` before; one `#define X_REMOVE` keeps it to one guard); `hrtimer_setup` 6.13; `kthread_run_worker` 6.14; `WQ_BH`/`system_bh_wq` 6.9 (not exported in 6.8); `timer_container_of` (use `#ifndef` → `from_timer`) |
| `__user` pointer dereferenced directly "works" on the board | the A72 is ARMv8.0 with no PAN (`CONFIG_ARM64_SW_TTBR0_PAN=n`), so only invalid pointers oops. x86 SMAP oopses every time. Always `copy_*_user` |
| `ERROR: modpost: "sym" undefined!` | in an `M=` build an undefined symbol is an error: build/load the exporting module first, or add `KBUILD_EXTRA_SYMBOLS` |
| checkpatch complains about `*.mod.c` | Kbuild-generated. Exclude them from the glob, or run checkpatch before building |
| `-Wcomment` under `W=1` | a `/*` inside a comment (e.g. `parameters/*`). Rephrase it |
| `make drivers` fails in a lesson you didn't touch | it builds every lesson. Build yours alone with `make -C <lesson>/kernel` |
| module param "redeclared as different kind of symbol" | the name collides with a kernel inline (e.g. `page_size()`). Use `module_param_named(page_size, my_var, uint, 0444)` |
| `<linux/unaligned.h>` not found on the host | it only exists from 6.12 (`asm/unaligned.h` before). Guard the include or decode bytes by hand |
| `dma_request_chan_by_mask()` returns -517 on the PC | `-EPROBE_DEFER` means no dmaengine provider at all, and `-ENODEV` means none has the capability. Treat both as "fall back to memcpy" |
| dmaengine memcpy corrupts or faults | buffers must be allocated/mapped for `dmaengine_get_dma_device(chan)`, not the client. K3 UDMA is `dma-coherent`, so cache bugs hide on the board; boot with `swiotlb=force` to expose them |
| `-Wformat` printing `SPI_CPOL`/`SPI_MODE_X_MASK` | they are `_BITUL` (unsigned long). Cast to `unsigned int` |
| driver works on hardware but not with `i2c-stub` | i2c-stub has SMBus + I2C-block only, no `I2C_FUNC_I2C`. Choose the transfer method from `i2c_check_functionality` |
| checkpatch "line length" on diagrams | it counts UTF-8 box-drawing bytes. Draw diagrams in ASCII |
| `gpiod_get_value()` on a TDA4VM output pin reads 0 | `PIN_OUTPUT` disables the input buffer. Mux GPIO pads `PIN_INPUT` mode 7 like BeagleBoard's io-pins overlay |
| overlay with `J721E_IOPAD` won't compile | run `cpp -nostdinc -undef -x assembler-with-cpp -I $BB/KERNEL/arch/arm64/boot/dts/ti -I $BB/KERNEL/include` first, then `dtc -@`. Validate with `$BB/KERNEL/scripts/dtc/fdtoverlay -i <base>.dtb -o out.dtb <x>.dtbo` |
| gpio-sim lookup table doesn't match | set the bank `label` in configfs. Default is `gpio-sim.N:<bank>`. Drive inputs via `.../sim_gpioK/pull` |
| need a real IRQ on a PC with no device | `CONFIG_IRQ_SIM` is not on the board. Build a module-private `irq_domain_create_linear` + `irq_chip`, and fire it with `generic_handle_irq_safe()` from an hrtimer (`linux-device-drivers/04`) |
| fake MEM resource clashes in `/proc/iomem` | set `res.parent` to a private root resource before `platform_device_add_resources` |
| overlay on `&main_gpio0` breaks other pins | setting `pinctrl-0` there replaces the base list. Put `pinctrl-0` on your own node |
| optional module not built although its CONFIG is on | `obj-$(CONFIG_X) += foo.o` becomes `obj-y` out of tree. Use `ifdef CONFIG_X` / `obj-m += foo.o` / `endif` |
| out-of-tree `TRACE_EVENT` header not found | add `CFLAGS_<obj>.o := -I$(src)`. Avoid `__string` (`__assign_str` changed in 6.10); use fixed-size fields |
| `faddr2line` gives no file:line for board modules | the board kernel has no `CONFIG_DEBUG_INFO`. Add `CFLAGS_x.o += -g`, then `CROSS_COMPILE=… $KDIR/scripts/faddr2line x.ko func+off/size` (gdb `list *` on a .ko can name the wrong section) |
| `rmmod` blocks for a whole period | a kthread sleeping in `msleep_interruptible()` ignores `kthread_stop()`. Use `schedule_timeout_interruptible()` |
| `kobject_uevent` fails | a bare kobject has no kset. Send uevents on a `struct device` |
| bin_attribute callback type mismatch | `attr` is non-const on 6.8 and `const` on 6.18. Guard with `LINUX_VERSION_CODE` |
| lockdep "non-static key" | `DEFINE_SPINLOCK` inside a function. Use static locks or `spin_lock_init()` |
| `*.ko`, `.*.cmd` show in `git status` | run `make drivers-clean`. They are git-ignored, but keep the tree clean anyway |
