# 13 — Signals, eventfd and epoll

> How a Linux program **gets told** that something happened: a signal from
> another process, a timer firing, a child exiting, a byte arriving, another
> thread finishing a job. Signals are the oldest and most dangerous way;
> `signalfd`, `eventfd` and `timerfd` turn every kind of event into a file
> descriptor; `epoll` waits on all of them at once. Put together, that is
> the **event loop** inside systemd, GLib/GStreamer, libuv (Node.js) and
> nginx.

This is the last lesson of the IPC part. The overview of all IPC
mechanisms is in [09 — Pipes and FIFOs](../09-ipc-pipes-and-fifos/README.md);
the socket side is [12 — Unix sockets](../12-ipc-unix-sockets/README.md).

---

## 1. Why this exists

A daemon on the BeagleBone AI-64 (say, a camera recorder) has to react to:

| Event | Comes as | Fd-based form |
|---|---|---|
| `systemctl stop` / Ctrl-C | `SIGTERM` / `SIGINT` | `signalfd` |
| "re-read your config" | `SIGHUP` | `signalfd` |
| a child (encoder process) died | `SIGCHLD` | `signalfd` (or `pidfd`) |
| every 33 ms: grab a frame | a timer | `timerfd` |
| another thread finished a job | a function call... | `eventfd` |
| a client sent a request | socket readable | the socket itself |

Waiting for these one at a time does not work: if you block in `read()` on
the socket, you miss the timer. You need **one** call that sleeps until
**any** of them happens: `poll()` or `epoll_wait()`. That only works if
everything is a file descriptor, and that is why Linux added `signalfd`,
`eventfd` and `timerfd`.

---

## 2. Mental model

**A signal is a doorbell.** It interrupts whatever you are doing, carries
almost no information (which bell rang, sometimes one number), and if it
rings twice while you are busy you only know it rang (standard signals
coalesce). **An fd-based event is a letter in your mailbox.** It waits
until you look, and `epoll` is the one mailbox you check for all of them.

```
         ASYNCHRONOUS (signal handler)          SYNCHRONOUS (fd + epoll)

  main code   ...a = b + c; malloc(...)        epoll_wait(ep, ...)   <- sleeps here
                     ^                                  |  wakes when ANY fd is ready
   SIGTERM -------->[handler runs HERE,                 v
                     in the middle of               for each ready fd:
                     malloc, holding                  signalfd -> on_signal()
                     its lock!]                       eventfd  -> on_wakeup()
                     only async-signal-               timerfd  -> on_tick()
                     safe calls allowed               socket   -> on_request()
                                                    normal code: any function OK
```

The sentence to remember: **handle signals synchronously: block them and
read them from a `signalfd` (or a `sigwait` thread), then let one `epoll`
loop wait for everything.**

---

## 3. How it works

### 3.1 Signals (`c/01_signal_basics.c`)

A signal is a number (1–31 standard, `SIGRTMIN`–`SIGRTMAX` real-time)
sent by the kernel (`SIGSEGV`, `SIGPIPE`, `SIGCHLD`, `SIGALRM`), by another
process (`kill`, `sigqueue`), by a thread (`pthread_kill`, `tgkill`) or by
yourself (`raise`). For each signal a process has a **disposition**:

| Disposition | Set with | Effect |
|---|---|---|
| default | `SIG_DFL` | the signal's default action (table below) |
| ignore | `SIG_IGN` | discarded at once (not even pending) |
| catch | a handler function | the handler runs, then the program continues |

Default actions (`man 7 signal`), printed by the program:

| Action | Signals |
|---|---|
| terminate | `SIGTERM SIGINT SIGHUP SIGKILL SIGUSR1 SIGUSR2 SIGPIPE SIGALRM` and all real-time |
| terminate + core dump | `SIGSEGV SIGBUS SIGFPE SIGILL SIGABRT SIGQUIT` |
| ignore | `SIGCHLD SIGWINCH SIGURG` |
| stop | `SIGSTOP` (uncatchable), `SIGTSTP SIGTTIN SIGTTOU` |
| continue | `SIGCONT` |

`SIGKILL` and `SIGSTOP` can never be caught, blocked or ignored
(`sigaction` returns `EINVAL`): the system always has a way to stop a
process. The parent sees how a child ended with `waitpid`:
`WIFSIGNALED`/`WTERMSIG`, `WIFSTOPPED` (with `WUNTRACED`),
`WIFCONTINUED` (with `WCONTINUED`).

Every signal has two per-thread/per-process bits of state:

```
   generated --> PENDING --(not blocked)--> DELIVERED: disposition runs
                    ^
                    blocked (in the thread's signal MASK): stays pending
```

### 3.2 `sigaction` (`c/01`)

Always use `sigaction()`, never `signal()` (whose semantics differ
between systems):

```c
struct sigaction sa = {0};
sa.sa_sigaction = handler;            /* void handler(int, siginfo_t *, void *) */
sa.sa_flags = SA_SIGINFO | SA_RESTART;
sigemptyset(&sa.sa_mask);             /* extra signals blocked while handler runs */
sigaction(SIGUSR1, &sa, NULL);
```

- `SA_SIGINFO`: the handler gets a `siginfo_t`: `si_pid` and `si_uid` of
  the sender, `si_code` (`SI_USER` = `kill`, `SI_TKILL` = `raise`/`tgkill`,
  `SI_QUEUE` = `sigqueue`, `SI_TIMER`, ...), `si_value` (the payload of
  `sigqueue`), `si_addr` for faults, `si_status` for `SIGCHLD`.
- `sa_mask`: signals blocked *while the handler runs* (the handled signal
  itself is blocked too, unless `SA_NODEFER`).
- `SA_RESTART`: restart interrupted slow syscalls (3.5).
- Waiting for a signal without a race: **block it, then `sigsuspend()`**
  with a mask that unblocks it. `sigsuspend` unblocks and sleeps in one
  atomic step. `if (!flag) pause();` has a window where the signal arrives
  between the test and `pause()`, and you sleep forever.

### 3.3 Standard vs real-time signals (`c/02_rt_signals_queue.c`)

```
   pending set of a process
   standard 1..31:  one BIT per signal   SIGUSR1 x5 while blocked -> delivered ONCE
   real-time:       a QUEUE per signal   SIGRTMIN x5 (values 100..104)
                                         -> delivered 5 times, FIFO, each with its value
   delivery order:  lowest signal number first (standard before real-time)
```

The program sends five of each while they are blocked and counts: 1
`SIGUSR1`, 5 `SIGRTMIN` with values 100..104 in order. **A standard
signal is "something happened at least once", not a counter.** That is why
a `SIGCHLD` handler must reap in a `while (waitpid(-1, &st, WNOHANG) > 0)`
loop: two children may die and produce one `SIGCHLD`. Real-time signals
queue up to `RLIMIT_SIGPENDING` per user. Always write `SIGRTMIN + n`:
glibc reserves the first kernel RT signals for its thread implementation.

### 3.4 Async-signal-safety (`c/09_unsafe_handler_bug.c`)

A handler can run **between any two instructions** of the main program:
inside `malloc` (heap lock held, free lists half updated), inside `printf`
(stdout lock held, buffer half written). If the handler calls these
functions too:

```
  main:    malloc() -> lock(arena) ... updating lists ...
                                  <-- SIGPROF
  handler:      malloc() -> lock(arena)   <- same thread, NOT recursive
                            waits for itself forever: DEADLOCK
```

Rules (`man 7 signal-safety`):

- Call only **async-signal-safe** functions: `write`, `read`, `_exit`,
  `kill`, `sigaction`, `sem_post`, `clock_gettime`, `open`, `close`, ...
  **Not** `printf`, `malloc`, `free`, `exit`, `pthread_mutex_lock`,
  `syslog`, C++ `new`, `std::cout`.
- Share data with the main program only through `volatile sig_atomic_t`
  or lock-free `_Atomic`/`std::atomic` variables.
- Save and restore `errno` (your `write` may change it under the
  interrupted code's feet).

`c/09_unsafe_handler_bug.c` calls `malloc`/`fprintf` from a `SIGPROF`
handler while the main loop does the same. On the author's PC it
deadlocked in every run (killed by its `alarm(5)` watchdog); on another
machine it may survive many runs, which is what makes it dangerous.
ThreadSanitizer reports it every time:

```
make run P=linux-internals/13-signals-eventfd-and-epoll/c/09_unsafe_handler_bug.c SAN=tsan
WARNING: ThreadSanitizer: signal-unsafe call inside of a signal
    #0 malloc ...   #1 on_prof .../09_unsafe_handler_bug.c:55
WARNING: ThreadSanitizer: signal handler spoils errno
```

The safe patterns are all in this lesson: set a flag (`c/03`), write a
byte to a self-pipe (`c/05`), use `signalfd` (`c/05`, `c/08`) or a
`sigwait` thread (`c/04`, `cpp/02`).

### 3.5 EINTR and restarting (`c/03_eintr_and_sa_restart.c`)

When a handler runs while the thread sleeps in a slow syscall, the kernel
aborts the syscall to run it. Afterwards:

```
  read() sleeping ......  SIGALRM -> handler runs
     without SA_RESTART:  read() returns -1, errno = EINTR
     with    SA_RESTART:  the kernel re-enters read() by itself
  poll/epoll_wait/select/nanosleep/sigsuspend: ALWAYS -1/EINTR, SA_RESTART or not
```

So every blocking call in real code sits in a retry loop:

```c
while ((n = read(fd, buf, len)) == -1 && errno == EINTR)
    ;                                   /* glibc: TEMP_FAILURE_RETRY(read(...)) */
```

For sleeps, `nanosleep` hands back the remaining time; for exact
deadlines use `clock_nanosleep(..., TIMER_ABSTIME, ...)` so retries do not
drift.

### 3.6 Signals and threads (`c/04_signals_and_threads.c`, `cpp/02`)

```
   per PROCESS: dispositions (handlers)      per THREAD: the signal mask
   kill(pid, SIG)          -> any ONE thread that does not block SIG
   pthread_kill(t, SIG)    -> thread t
   SIGSEGV/SIGFPE (faults) -> the thread that caused it
```

If any thread leaves `SIGTERM` unblocked, it may receive it, and the
default action kills the whole process mid-operation. The robust design:

1. In `main`, **before creating threads**, block the signals you care
   about with `pthread_sigmask` (threads inherit the mask).
2. One thread waits for them with `sigwaitinfo()` / `sigtimedwait()`, or
   the event loop reads them from a `signalfd`.
3. That thread handles them as ordinary code (locks, logging, malloc all
   allowed).

`c/04` shows process-directed `SIGUSR1` picked up by the signal thread,
and a thread-directed `SIGUSR2` (`pthread_kill`) handled on exactly the
worker it was aimed at. `cpp/02` packages this as a `SignalThread` class
with `std::function` handlers and an RAII `SignalBlocker`.

### 3.7 Signals as file descriptors: self-pipe and `signalfd` (`c/05_self_pipe_and_signalfd.c`)

**Self-pipe trick** (portable, used by libuv, Qt, old GLib):

```
  handler: write(pipe[1], &signo, 1)      <- async-signal-safe, O_NONBLOCK
  loop:    poll({pipe[0], ...}) -> read(pipe[0]) -> handle signo normally
```

The byte stays in the pipe even if the signal arrives just before `poll`
starts sleeping, so no wake-up is lost (a plain flag would be).

**signalfd** (Linux): block the signals, then

```c
sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
read(sfd, &si, sizeof si);   /* struct signalfd_siginfo: ssi_signo, ssi_pid,
                                ssi_status (SIGCHLD), ssi_int (sigqueue value) */
```

No handler exists at all, so there is nothing async to get wrong. Two
notes: `signalfd` does not reap children (still `waitpid`), and the
signals must stay blocked in **all** threads or another thread may take
them first.

### 3.8 eventfd (`c/06_eventfd.c`, `cpp/02`)

One `uint64_t` counter in the kernel behind one fd:

```
   counter mode:       write(+3) write(+4) -> [7];  read -> 7, counter = 0
   EFD_SEMAPHORE:      write(+3) -> [3];  read -> 1 (x3), then blocks/EAGAIN
   readable while counter > 0; writes saturate at 2^64-2 (then block/EAGAIN)
```

- **Wake-up between threads:** a worker posts work and writes 1; the loop
  thread's `poll`/`epoll` wakes. Many posts before the loop runs = one
  wake-up, one read returning the sum (in the run: 600 posts in ~100–200
  wake-ups). That is the "post a task to the loop" mechanism in `cpp/01`.
- **Parent ↔ child:** create it before `fork()`; both share the counter.
  Use one eventfd per direction.
- **Counting semaphore with an fd:** `EFD_SEMAPHORE`, used by the thread
  pool in `cpp/02`.
- **Kernel ↔ user:** KVM `irqfd`/`ioeventfd`, VFIO interrupts, `io_uring`
  completion notification and many drivers signal user space through an
  eventfd.

Compared with a pipe: 1 fd instead of 2, no buffer that can fill with
wake-up bytes, 8 bytes per operation.

### 3.9 timerfd (`c/07_timerfd_and_epoll_modes.c`)

```c
tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
timerfd_settime(tfd, 0, &(struct itimerspec){ .it_value = 10ms, .it_interval = 10ms }, NULL);
read(tfd, &expirations, 8);   /* how many periods elapsed since the last read */
```

Missed ticks are **counted**, not lost (sleep 55 ms on a 10 ms timer and
`read` says 5). Compared with `setitimer`/`SIGALRM`: any number of
timers, no signal handler, works in an epoll loop. `CLOCK_MONOTONIC` does
not jump when the wall clock is set (NTP); `TFD_TIMER_ABSTIME` gives
drift-free absolute deadlines.

### 3.10 select vs poll vs epoll (`c/07`)

| | `select` | `poll` | `epoll` |
|---|---|---|---|
| Interface | three fd bitmaps, rebuilt every call | array of `{fd, events, revents}` | kernel object: `epoll_ctl` once, `epoll_wait` many times |
| Max fds | `FD_SETSIZE` = 1024 (fd *numbers*!) | unlimited | unlimited |
| Cost per wait | O(highest fd) | O(n) | O(ready) |
| Edge-triggered | no | no | `EPOLLET` |
| Portable | POSIX | POSIX | Linux (BSD: kqueue) |

Level- vs edge-triggered, as the program shows with 10 bytes and 1-byte
reads:

```
  level (default):  wait->ready read 1 | wait->ready read 1 | ... 10 reports
  edge (EPOLLET) :  wait->ready read 1 | wait->NOTHING (9 bytes stranded!)
                    new data -> ready again -> read until EAGAIN
  EPOLLONESHOT   :  one report, then silent until epoll_ctl(MOD) re-arms
```

Rules: with `EPOLLET` use non-blocking fds and always read/write until
`EAGAIN`. `EPOLLONESHOT` lets several threads share one epoll set safely
(the thread that got the event owns the fd until it re-arms).
`EPOLLEXCLUSIVE` (Linux 4.5) wakes only one of several epoll instances
waiting on the same fd: avoids a "thundering herd" on a shared listening
socket. Regular files cannot be added to epoll (`EPERM`): they are always
"ready"; use `io_uring` for real async file I/O.

### 3.11 An epoll event loop (`c/08_epoll_event_loop.c`, `cpp/01_event_loop.cpp`)

```
   setup:  block SIGTERM/SIGINT; create signalfd, eventfd, timerfd, pipe;
           epoll_ctl(ADD) each with data.u32 = which source
   loop:   while (running)
               n = epoll_wait(ep, ev, 8, 2000 /* watchdog */)
               for each event: switch (source)
                   TIMER  -> read expirations; write a line to the pipe;
                             eventfd += 1; after 5 ticks kill(getpid(), SIGTERM)
                   PIPE   -> read until EAGAIN, split lines
                   EVENT  -> read counter
                   SIGNAL -> read signalfd_siginfo; SIGTERM -> running = 0
   shutdown: disarm timer, DRAIN pipe and eventfd, close everything
```

The final checks prove the shutdown is clean: every line written to the
pipe and every eventfd increment was consumed, even those still queued
when `SIGTERM` arrived. `cpp/01` wraps this in an `EventLoop` class:
`add_fd`, `add_timer`, `add_signals`, `remove_fd` (callbacks may remove
themselves) and a thread-safe `post(std::function)` built from a mutex +
deque + eventfd.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `kill` / `sigqueue` / `raise` | O(1) syscall | set a bit / append to a queue |
| Signal delivery to a handler | ~1–3 µs | kernel builds a signal frame on the user stack, `sigreturn` afterwards |
| `sigprocmask` / `pthread_sigmask` | O(1) syscall | per-thread mask |
| `read` on `signalfd`, `eventfd`, `timerfd` | O(1) syscall | 8 bytes or one `signalfd_siginfo` (128 bytes) |
| `eventfd` write (wake-up) | O(1) | add + wake waiters |
| `select` | O(max fd) per call | scans the bitmaps |
| `poll` | O(n) per call | copies + scans the whole array |
| `epoll_ctl` | O(log n) | interest list is a red-black tree |
| `epoll_wait` | O(ready) | the kernel keeps a ready list filled by callbacks |
| Memory | `eventfd`/`timerfd`: one small kernel object each; `epoll`: ~ 160 bytes per watched fd | |

---

## 5. C vs C++

| Concern | C (this lesson) | C++ (this lesson) |
|---|---|---|
| Owning fds | `close()` on every path | `Fd` move-only class |
| Event dispatch | `switch (ev.data.u32)` | `std::unordered_map<int, std::shared_ptr<std::function<...>>>` |
| Cross-thread wake-up | `write(eventfd, 1)` | `EventLoop::post(std::function<void()>)` (mutex + deque + eventfd) |
| Signal mask scope | `pthread_sigmask` + remember to restore | `SignalBlocker` RAII guard |
| Signal handling thread | `pthread_create` + `sigwaitinfo` loop | `SignalThread` with a `std::map<int, Handler>` |
| Semaphore | `eventfd(EFD_SEMAPHORE)` | `EventFdSemaphore` class (or C++20 `std::counting_semaphore`) |
| Shared counters | `_Atomic` / `volatile sig_atomic_t` | `std::atomic` |

The C++17 standard library has no event loop, no signal API beyond
`std::signal`/`std::raise` (with the same async-safety rules) and no
`epoll` wrapper. Real projects use Boost.Asio (`io_context`,
`signal_set`, `steady_timer`, `post`), libuv, libevent, Qt's event loop or
GLib's `GMainLoop`. All of them are built on exactly these syscalls.
Never call `std::cout` or allocate in a C++ signal handler, and remember
that a `std::function` call may allocate.

---

## 6. Where it is used in the real world

- **GLib main loop / GStreamer.** `GMainContext` polls fds (`GSource`s);
  `g_unix_signal_add()` uses a self-pipe/`signalfd`-style wake-up;
  `g_main_context_wakeup()` writes to an eventfd (GLib's `GWakeup`). A
  GStreamer bus watch (`gst_bus_add_watch`) is a `GSource` on such a loop:
  see [gstreamer/03-bus-and-messages](../../gstreamer/03-bus-and-messages/README.md).
  GStreamer's `GstPoll` uses a control socket/eventfd to wake streaming
  threads; see also [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md).
- **systemd.** `sd-event` is an epoll loop over signalfd, timerfd, inotify,
  child pidfds and sockets. systemd stops a service with `SIGTERM`, then
  `SIGKILL` after `TimeoutStopSec`; `SIGHUP` means reload for many daemons
  (`ExecReload=kill -HUP $MAINPID`).
- **libuv (Node.js), nginx, Redis, HAProxy.** One epoll loop per thread or
  worker process; `uv_async_send` is an eventfd write; nginx workers use
  `EPOLLEXCLUSIVE` on the shared listening socket.
- **Linux kernel.** A driver supports `poll()`/`epoll` by implementing the
  `.poll` file operation: call `poll_wait(file, &wq, pt)` on its wait queue
  and return `EPOLLIN` when data is ready; the IRQ handler calls
  `wake_up_interruptible(&wq)`. See
  [linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md).
  KVM and VFIO use eventfds to inject and report interrupts; `io_uring`
  can notify completions through an eventfd.
- **Embedded/RTOS.** FreeRTOS has no signals: the equivalent of "event
  from an ISR" is `xTaskNotifyFromISR` / `xSemaphoreGiveFromISR`, the
  equivalent of `epoll` is waiting on a **queue set** (`xQueueSelectFromSet`)
  or an **event group**. The ISR rules ("only `...FromISR` functions, no
  blocking") are the RTOS form of async-signal-safety. On the TDA4VM,
  messages from the R5F/C7x cores arrive on `/dev/rpmsg*` fds that you add
  to the same epoll loop.
- **Shells and terminals.** `Ctrl-C` = `SIGINT` to the foreground process
  group, `Ctrl-Z` = `SIGTSTP`, window resize = `SIGWINCH`, closing the
  terminal = `SIGHUP`.

---

## 7. Common mistakes and pitfalls

1. **Unsafe calls in a handler** (`printf`, `malloc`, mutexes, C++
   streams). Deadlocks or corrupts data, rarely and unreproducibly. Check
   against `signal-safety(7)`; run the program under TSan.
2. **Not saving `errno`** in a handler that calls `write()`.
3. **Using `signal()`** instead of `sigaction()`: unclear `SA_RESTART`
   and reset-to-default semantics across systems.
4. **Counting signals.** Standard signals coalesce. Count with real-time
   signals, or better, with an eventfd or a pipe.
5. **`SIGCHLD` with one `waitpid`.** Reap in a `WNOHANG` loop.
6. **Forgetting `EINTR`.** Every blocking call needs a retry loop;
   `SA_RESTART` does not cover `poll`, `epoll_wait`, `select`,
   `nanosleep`.
7. **Signal flag + `pause()`** = lost wake-up race. Use `sigsuspend`,
   the self-pipe, or `signalfd`.
8. **Threads created before blocking signals.** They keep an unblocked
   mask and may receive `SIGTERM` (default action: the whole process
   dies). Block first, then create threads.
9. **`signalfd` without blocking** the signals: the default action or a
   handler wins the race and the fd never becomes readable.
10. **Edge-triggered epoll without reading to `EAGAIN`.** Data is stranded
    and the fd never reports again.
11. **`select` with fd ≥ 1024.** `FD_SET` writes past the bitmap: memory
    corruption. Use `poll`/`epoll`.
12. **Closing an fd without `EPOLL_CTL_DEL`** while it is `dup()`ed:
    epoll tracks the open file description, so events may keep coming for
    an fd number that is now something else.
13. **ThreadSanitizer changes signal timing.** TSan defers async handlers
    to its next safe point: a handler that should run while the thread is
    blocked in `read()` with `SA_RESTART` only runs after `read()`
    returns, and queued real-time signals can be merged. `c/02` and `c/03`
    are written so their checks hold either way. Run them with
    `make run P=linux-internals/13-signals-eventfd-and-epoll/c/04_signals_and_threads.c SAN=tsan`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_signal_basics.c](c/01_signal_basics.c) | Default actions table, `sigaction` + `SA_SIGINFO`, `raise`/`kill`/`sigqueue`, block + `sigsuspend`, `SIG_IGN`, stop/continue, `SIGKILL` | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/01_signal_basics.c` |
| [c/02_rt_signals_queue.c](c/02_rt_signals_queue.c) | Standard signals coalesce, real-time signals queue with values, delivery order; `sigtimedwait` and handlers with `sa_mask` | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/02_rt_signals_queue.c` |
| [c/03_eintr_and_sa_restart.c](c/03_eintr_and_sa_restart.c) | `EINTR` without `SA_RESTART`, the retry loop, transparent restart, `poll`/`nanosleep` never restarted | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/03_eintr_and_sa_restart.c` |
| [c/04_signals_and_threads.c](c/04_signals_and_threads.c) | Block before `pthread_create`, dedicated `sigwaitinfo` thread, `pthread_kill` to one thread | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/04_signals_and_threads.c` |
| [c/05_self_pipe_and_signalfd.c](c/05_self_pipe_and_signalfd.c) | Self-pipe trick with `poll`, `SIGCHLD` reaping loop, `signalfd` with `ssi_pid`/`ssi_status`/`ssi_int` | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/05_self_pipe_and_signalfd.c` |
| [c/06_eventfd.c](c/06_eventfd.c) | Counter and `EFD_SEMAPHORE` semantics, overflow, parent↔child across fork, 3 producer threads → 1 consumer | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/06_eventfd.c` |
| [c/07_timerfd_and_epoll_modes.c](c/07_timerfd_and_epoll_modes.c) | timerfd expiration counts, `select`/`poll`/`epoll` side by side, LT vs ET vs `EPOLLONESHOT`, `EPERM` for regular files | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/07_timerfd_and_epoll_modes.c` |
| [c/08_epoll_event_loop.c](c/08_epoll_event_loop.c) | Event loop over signalfd + eventfd + timerfd + pipe, `SIGTERM` to itself, clean drain on shutdown | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/08_epoll_event_loop.c` |
| [c/09_unsafe_handler_bug.c](c/09_unsafe_handler_bug.c) | **Bug:** `malloc`/`fprintf` in a handler; TSan reports it, plain runs may deadlock (watchdog kills them) | `make run P=linux-internals/13-signals-eventfd-and-epoll/c/09_unsafe_handler_bug.c SAN=tsan` |
| [cpp/01_event_loop.cpp](cpp/01_event_loop.cpp) | RAII `EventLoop`: `add_fd`/`add_timer`/`add_signals`/`remove_fd`, thread-safe `post()`, self-removing callback | `make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/01_event_loop.cpp` |
| [cpp/02_signal_thread_and_eventfd.cpp](cpp/02_signal_thread_and_eventfd.cpp) | `SignalBlocker` RAII mask, `SignalThread` with `std::function` handlers, eventfd semaphore thread pool | `make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/02_signal_thread_and_eventfd.cpp` |

All non-bug programs are also clean under ThreadSanitizer, e.g.
`make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/01_event_loop.cpp SAN=tsan`.
While a program runs you can inspect it with `grep Sig /proc/<pid>/status`
(pending/blocked/ignored/caught masks) and `ls -l /proc/<pid>/fd`
(`anon_inode:[eventfd]`, `[signalfd]`, `[timerfd]`, `[eventpoll]`).

---

## 9. Exercises

**Easy**
1. In `c/01`, add `SA_RESETHAND` to the `SIGUSR1` handler and raise it
   twice. What happens on the second `raise`, and why?
2. In `c/02`, send 5 x `SIGUSR2` and 5 x `SIGRTMIN+3` and predict the
   order before running.
3. Run `kill -l` and `grep Sig /proc/self/status`. Decode `SigBlk` of a
   running `c/08` (add a `sleep(30)` first) into signal names.

**Medium**
4. Fix `c/09_unsafe_handler_bug.c` in three ways: (a) flag + main loop
   prints, (b) self-pipe, (c) `signalfd`. Confirm with `SAN=tsan` that the
   reports are gone.
5. Add a `SIGHUP` "reload" to `c/08` that re-arms the timer with a new
   period, and a `SIGINT` that prints statistics but does not stop.
6. Replace the `poll()` server in
   [12-ipc-unix-sockets/c/06](../12-ipc-unix-sockets/c/06_rpc_server_poll.c)
   with `epoll` in edge-triggered mode. What must change in the read path?
7. Implement `sleep_ms()` that survives signals with
   `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)` and show that it
   does not drift when a periodic `SIGALRM` interrupts it.

**Hard**
8. Add `add_child(pid_t, callback)` to `cpp/01` using `pidfd_open()`
   (Linux 5.3+): the pidfd becomes readable when the child exits — no
   `SIGCHLD` needed.
9. Make the `cpp/01` loop multi-threaded: N threads on one epoll fd with
   `EPOLLONESHOT`, re-armed after each callback. Prove with TSan that no
   fd is handled by two threads at once.
10. Write a small kernel module (see
    [linux-device-drivers](../../linux-device-drivers/03-kernel-synchronization/README.md))
    with a misc device whose `.poll` reports readable when a timer in the
    kernel fires, and add that device to the `c/08` loop on the
    BeagleBone AI-64.

---

## 10. Remember forever

```
signal = number (+ 1 int via sigqueue). disposition per PROCESS, mask per THREAD.
SIGKILL/SIGSTOP: never caught. Standard signals COALESCE; RT signals QUEUE (FIFO, value).
sigaction(SA_SIGINFO|SA_RESTART, sa_mask). Wait race-free: block, then sigsuspend.
Handler: only async-signal-safe calls (write, _exit, sem_post...), volatile sig_atomic_t,
         save errno. NO printf/malloc/mutex.   -> TSan: "signal-unsafe call"
EINTR: retry loops. SA_RESTART doesn't cover poll/epoll_wait/select/nanosleep.
Threads: block in main BEFORE pthread_create; one sigwait thread or a signalfd.
eventfd: write adds, read returns+resets (SEMAPHORE: -1). timerfd: read = #expirations.
epoll: ctl once, wait many, O(ready). LT = while ready; ET = on change -> read to EAGAIN.
Loop: signalfd + eventfd + timerfd + sockets in ONE epoll; shutdown = drain, then close.
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What three dispositions can a signal have, and which two signals ignore
   your choice?
2. You send `SIGUSR1` five times to a process that has it blocked. How
   many times does the handler run after unblocking? And for `SIGRTMIN`?
3. Why is `if (!got_signal) pause();` buggy, and how does `sigsuspend`
   fix it?
4. Explain how calling `malloc` in a handler can deadlock a
   single-threaded program.
5. Which variable types may a handler and the main program share?
6. What is the difference between a syscall interrupted with and without
   `SA_RESTART`? Name two calls that `SA_RESTART` never restarts.
7. In a multi-threaded program, which thread runs the handler for
   `kill(getpid(), SIGTERM)`? How do you make that deterministic?
8. Why is a self-pipe free of the lost-wake-up race that a flag has?
9. What does `read()` return on an eventfd after writes of 2, 3 and 5? And
   with `EFD_SEMAPHORE`?
10. A 10 ms timerfd is not read for 1 s. What does the next `read` return?
11. Compare `select`, `poll` and `epoll` in cost per call and limits.
12. With `EPOLLET`, you read 100 of 4096 available bytes and call
    `epoll_wait` again. What happens and how do you write the read loop
    correctly?
13. How does a Linux driver make its device usable with `epoll`?
14. What is the FreeRTOS counterpart of a signal handler's restrictions,
    and of `epoll`?
