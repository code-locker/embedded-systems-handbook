---
name: kernel-module-lesson
description: Write, build and (on the board or a VM) run a linux-device-drivers/ lesson in the embedded-systems-handbook — out-of-tree module in <lesson>/kernel/ with a two-personality Kbuild Makefile, user/ test programs, self-testing modules that print PASS/FAIL to dmesg, host build against Ubuntu headers (right gcc), cross build against the BeagleBone AI-64 6.18.x-k3 PREEMPT_RT tree, W=1/checkpatch, and the never-insmod-on-the-host rule. Use when adding or changing kernel code in this repo or when a module build fails.
---

# Kernel module lesson

Reference implementation: `linux-device-drivers/01-kernel-synchronization/`.

## Layout

```
linux-device-drivers/NN-topic/
├── README.md        11 sections (docs/CONVENTIONS.md) + "expected dmesg output"
├── kernel/Makefile  Kbuild: obj-m list when KERNELRELEASE is set, else calls $(KDIR) with M=$(CURDIR)
├── kernel/*.c       GPL, SPDX header, module params range-checked
└── user/*.c         built by `make drivers` (ASan/UBSan), need the module loaded, NOT run by make test
```

Copy `01-kernel-synchronization/kernel/Makefile` and change the `obj-m` line.
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
BB=<path to beaglebone-ai64>
make -C linux-device-drivers/<lesson>/kernel \
     KDIR=$BB/sources/arm64-multiplatform/KERNEL ARCH=arm64 \
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
| `*.ko`, `.*.cmd` show in `git status` | run `make drivers-clean`. They are git-ignored, but keep the tree clean anyway |
