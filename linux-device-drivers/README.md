# Linux device drivers: kernel-side C for the TDA4VM

The [linux-internals](../linux-internals/) track teaches threads, locks,
memory ordering and IPC from **user space**. This track crosses the system
call boundary: the same ideas written as **kernel modules**, the way a
driver for the BeagleBone AI-64 (TI TDA4VM / J721E, 2× Cortex-A72, kernel
`6.18.x-k3` arm64, PREEMPT_RT) has to write them — and each lesson maps
every kernel API back to the user-space one you already know.

Every lesson follows the same shape as the rest of the repo
([CONVENTIONS](../docs/CONVENTIONS.md)): analogy, ASCII diagrams, how it
works, real-world drivers, pitfalls, exercises, a recall card. The modules
are **self-testing**: on `insmod` they run their tests in kthreads, print
`PASS`/`FAIL` lines to the kernel log and return 0 so you can `rmmod` them.

## Layout

```
linux-device-drivers/
└── NN-topic/
    ├── README.md        the lesson
    ├── kernel/          out-of-tree module(s)
    │   ├── Makefile     Kbuild: accepts KDIR=, ARCH=, CROSS_COMPILE=, W=1, clean
    │   └── *.c          one or more modules
    └── user/            user-space test programs (C), need the module loaded
        └── *.c
```

- `kernel/` is built by Kbuild **against a kernel tree** (`KDIR`), not by
  the repo's normal compiler rules. The `.ko` files land next to the
  sources; `make drivers-clean` removes them.
- `user/*.c` are built like every other program into `build/<same path>`
  (with ASan/UBSan), but `make test` does **not** run them: they need the
  module loaded and usually root.

## Building

```sh
# Host (the running kernel's headers: /lib/modules/$(uname -r)/build)
sudo apt install linux-headers-$(uname -r) build-essential
make drivers                        # every lesson's kernel/ + user/
make -C linux-device-drivers/01-kernel-synchronization/kernel W=1   # one lesson, extra warnings

# Board: against the configured + built 6.18.x-k3 tree (it must contain
# Module.symvers), with the SAME compiler that built that kernel.
# In the beaglebone-ai64 workspace that is GCC 15 from its dl/ directory:
BB=~/beaglebone-ai64/sources/arm64-multiplatform          # adjust the path
make drivers KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
# Ubuntu's aarch64-linux-gnu-gcc (11) is too old for that tree's flags:
#   "unrecognized command-line option '-ftrivial-auto-var-init=zero'"

make drivers-clean                  # remove *.ko / *.o / .*.cmd / Module.symvers
```

On Ubuntu the host kernel was built with a newer `gcc-NN` than the default
`gcc`; each `kernel/Makefile` picks that compiler automatically when it is
installed, otherwise Kbuild warns "the compiler differs from the one used to
build the kernel". A module only loads on the exact kernel it was built for
(`modinfo x.ko | grep vermagic`). The user-space programs for the board:
`aarch64-linux-gnu-gcc -O2 -static -o prog user/prog.c -lpthread`.

## Safety

A kernel module runs with full privileges and no memory protection: a bug
is a kernel oops, a hang, or silent memory corruption — not a segfault.

- **Load test modules on the board or in a VM**, never on a machine you
  cannot afford to reboot. A QEMU arm64 VM with a debug kernel is ideal.
  The modules in this repo are build-tested on the host but are never
  loaded there by the build.
- Watch the log while testing: `sudo dmesg -w` in a second terminal
  (or the serial console on the board). Any `BUG:`, `WARNING:`, `Oops`,
  `KCSAN:` or lockdep report means the test failed, whatever it printed.
- After an oops the kernel is *tainted* and may be inconsistent: reboot
  before trusting further results.
- Files ending in `_bug.c` are **intentionally broken** and are only built
  on request (`BUGS=1`).

**Build a debug kernel for driver work** (VM or a second board kernel —
these options cost performance):

| Option | Catches |
|---|---|
| `CONFIG_PROVE_LOCKING` (lockdep) | lock-order inversions, IRQ-unsafe lock use, recursive locking |
| `CONFIG_DEBUG_ATOMIC_SLEEP` | sleeping in atomic context (`mutex_lock` under a spinlock, …) |
| `CONFIG_DEBUG_SPINLOCK`, `CONFIG_DEBUG_MUTEXES` | uninitialised locks, wrong-owner unlocks |
| `CONFIG_PROVE_RCU` | `rcu_dereference()` outside a read section |
| `CONFIG_KCSAN` | data races (concurrent unmarked accesses) |
| `CONFIG_KASAN`, `CONFIG_DEBUG_KMEMLEAK` | out-of-bounds, use-after-free, leaks |
| `CONFIG_DEBUG_OBJECTS_TIMERS`, `CONFIG_DEBUG_OBJECTS_RCU_HEAD` | freeing an active timer / RCU head |

The board's production kernel has none of these enabled (and lockdep/KCSAN
are not compatible with every config, e.g. KCSAN conflicts with KASAN), so
use a separate debug build.

## Lessons

| # | Lesson | You will be able to... |
|---|--------|------------------------|
| 01 | [Kernel synchronization](01-kernel-synchronization/) | name every source of concurrency in the kernel and the "may not sleep" rule; choose between `atomic_t`, `refcount_t`, bitops, barriers, `spinlock_t` (`_bh`/`_irqsave`/`raw_`), mutex, semaphore, rw-semaphore, completion, wait queue, seqlock, RCU and per-CPU data; implement a blocking `read()` + `poll()`; read lockdep splats |
