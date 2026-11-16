# Linux internals and device drivers: start here

This is the guide for the two Linux tracks:

- [linux-internals/](../linux-internals/README.md): 17 lessons in user space
  (system calls, threads and locks, IPC, memory, real time, fast I/O).
- [linux-device-drivers/](../linux-device-drivers/README.md): 10 lessons in
  the kernel (modules up to real GPIO/I2C/SPI/DMA drivers on the BeagleBone
  AI-64).

It tells you **where to start, what order to go in, how to study each lesson,
and how to check you have really learned it**. Progress dates and review
slots are tracked in [ROADMAP.md](ROADMAP.md). How to make it stick is in
[LEARNING_METHOD.md](LEARNING_METHOD.md).

---

## 1. The whole path in one picture

```
 DSA phase 1–2 (pointers, memory, debugging, lists, ring buffers)        ← prerequisite
        │
        ▼
 ┌──────────────────────── linux-internals (user space) ───────────────────────┐
 │ Part 0  00 syscalls + file I/O                                    (week 1)  │
 │ Part 1  01 threads → 02 atomics → 03 mutex → 04 condvar → 05 semaphore      │
 │         → 06 spinlock/futex → 07 rwlock/seqlock/RCU → 08 barrier/pools      │
 │                                                                 (wks 2–5)   │
 │ Part 2  09 pipes (+IPC map) → 10 mqueue → 11 shm → 12 unix sockets          │
 │         → 13 signals/eventfd/epoll                              (wks 6–8)   │
 │ Part 3  14 virtual memory → 15 scheduling/real time → 16 zero-copy/io_uring │
 │                                                                 (wks 9–10)  │
 └─────────────────────────────────────────────────────────────────────────────┘
        │   (gate: checklist A–D ticked, milestone project 4 started)
        ▼
 ┌──────────────────────── linux-device-drivers (kernel) ──────────────────────┐
 │ Phase 1  00 module basics → 01 char device → 02 kernel memory   (wks 11–12) │
 │ Phase 2  03 kernel synchronisation → 04 interrupts/timers/work  (wks 13–14) │
 │ Phase 3  05 sysfs/procfs/debugfs → 06 device model + device tree            │
 │          → 07 GPIO/I2C/SPI → 08 DMA + mmap → 09 debugging/tracing           │
 │                                                       (wks 15–18, on board) │
 └─────────────────────────────────────────────────────────────────────────────┘
        │
        ▼
 Capstone: a real driver + user-space app on the BeagleBone AI-64 (section 6)
```

The weeks assume about **1–1.5 hours a day, 5–6 days a week**. Go slower
rather than skip the "rewrite from memory" step. That step is what makes it
stick.

**Shortcut if you are short on time:** lessons 00, 01, 02, 03, 04, 09, 11, 13
and 14 in internals, then drivers 00, 01, 03, 04 and 06. That's the minimum an
embedded Linux engineer is expected to know.

---

## 2. Before you start

### Prerequisite checklist
- [ ] I can explain pointers, pointer arithmetic and `void*` ([dsa/01](../dsa/01-pointers/README.md))
- [ ] I can draw the process layout (text, data, bss, heap, stack) and explain malloc/free ([dsa/02](../dsa/02-memory-management/README.md))
- [ ] I can use gdb, valgrind and ASan on a crashing program ([dsa/03](../dsa/03-debugging/README.md))
- [ ] I know linked lists including the kernel `list_head` ([dsa/06](../dsa/06-linked-lists/README.md))
- [ ] I can write a ring buffer ([dsa/07](../dsa/07-stacks-and-queues/README.md))
- [ ] I can set, clear and test bits in a register ([dsa/18](../dsa/18-bit-manipulation/README.md))

### Set up the PC once
```bash
sudo apt install build-essential gdb valgrind strace ltrace linux-headers-$(uname -r) gcc-12
make test               # everything builds and passes (ASan + UBSan)
make test-tsan          # linux-internals again under ThreadSanitizer
make drivers            # kernel modules build against this PC's kernel
make drivers-clean
```

### The board (needed from drivers phase 1 onwards)
- A BeagleBone AI-64 running the kernel from the `beaglebone-ai64` workspace.
  Build modules for it with the `KDIR=`/`CROSS_COMPILE=` command in
  [linux-device-drivers/README.md](../linux-device-drivers/README.md).
- A serial console cable (you will want it the first time a module oopses).
- For phase 3: a push button and resistor, jumper wires, and optionally an
  I2C sensor or EEPROM.
- No board yet? Use a QEMU VM with a matching kernel. `i2c-stub` and
  `gpio-sim` let you test the I2C/GPIO drivers without hardware.

**Rule: never `insmod` lesson modules on your desktop PC.** A locking bug in
kernel code can freeze or corrupt the machine you are working on.

---

## 3. How to study one lesson (the loop)

Do this for every lesson, in order:

1. **Read** the README through section 3 ("How it works"). Draw the ASCII
   diagrams yourself on paper.
2. **Run** each program from section 8 ("Files in this lesson"):
   ```bash
   make run P=linux-internals/03-mutexes-and-deadlock/c/01_mutex_basics.c
   make run P=linux-internals/03-mutexes-and-deadlock/c/01_mutex_basics.c SAN=tsan
   ```
   Kernel lessons: build with `make -C linux-device-drivers/<lesson>/kernel`,
   copy to the board, `insmod`, read `dmesg`, run the `user/` program,
   `rmmod`.
3. **Break it.** Remove a lock, swap two lines, drop a `copy_from_user`. Watch
   TSan, ASan, lockdep or the oops tell you. Then run the `*_bug` programs
   and find the bug before reading the header.
4. **Rewrite the core from memory** in a blank file: the smallest version of
   the main program. Compare it with the original.
5. **Recall card** (section 10): read it, close it, write it out.
6. **Self-test questions** (section 11): answer out loud. Any you can't
   answer means going back to that section.
7. **One exercise** from section 9, the hardest you can manage.
8. Tick the lesson's boxes in section 5 below and record the date in
   [ROADMAP.md](ROADMAP.md). Review on the R1–R5 schedule.

---

## 4. Weekly plan

| Week | Lessons | End-of-week proof |
|---|---|---|
| 1 | internals 00 | `strace -c` a program and explain every syscall in the output |
| 2 | 01, 02 | explain why `counter++` races and show TSan catching it |
| 3 | 03, 04 | MPMC bounded queue from memory, clean under TSan |
| 4 | 05, 06 | futex mutex from memory; explain spin vs sleep |
| 5 | 07, 08 | thread pool from memory; **checklist B** |
| 6 | 09, 10 | parent/child pipeline and mqueue producer/consumer |
| 7 | 11, 12 | shm ring between two processes; pass an fd over a Unix socket |
| 8 | 13 | epoll event loop with signalfd + eventfd + timerfd; **checklist C** |
| 9 | 14 | explain a page fault end to end; read `/proc/self/smaps` |
| 10 | 15, 16 | periodic RT loop with measured latency on the board; **checklist D** |
| 11 | drivers 00, 01 | char device with ioctl, loaded on the board |
| 12 | 02 | slab cache + rbtree/xarray module; **checklist E** |
| 13 | 03 | blocking read + poll driver; read a lockdep splat |
| 14 | 04 | threaded IRQ + workqueue; GPIO button on the board; **checklist F** |
| 15 | 05, 06 | platform driver bound through your own DT overlay |
| 16 | 07 | I2C client driver with regmap (i2c-stub or a real sensor) |
| 17 | 08 | mmap a DMA buffer to user space; dmaengine copy |
| 18 | 09 | decode an oops, trace your driver with ftrace; **checklist G** |
| 19+ | capstone (section 6) | |

---

## 5. Checklists, in sequence

Tick a box only when you can do the item **without looking**.

### A. Part 0: the system call boundary
**00 System calls and file I/O**
- [ ] I can explain user vs kernel mode (EL0/EL1) and what happens during a syscall
- [ ] I can explain the vDSO and why `clock_gettime` doesn't enter the kernel
- [ ] I can draw fd table → open file description → inode, and explain what `dup` and `fork` share
- [ ] I always loop on short reads/writes and retry `EINTR`
- [ ] I can explain stdio buffering, why output is lost after `fork`/crash, and when to `fflush`
- [ ] I know what `fsync` guarantees and why it matters on an SD card or eMMC
- [ ] I can use `flock`/`fcntl` locks, `ioctl`, `/proc` and `/sys`, and read an `strace` log

### B. Part 1: threads and synchronisation
**01 Processes and threads**
- [ ] fork/exec/waitpid from memory, with exit-status macros, zombies and orphans
- [ ] I know what threads share and what is private, and how `clone()` makes both
- [ ] I can pass arguments and results to threads safely (no `&loop_var`)
- [ ] I can use thread attributes, TLS, scheduling policy and CPU affinity

**02 Race conditions and atomics**
- [ ] I can define data race vs race condition and give an example of each
- [ ] I can explain why `volatile` is not synchronisation
- [ ] I can write a CAS loop, and explain relaxed vs acquire/release vs seq_cst with the message-passing example
- [ ] I can explain ARM's weak ordering vs x86 TSO, the ABA problem and false sharing

**03 Mutexes and deadlock**
- [ ] I know the mutex types (normal/errorcheck/recursive) and robust mutexes
- [ ] I can tell the Mars Pathfinder priority-inversion story and how priority inheritance fixes it
- [ ] I know the four Coffman conditions and prevent deadlock with lock ordering
- [ ] I can make TSan report a lock-order inversion, and use `std::scoped_lock`

**04 Condition variables**
- [ ] I can write the `while (!pred) wait` loop and explain spurious and lost wakeups
- [ ] I can explain why wait takes the mutex, and signal vs broadcast
- [ ] MPMC bounded queue with `close()` from memory; timed waits on CLOCK_MONOTONIC

**05 Semaphores**
- [ ] I can explain semaphore vs mutex (ownership, priority inheritance, signalling)
- [ ] I can write a resource pool and producer/consumer with counting semaphores
- [ ] I know named vs unnamed vs System V semaphores, and `sem_post` from a signal handler

**06 Spinlocks and futex**
- [ ] I know when spinning beats sleeping, and why user-space spinlocks are usually wrong
- [ ] TAS → TTAS → ticket lock from memory, with the right memory orders
- [ ] I can explain the futex fast/slow path and write Drepper's 3-state mutex

**07 Read-write locks, seqlocks and RCU**
- [ ] I can explain writer starvation and the upgrade deadlock
- [ ] Seqlock from memory, and when it beats an rwlock
- [ ] I can explain RCU's read side, publish, grace period and reclamation

**08 Barriers, once and thread pools**
- [ ] Reusable barrier with a generation counter from memory
- [ ] I can explain why naive double-checked locking is broken and fix it with atomics
- [ ] Thread pool with graceful shutdown from memory (C and C++)

### C. Part 2: inter-process communication
**09 Pipes and FIFOs**
- [ ] I can pick the right IPC for a given problem using the overview table
- [ ] I know the EOF rule (close unused ends), `PIPE_BUF` atomicity and SIGPIPE/EPIPE
- [ ] I can build `ls | wc -l` with fork + dup2 + exec, and use FIFOs between unrelated processes

**10 Message queues**
- [ ] POSIX mq with priorities, timeouts and `mq_notify`; System V receive-by-type
- [ ] I can compare it with a FreeRTOS queue

**11 Shared memory**
- [ ] `shm_open` + `mmap` from memory; `memfd`; System V shm
- [ ] I can put process-shared mutexes/condvars in shm and recover a robust mutex after the owner dies
- [ ] I store offsets, not pointers, in shared memory, and can explain why

**12 Unix domain sockets**
- [ ] Stream vs datagram vs seqpacket; abstract namespace
- [ ] Length-prefixed framing over a stream socket from memory
- [ ] I can pass an fd with `SCM_RIGHTS` and identify the peer with `SO_PEERCRED`

**13 Signals, eventfd and epoll**
- [ ] I can write `sigaction` handlers that are async-signal-safe
- [ ] I know standard vs real-time signals, signals in threaded programs, and the self-pipe trick
- [ ] signalfd, eventfd and timerfd, and level- vs edge-triggered epoll
- [ ] An epoll event loop that shuts down cleanly on SIGTERM, from memory

### D. Part 3: memory, scheduling and I/O
**14 Virtual memory**
- [ ] I can walk a virtual address through a 4-level page table and the TLB
- [ ] Minor vs major faults, demand paging, copy-on-write, and measuring them
- [ ] mmap / mprotect / madvise / mlock; overcommit and the OOM killer
- [ ] I can read `/proc/<pid>/maps` and `smaps` (VSZ vs RSS vs PSS)

**15 Scheduling and real time**
- [ ] I know CFS/EEVDF and nice, SCHED_FIFO/RR/DEADLINE, and what PREEMPT_RT changes
- [ ] I know which clock to use and why relative sleeps drift; a `TIMER_ABSTIME` loop from memory
- [ ] The RT recipe: mlockall, prefault, SCHED_FIFO, pinning, no malloc/syscalls in the loop
- [ ] I can measure wakeup latency (cyclictest-style) on the board

**16 Zero-copy and async I/O**
- [ ] I can count the copies in read+write vs sendfile vs splice vs mmap
- [ ] O_DIRECT rules, and POSIX AIO vs io_uring
- [ ] I can explain what a DMA-BUF fd is and how GStreamer uses it on the TDA4VM

**Gate before drivers:** A–D ticked, and milestone project 4 (multi-process
sensor logger in [ROADMAP.md](ROADMAP.md)) working.

### E. Drivers phase 1: first modules
**00 Kernel module basics**
- [ ] I know what kernel code cannot do (libc, floating point, big stacks, sleeping anywhere)
- [ ] A hello module with parameters from memory: Kbuild Makefile, build, insmod, dmesg, rmmod
- [ ] Export a symbol from one module and use it in another
- [ ] I can diagnose vermagic, "Unknown symbol" and "Invalid module format" errors

**01 Char devices and file operations**
- [ ] dev_t, cdev, class/device → /dev node; misc devices
- [ ] read/write/llseek with `copy_to_user`/`copy_from_user` and `*ppos`
- [ ] A clean ioctl interface (`_IOR`/`_IOW`, shared header, `-ENOTTY`)

**02 Kernel memory and data structures**
- [ ] kmalloc vs kmem_cache vs vmalloc vs alloc_pages, and GFP_KERNEL vs GFP_ATOMIC
- [ ] devm_* and mempools: why they exist
- [ ] Kernel list/hashtable/rbtree/xarray/IDA/kfifo, and `container_of`

### F. Drivers phase 2: concurrency and asynchronous events
**03 Kernel synchronisation**
- [ ] I know every source of kernel concurrency and the "may not sleep" rule
- [ ] Which spinlock variant (`_bh`, `_irqsave`, `raw_`) for which data, and why
- [ ] Mutex, semaphore, rwsem, completion, wait queue, seqlock, RCU, per-CPU
- [ ] A blocking read() + poll() driver from memory; reading a lockdep splat

**04 Interrupts, timers and deferred work**
- [ ] The path GIC → irq_desc → handler; `/proc/interrupts`; affinity
- [ ] Hard IRQ vs threaded IRQ vs workqueue vs kthread; what PREEMPT_RT changes
- [ ] timer_list vs hrtimer; udelay vs usleep_range vs msleep, and when each is allowed
- [ ] A GPIO button IRQ with debounce working on the board

### G. Drivers phase 3: real drivers on real hardware
**05 sysfs, procfs and debugfs**
- [ ] I can choose between sysfs, procfs, debugfs, ioctl and char device for a given need
- [ ] Device attributes with groups, `sysfs_emit`, `sysfs_notify` + poll
- [ ] seq_file proc entries and debugfs knobs

**06 Device model and device tree**
- [ ] Bus/device/driver matching and probe/remove; deferred probe
- [ ] A platform driver with devm, MMIO (readl/writel) and device properties
- [ ] I can read the BeagleBone AI-64 DTS and write and apply an overlay

**07 GPIO, I2C and SPI drivers**
- [ ] gpiod consumer API and the GPIO char device (no sysfs GPIO)
- [ ] An I2C client driver with regmap, tested with i2c-stub or real hardware
- [ ] SPI modes, spi_sync, regmap-spi; spidev loopback on the board
- [ ] Pinmux: why a header pin needs it on the TDA4VM

**08 DMA and mmap**
- [ ] Coherent vs streaming DMA, and the ownership/sync rules
- [ ] I can explain cache coherency on ARM, the SMMU/IOMMU, and CMA
- [ ] dmaengine memcpy/slave transfers; mmap of driver memory into user space

**09 Kernel debugging and tracing**
- [ ] Dynamic debug and printk levels; decode an oops to a source line
- [ ] ftrace function_graph, tracepoints (my own TRACE_EVENT) and kprobes
- [ ] Which debug config catches what (KASAN, KCSAN, lockdep, kmemleak, DEBUG_ATOMIC_SLEEP)
- [ ] kgdb or pstore on the board's serial console

---

## 6. Capstone: prove it end to end

Build on the BeagleBone AI-64, from scratch, in a `projects/` folder:

1. **Sensor driver + daemon.** An I2C or SPI sensor driver (regmap, DT overlay,
   sysfs attributes, threaded IRQ on data-ready, blocking read + poll). Pair it
   with a user-space daemon that runs a SCHED_FIFO acquisition thread, writes
   to a shared-memory ring, and has an epoll control loop listening on a Unix
   socket. It shuts down cleanly on SIGTERM.
2. **Measure it.** Interrupt-to-user-space latency with ftrace and the lesson
   15 latency tool, with and without CPU isolation.
3. **Break it on purpose.** Remove a lock and catch it with lockdep/KCSAN.
   Leak memory and catch it with kmemleak. Decode the oops from a NULL
   dereference.

When all three work and you can explain every line, you have covered what
these two tracks teach.

---

## 7. Not covered yet (what to learn next)

These are outside these two tracks for now, roughly in the order they
become useful on this board:

| Topic | Why it matters on the BeagleBone AI-64 |
|---|---|
| **remoteproc / RPMsg** | the TDA4VM's R5F and C7x DSP cores are started and talked to this way |
| **V4L2 / media controller** | camera drivers, capture pipelines (pairs with gstreamer/14) |
| **Power management** | runtime PM, suspend/resume, cpufreq/cpuidle, regulators and clocks (`clk`) |
| **Network drivers** | `net_device`, NAPI, sk_buff, the CPSW Ethernet switch |
| **ARM architecture** | exception levels, GIC, MMU, caches, barriers in assembly (planned track) |
| **Protocols** | UART, I2C, SPI, CAN (MCAN), USB, TCP/IP on the wire (planned track) |
| **Build systems** | Buildroot / Yocto, root filesystems, OTA updates |
| **Bare metal / RTOS** | FreeRTOS or Zephyr on the R5F cores; startup code, linker scripts |
| **Security** | secure boot (the beaglebone-ai64 repo's boot chain), OP-TEE, seccomp, capabilities |
