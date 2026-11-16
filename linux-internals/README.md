# Linux internals track: system calls, concurrency, IPC, memory and real time in C and C++

Embedded Linux is concurrent from the start. A camera app on the BeagleBone
AI-64 has a capture thread, an encoder thread and a network thread. Those
threads talk to other processes, the kernel takes interrupts underneath them,
and the TDA4VM has two Cortex-A72 cores that really do run at the same time.
This track teaches how a program works with the kernel, and how to make it safe and fast:

- **Part 0 (the system call boundary):** how a program asks the kernel for
  anything, and files as the universal interface.
- **Part 1 (threads and synchronisation):** how threads share memory, and
  every lock and wait primitive you will meet: mutex, condition variable,
  semaphore, spinlock, futex, read-write lock, seqlock, RCU, barrier and once.
- **Part 2 (inter-process communication, IPC):** every way two *processes*
  exchange data or wake each other up: pipes, FIFOs, message queues, shared
  memory, Unix sockets, signals, eventfd and epoll.
- **Part 3 (memory, scheduling and I/O):** virtual memory, real-time
  scheduling and timing, and zero-copy / async I/O.

The kernel side (spinlocks with interrupts disabled, `struct mutex`, wait
queues, RCU in drivers) is in
[linux-device-drivers/03-kernel-synchronization](../linux-device-drivers/03-kernel-synchronization/README.md).

## Lessons

### Part 0: the system call boundary
| # | Lesson | Key ideas |
|---|---|---|
| 00 | [System calls and file I/O](00-syscalls-and-file-io/README.md) | user vs kernel mode, how a syscall works, vDSO, errno, file descriptors, short reads/writes, stdio buffering, fsync, file locks, ioctl, /proc and /sys |

### Part 1: threads and synchronisation
| # | Lesson | Key ideas |
|---|---|---|
| 01 | [Processes and threads](01-processes-and-threads/README.md) | fork/exec/wait, what threads share, pthread create/join/detach, attributes, TLS, clone(), scheduling and affinity, `std::thread` |
| 02 | [Race conditions and atomics](02-race-conditions-and-atomics/README.md) | data race vs race condition, `volatile` is not synchronisation, C11/C++ atomics, CAS, memory ordering, ARM vs x86, ABA, false sharing |
| 03 | [Mutexes and deadlock](03-mutexes-and-deadlock/README.md) | pthread mutex types, trylock/timedlock, robust and priority-inheritance mutexes, priority inversion, lock ordering, `std::scoped_lock` |
| 04 | [Condition variables](04-condition-variables/README.md) | the wait loop, spurious and lost wakeups, signal vs broadcast, MPMC bounded queue, monotonic timed waits |
| 05 | [Semaphores](05-semaphores/README.md) | counting/binary, semaphore vs mutex, unnamed/named POSIX and System V semaphores, posting from a signal handler, FreeRTOS mapping |
| 06 | [Spinlocks and futex](06-spinlocks-and-futex/README.md) | `pthread_spinlock`, TAS/TTAS/ticket/MCS locks, backoff, futex syscall, a futex mutex from scratch, spin vs sleep benchmark |
| 07 | [Read-write locks, seqlocks and RCU](07-rwlocks-seqlocks-and-rcu/README.md) | `pthread_rwlock`, writer starvation, `std::shared_mutex`, seqlock, user-space RCU and grace periods |
| 08 | [Barriers, once and thread pools](08-barriers-once-and-thread-pools/README.md) | `pthread_barrier`, reusable barrier, `pthread_once`/`call_once`, double-checked locking, thread pools in C and C++ |

### Part 2: inter-process communication
| # | Lesson | Key ideas |
|---|---|---|
| 09 | [Pipes and FIFOs](09-ipc-pipes-and-fifos/README.md) | **IPC overview and "which one should I pick"**, pipe + fork, EOF rules, `PIPE_BUF` atomicity, SIGPIPE, mkfifo, splice |
| 10 | [Message queues](10-ipc-message-queues/README.md) | POSIX `mq_*` with priorities and `mq_notify`, System V `msgget`/`msgrcv` by type, comparison with RTOS queues |
| 11 | [Shared memory](11-ipc-shared-memory/README.md) | `shm_open` + `mmap`, `memfd`, System V shm, process-shared and robust mutexes, a lock-free ring between processes |
| 12 | [Unix domain sockets](12-ipc-unix-sockets/README.md) | stream/datagram/seqpacket, abstract namespace, message framing, fd passing with `SCM_RIGHTS`, peer credentials |
| 13 | [Signals, eventfd and epoll](13-signals-eventfd-and-epoll/README.md) | `sigaction`, real-time signals, async-signal-safety, signalfd, eventfd, timerfd, an epoll event loop |

### Part 3: memory, scheduling and I/O performance
| # | Lesson | Key ideas |
|---|---|---|
| 14 | [Virtual memory](14-virtual-memory/README.md) | MMU, page tables, TLB, page faults, demand paging, copy-on-write, mmap/mprotect/madvise, mlock, overcommit and OOM, huge pages, RSS vs PSS |
| 15 | [Scheduling and real time](15-scheduling-and-real-time/README.md) | CFS/EEVDF, nice, SCHED_FIFO/RR/DEADLINE, PREEMPT_RT, clocks, drift-free periodic loops, POSIX timers, latency measurement, the RT application recipe |
| 16 | [Zero-copy and async I/O](16-zero-copy-and-async-io/README.md) | where copies come from, O_DIRECT, sendfile/splice/copy_file_range, mmap I/O, MSG_ZEROCOPY, POSIX AIO, io_uring, DMA-BUF |

Follow the order in [docs/ROADMAP.md](../docs/ROADMAP.md). Before this track,
finish DSA phase 1 (pointers, memory, debugging). Read
[dsa/07 §3.8 (lock-free SPSC ring)](../dsa/07-stacks-and-queues/README.md) and
[dsa/19 ring buffer](../dsa/19-system-design-structures/README.md) any time;
lessons 02, 04 and 11 build on them.

## Picking a primitive (the one-page version)

```
Protecting shared data between THREADS
  single variable, simple update ............ atomic (02)
  short critical section, may block ......... mutex (03)               <- default
  very short, never sleeps, multi-core ...... spinlock (06)            (rare in user space)
  many readers, few writers ................. rwlock (07) -> seqlock / RCU when reads dominate
Waiting for something to happen
  wait for a condition on shared state ...... condition variable (04)
  count resources / signal across threads ... semaphore (05)
  everyone reaches a point .................. barrier (08)
  run init exactly once ..................... pthread_once / call_once (08)
Between PROCESSES
  stream of bytes, parent <-> child ......... pipe (09)
  unrelated processes, by name .............. FIFO (09) or Unix socket (12)
  discrete messages with priority ........... POSIX message queue (10)
  large data, highest bandwidth ............. shared memory + a lock or ring (11)
  pass an fd / know who the peer is ......... Unix socket SCM_RIGHTS / SO_PEERCRED (12)
  "wake up" notification only ............... eventfd / signal (13)
  wait on many of the above at once ......... epoll (13)
```

## Testing for races: ThreadSanitizer

Every program here also has to be clean under **ThreadSanitizer**, which
finds data races and lock-order inversions (potential deadlocks) that ASan
cannot see:

```bash
make run P=linux-internals/03-mutexes-and-deadlock/c/01_mutex_basics.c SAN=tsan
make test-tsan             # every linux-internals program under TSan
```

TSan needs ASLR off on recent Ubuntu kernels; the Makefile runs the program
under `setarch -R` for you. TSan sees only threads inside one process. For
bugs that span processes (lessons 09–13), the program's own checks are the
test.

Programs named `*_bug.c` / `*_bug.cpp` contain a planted race, deadlock or
lost wakeup. `make test` builds them but does not run them. Their header
says which tool exposes the bug.
