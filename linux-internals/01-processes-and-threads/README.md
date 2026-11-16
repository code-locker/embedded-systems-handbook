# 01 — Processes and Threads

> A **process** is a running program with its own private address space.
> A **thread** is one flow of control *inside* a process; all threads of a
> process share its memory and files. On Linux both are the same kernel
> object — a *task* — that differ only in what they share. Everything else
> in this track (locks, condition variables, IPC) exists because of this one
> fact: threads share memory, processes do not.

---

## 1. Why this exists

A program that does one thing at a time wastes the machine. The camera
pipeline on the BeagleBone AI-64 has to capture a frame, run inference on
the C7x DSP, draw boxes and stream the result — and it cannot stop capturing
while it encodes. There are two ways to do several things at once:

| Need                                                        | Use        |
|-------------------------------------------------------------|------------|
| Isolation: a crash or bug must not take the others down     | processes  |
| Run a *different program* (shell out, start a daemon)       | `fork` + `exec` |
| Cheap sharing of big data (frames, tensors, caches)         | threads    |
| Use both A72 cores for one computation                      | threads    |
| Privilege separation (root helper + unprivileged worker)    | processes  |

Processes cost more to create and need IPC (lessons 09–13) to talk;
threads are cheap and talk through memory, but then *every* shared variable
is a potential bug (lessons 02–08).

---

## 2. Mental model

**Process = a house. Thread = a person living in it.** Each house has its
own rooms (address space), its own mailbox and keys (file descriptors).
People in the same house share all the rooms, so they can hand each other
things instantly — and they can also bump into each other in the kitchen.
Each person still has their own pocket notebook (stack) and their own train
of thought (registers, program counter).

```
            PROCESS A (pid 100)                      PROCESS B (pid 200)
  +-------------------------------------------+   +------------------------+
  | SHARED by all threads of A:               |   | completely separate:   |
  |   code (.text)  globals (.data/.bss)      |   |   own code, own heap,  |
  |   heap (malloc)  mmap regions             |   |   own fd table, ...    |
  |   fd table, cwd, signal handlers, pid     |   |                        |
  |                                           |   |  +------------------+  |
  |  +-----------+ +-----------+ +---------+  |   |  | thread (tid 200) |  |
  |  | thread    | | thread    | | thread  |  |   |  +------------------+  |
  |  | tid 100   | | tid 101   | | tid 102 |  |   +------------------------+
  |  | PRIVATE:  | | PRIVATE:  | |PRIVATE: |  |
  |  |  stack    | |  stack    | | stack   |  |     talk only through the
  |  |  registers| |  registers| | regs    |  |     kernel: pipes, sockets,
  |  |  TLS      | |  TLS      | | TLS     |  |     shared memory, signals
  |  |  errno    | |  errno    | | errno   |  |
  |  |  sig mask | |  sig mask | | sigmask |  |
  |  +-----------+ +-----------+ +---------+  |
  +-------------------------------------------+
```

| Thing                         | Shared between threads? | Shared between parent/child after `fork`? |
|-------------------------------|:-----------------------:|:-----------------------------------------:|
| Code, globals, heap           | yes                     | **copied** (copy-on-write)                |
| Stack                         | no (one per thread)     | copied                                    |
| Registers, program counter    | no                      | copied, then diverge                      |
| Thread-local storage, `errno` | no                      | copied                                    |
| File descriptor table         | yes                     | copied, but each fd points to the **same** open file description (shared offset) |
| Signal handlers               | yes                     | copied                                    |
| Signal mask                   | no (per thread)         | copied                                    |
| PID                           | yes (same `getpid()`)   | no (child gets a new one)                 |
| TID                           | no (`gettid()` differs) | no                                        |

The one sentence to remember: **threads share everything except their stack,
registers and thread-locals; a forked child shares nothing writable except
open files.**

---

## 3. How it works

### 3.1 `fork`, `exec`, `waitpid`  (`c/01_fork_exec_wait.c`)

```
  parent                                    child
  ------                                    -----
  pid = fork();  ─────── clone ──────────>  pid = fork();   (returns 0)
  (returns child pid)                         |
       |                                      | execlp("sh", ...)
       |                                      |   replaces code+data, keeps
       |                                      |   pid and open fds
       |                                      v
  waitpid(pid, &st, 0)  <──── SIGCHLD ────  exit(7)  -> becomes a ZOMBIE
       |                                              until waited for
  WIFEXITED(st) && WEXITSTATUS(st) == 7
```

- `fork()` returns **twice**: the child's pid in the parent, 0 in the child,
  -1 on failure (no child).
- `exec*()` never returns on success. The new program keeps the pid, the open
  fds (unless `O_CLOEXEC`), the cwd, and the signal *mask*; handlers reset to
  default. That is how a shell sets up `cmd > file`: fork, `dup2` the file
  onto fd 1 in the child, exec.
- `waitpid()` returns a **packed status word**, never the exit code. Decode
  it with `WIFEXITED`/`WEXITSTATUS` (normal exit, 0–255) or
  `WIFSIGNALED`/`WTERMSIG` (killed by a signal). The shell shows a signal
  death as `128 + signo`.

### 3.2 Copy-on-write, shared file descriptions

After `fork()` the child *appears* to have a full copy of the parent's
memory, but the kernel copies nothing up front: both processes' page tables
point at the same physical pages, marked read-only. The first **write** to a
page by either side faults, and the kernel copies just that 4 KiB page for
the writer. So `fork()` of a 1 GiB process is fast, and `fork` + immediate
`exec` copies almost nothing. The program shows the child writing `g_value`
at the *same virtual address* while the parent still reads the old value.

File descriptors are different: the fd *table* is copied, but each entry
points to the same **open file description** — the kernel object that holds
the file offset and flags. Child writes `"child\n"`, parent then writes
`"parent\n"`, and the file contains both lines in order because they share
one offset.

```
  parent fd 3 ──┐
                ├──> open file description { offset=13, O_RDWR } ──> inode
  child  fd 3 ──┘
```

### 3.3 Zombies and orphans

- **Zombie** (`Z` in `ps`, `/proc/<pid>/stat`): the child has exited but the
  parent has not called `wait*()` yet. The kernel keeps a tiny record (pid +
  exit status) so the parent can still ask. A long-running daemon that never
  reaps fills the pid table. Fix: `waitpid()` in a loop, or a `SIGCHLD`
  handler that does `while (waitpid(-1, &st, WNOHANG) > 0);`, or
  `signal(SIGCHLD, SIG_IGN)` if you never care about status.
- **Orphan**: the parent dies first. The child is re-parented to the nearest
  ancestor that called `prctl(PR_SET_CHILD_SUBREAPER)` (systemd, container
  init, our demo) or to pid 1, which reaps it. Daemons double-fork on
  purpose to become orphans owned by init.

### 3.4 Creating and joining threads  (`c/02_pthread_create_join.c`)

```c
pthread_t t;
int rc = pthread_create(&t, NULL /*attr*/, worker, arg);  /* returns errno-style code */
...
void *ret;
pthread_join(t, &ret);   /* wait for it, collect its return value, free its resources */
```

- `pthread_*` functions **return** the error number (0 = OK) and do not set
  `errno`. `if (pthread_create(...) == -1)` is a bug.
- `pthread_join` is the synchronisation point: everything the thread wrote
  before returning is visible to the joiner afterwards. That is why reading
  `tasks[i].sum` after join is safe without any lock.
- A thread that is neither joined nor detached leaks its stack and kernel
  task until the process exits ("zombie thread"). TSan reports it as a
  *thread leak*.
- **Detached** threads clean up after themselves but cannot be joined; you
  need some other way (flag, condvar, future) to know they finished, and they
  must not use anything that may be freed before they end.

### 3.5 Passing arguments and returning results safely

The argument is a `void *`. The thread may run *now* or *in 10 ms* — so the
object it points to must stay alive and unchanged until the thread has read
it:

```
 WRONG                                   RIGHT
 for (int i = 0; i < N; i++)             Task tasks[N];                  /* one each */
     pthread_create(&t[i], 0, f, &i);    for (int i = 0; i < N; i++) {
         \___ all threads get the            tasks[i].id = i;
              SAME address; i changes        pthread_create(&t[i], 0, f, &tasks[i]);
              and then dies                }
                                         /* or pass the value itself:     */
                                         pthread_create(&t, 0, f, (void *)(intptr_t)i);
```

`c/05_loop_var_arg_bug.c` is the wrong version; ASan reports
*stack-use-after-scope* and TSan reports a *data race* on `i`.

Results come back either in the argument struct (read after join) or as a
`malloc`'d pointer returned from the thread function — the joiner then owns
and frees it. **Never return the address of a thread's local variable**:
its stack is gone after the thread ends.

### 3.6 Attributes

`pthread_attr_t` configures a thread *before* it starts:

| Attribute                     | Why you set it                                               |
|-------------------------------|--------------------------------------------------------------|
| `setstacksize`                | default is `ulimit -s` (8 MiB virtual); shrink for many threads on a small board, grow for deep recursion |
| `setdetachstate(DETACHED)`    | fire-and-forget thread                                       |
| `setinheritsched(EXPLICIT)` + `setschedpolicy` + `setschedparam` | start directly as a real-time thread (3.10) |
| `setguardsize`                | size of the no-access page(s) below the stack that turn overflow into SIGSEGV |
| `pthread_attr_setaffinity_np` | start pinned to a CPU set                                    |

`pthread_getattr_np()` reads the real attributes of a running thread.

### 3.7 Thread-local storage  (`c/03_tls_and_cancellation.c`)

```
  static _Thread_local int tls_count;      C11 (thread_local via <threads.h>)
  static __thread int tls_count;           GCC/Clang extension, same thing
  static thread_local int tls_count;       C++11

  thread A ── TPIDR_EL0 ──> [ TLS block A ] tls_count = 1000
  thread B ── TPIDR_EL0 ──> [ TLS block B ] tls_count = 1000
  main     ── TPIDR_EL0 ──> [ TLS block M ] tls_count = 0
```

On arm64 each thread's TLS block is found through the `TPIDR_EL0` system
register (`FS` base on x86-64), which the kernel saves and restores on every
context switch. So a TLS access is about as cheap as a global access and
needs no lock. `errno` is the classic example: it is thread-local, so one
thread's failing `read()` cannot clobber another thread's error.

`pthread_key_create(&key, destructor)` is the older, dynamic form: a
per-thread `void *` slot whose destructor runs at **thread exit** for every
non-NULL value — useful in libraries that allocate per-thread buffers lazily.
C++ `thread_local` objects get real constructors and destructors per thread
(`cpp/02`).

### 3.8 Cancellation and cleanup handlers (brief)

`pthread_cancel(t)` *requests* that `t` stop. With the default **deferred**
type, the thread only acts on it at a *cancellation point* — most blocking
calls (`read`, `write`, `nanosleep`, `pthread_cond_wait`, `accept`, ...;
list in `man 7 pthreads`) and `pthread_testcancel()`. On the way out it runs
the handlers pushed with `pthread_cleanup_push()` in reverse order, then TLS
destructors; `pthread_join` returns `PTHREAD_CANCELED`.

```
  push(free_buf)      ┐   cancellation unwinds:   unlock  (pushed last, runs first)
  push(unlock_mutex)  ┘                           free    (pushed first, runs last)
```

`pthread_setcancelstate(DISABLE)` protects a critical stretch. Asynchronous
cancellation (`PTHREAD_CANCEL_ASYNCHRONOUS`) can strike between any two
instructions — even inside `malloc` — and is almost never safe. In practice,
prefer a cooperative stop flag (an atomic, an `eventfd` — lesson 13, or
C++20 `std::stop_token`); cancellation is mostly for threads blocked in
calls you cannot otherwise interrupt.

### 3.9 How Linux implements threads: `clone()`, tasks, `/proc`

Linux has no separate "thread" object. `fork()` and `pthread_create()` are
both thin wrappers over the **`clone()`** system call, which creates a new
`task_struct` and takes flags saying what to *share* instead of copy:

```
  fork()            ≈ clone(SIGCHLD)                              share nothing
  pthread_create()  ≈ clone(CLONE_VM | CLONE_FS | CLONE_FILES |
                            CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM |
                            CLONE_SETTLS | CLONE_PARENT_SETTID |
                            CLONE_CHILD_CLEARTID, child_stack, ...)
                    = "new task that shares my mm, fd table, handlers, and
                       joins my thread group"

  kernel view                                 user view
  task 4100  tgid 4100  (group leader) ─┐
  task 4101  tgid 4100                  ├──  one process, pid 4100,
  task 4102  tgid 4100                 ─┘    three threads
```

- `getpid()` returns the **thread group id** (tgid) — the same in every
  thread. `gettid()` returns the task's own id; in the main thread it equals
  the pid.
- `/proc/<pid>/task/<tid>/` has one directory per thread (`comm`, `stat`,
  `status`, `sched`). `ps -eLf`, `top -H` and `htop` show them.
  `pthread_setname_np()` writes `comm` (max 15 chars) — name your threads,
  it makes `top -H` on the board readable.
- `CLONE_CHILD_CLEARTID` is how `pthread_join` works: when the thread dies the
  kernel zeroes a word and does a futex wake on it (lesson 06).
- The kernel's own threads (`kthreadd`, `ksoftirqd/0`, `irq/123-...`) are
  tasks too, with no user address space.

### 3.10 Scheduling basics  (`c/04_scheduling_and_affinity.c`)

The scheduler picks among runnable **tasks** per CPU:

```
  SCHED_FIFO  / SCHED_RR   priority 1..99   real-time: highest priority
                                            runnable task ALWAYS runs;
                                            FIFO runs until it blocks/yields,
                                            RR rotates equals every ~100 ms
  ------------------------------------------------------------------------
  SCHED_OTHER / SCHED_BATCH  nice -20..19   fair share (CFS, EEVDF since 6.6):
                                            nice changes the weight, ~10%
                                            CPU per nice step
  SCHED_IDLE                                only when nothing else wants it
  (SCHED_DEADLINE: runtime/deadline/period budget, needs root)
```

- **Nice is per thread on Linux** (POSIX says per process); the demo raises
  one worker's nice and the main thread's stays the same.
- **Real-time needs privilege**: `CAP_SYS_NICE` or `RLIMIT_RTPRIO`
  (`ulimit -r`, `LimitRTPRIO=` in a systemd unit). The demo gets `EPERM`
  as a normal user and skips. A runaway `SCHED_FIFO` loop can lock up a core;
  the kernel's RT throttling (`sched_rt_runtime_us`, 95% by default) is the
  safety net.
- When creating a real-time thread through attributes you **must** set
  `PTHREAD_EXPLICIT_SCHED`, otherwise the policy in `attr` is silently
  ignored and the thread inherits the creator's.
- **CPU affinity** (`pthread_setaffinity_np`, `sched_setaffinity`,
  `taskset`) restricts a thread to a set of CPUs: keep a latency-critical
  thread away from a busy core, keep a cache warm, or follow an IRQ that is
  routed to one core (`/proc/irq/<n>/smp_affinity`). The demo pins a worker
  to the last allowed CPU and checks `sched_getcpu()`.
- On the TDA4VM Linux sees only **two** A72 cores (CPU 0 and 1). The R5F and
  C7x cores are not Linux CPUs — you talk to them through remoteproc/RPMsg,
  not affinity.

---

## 4. Complexity table

| Operation                          | Cost (typical, x86-64 / A72)            | Why |
|------------------------------------|-----------------------------------------|-----|
| `fork()` of a small process        | ~50–200 µs                              | copies page tables (O(mapped pages)), not pages |
| first write to a page after fork   | page fault + 4 KiB copy                 | copy-on-write |
| `fork()` + `exec()`                | ~0.5–1 ms                               | loads a new ELF, dynamic linking |
| `vfork()` / `posix_spawn()`        | much cheaper than fork for huge parents | no page-table copy |
| `pthread_create()` + join          | ~10–30 µs                               | `clone()`, mmap of a stack, TLS setup |
| context switch between threads     | ~1–5 µs                                 | same mm: no page-table switch, no TLB flush |
| context switch between processes   | a bit more                              | address-space switch (ASID avoids a full TLB flush on ARM) |
| TLS variable access                | ~ a global access                       | one register-relative load |
| `pthread_getspecific`              | a few ns                                | array lookup in the thread descriptor |
| Memory per thread                  | 8 MiB virtual stack (default), a few KiB resident | stack pages are faulted in on demand |

---

## 5. C vs C++

| Concept               | C (POSIX)                                 | C++17                                         |
|-----------------------|-------------------------------------------|-----------------------------------------------|
| create                | `pthread_create(&t, attr, fn, arg)`       | `std::thread t(fn, args...)` (args are **copied**) |
| wait                  | `pthread_join(t, &ret)`                   | `t.join()`                                    |
| detach                | `pthread_detach(t)` / attr                | `t.detach()`                                  |
| forgot to join        | leaked thread (silent)                    | `std::terminate()` in `~thread()` — loud      |
| pass by reference     | pass a pointer                            | `std::ref(x)`; move-only args via `std::move` |
| return a value        | struct field or heap pointer              | `std::async` / `std::promise` → `std::future` |
| exception in thread   | n/a                                       | must be caught; `std::exception_ptr` or a future carries it out |
| thread-local          | `_Thread_local`, `__thread`, `pthread_key_t` | `thread_local` (real ctor/dtor per thread) |
| own id                | `pthread_self()`, `gettid()`              | `std::this_thread::get_id()`                  |
| number of CPUs        | `sysconf(_SC_NPROCESSORS_ONLN)`, `CPU_COUNT` | `std::thread::hardware_concurrency()` (may be 0) |
| attributes, priority, affinity, name | `pthread_attr_*`, `pthread_setschedparam`, `pthread_setaffinity_np`, `pthread_setname_np` | not in the standard: use `t.native_handle()` with the pthread calls |
| auto-join + stop      | —                                         | C++20 `std::jthread` + `std::stop_token` (our `JoiningThread` in `cpp/01` is the C++17 stand-in) |
| processes             | `fork`, `exec*`, `waitpid`, `posix_spawn` | nothing in the standard; use the POSIX calls |

`std::async(std::launch::async, f)` returns a future whose **destructor
blocks** until `f` is done, so a discarded future makes the call synchronous.
`launch::deferred` runs `f` lazily inside `get()`, on the caller's thread.

---

## 6. Where it is used in the real world

- **Linux kernel**: `kernel/fork.c` (`copy_process`, `kernel_clone`) is
  where `fork`, `pthread_create` and `kthread_create` all end up. CoW is
  handled in `mm/memory.c` (`do_wp_page`). Zombie reaping is
  `kernel/exit.c` (`wait_consider_task`, `release_task`).
- **glibc / NPTL**: `nptl/pthread_create.c` allocates the stack (cached for
  reuse), sets up TLS and calls `clone3`/`clone`. `posix_spawn` uses
  `clone(CLONE_VM|CLONE_VFORK)` to avoid copying page tables.
- **systemd**: forks + execs every service, is a child subreaper, puts each
  service in a cgroup, and applies `CPUAffinity=`, `Nice=`,
  `CPUSchedulingPolicy=fifo`, `LimitRTPRIO=` from unit files.
- **GStreamer**: every `queue` element starts a *streaming thread*; a
  pipeline is a set of threads handing buffers through queues
  ([gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md)).
  `GstTask` wraps a thread; `GST_DEBUG` prints thread ids so you can map
  log lines to `top -H`.
- **Shells, `make -j`, web servers** (Apache prefork vs worker MPM, nginx
  worker processes): fork for isolation, threads for throughput.
- **Android zygote**: forks a pre-initialised process for every app so CoW
  shares the framework pages.
- **Embedded / RTOS**: FreeRTOS has *only* tasks (no processes, one address
  space): `xTaskCreate(fn, name, stack_words, arg, prio, &handle)` is the
  `pthread_create` + attributes equivalent, task-local storage pointers are
  the `pthread_key` equivalent, and priorities behave like `SCHED_FIFO`. On
  the TDA4VM the R5F cores run FreeRTOS/TI-RTOS tasks this way while Linux
  runs on the A72s.

---

## 7. Common mistakes and pitfalls

1. **Passing `&loop_var`** to `pthread_create` (or capturing `[&]` in a C++
   lambda in a loop). All threads share one variable that keeps changing.
   `c/05_loop_var_arg_bug.c`; caught by ASan (*stack-use-after-scope*) and
   by `make run P=linux-internals/01-processes-and-threads/c/05_loop_var_arg_bug.c SAN=tsan`
   (*data race*).
2. **Returning a pointer to a local** from a thread function. The stack is
   unmapped (or reused) after the thread exits.
3. **Checking `pthread_*` results against -1 or reading `errno`** — they
   return the error code directly.
4. **Forgetting to join**: leaked threads in C, `std::terminate()` in C++.
   Use an RAII joiner / `std::jthread`.
5. **`printf` then `fork` without `fflush`**: the buffered text is printed
   twice when stdout is a pipe or a file (exactly what happens in CI).
6. **`exit()` in a forked child** runs the parent's atexit handlers and
   flushes the parent's stdio buffers again. Use `_exit()` in the child.
7. **Forking a multithreaded process**: the child has only the thread that
   called `fork()`. Any mutex another thread held (including the one inside
   `malloc`!) stays locked forever in the child. After fork in a threaded
   program, call only async-signal-safe functions until `exec`.
8. **Not reaping children** → zombies pile up. **Not closing the write end
   of a pipe** in the parent → `read()` never returns EOF.
9. **Leaking fds into exec'd programs**: open with `O_CLOEXEC` /
   `SOCK_CLOEXEC` / `pipe2(..., O_CLOEXEC)`.
10. **Real-time attributes without `PTHREAD_EXPLICIT_SCHED`**: silently
    ignored. And `SCHED_FIFO` needs privilege — check for `EPERM`.
11. **Busy-looping at `SCHED_FIFO`**: starves everything on that core,
    including the kernel threads your driver needs.
12. **`std::async` result thrown away**: blocks right there.
13. **Asynchronous cancellation** or cancelling a thread that holds a lock
    without a cleanup handler: deadlock later. Prefer stop flags.
14. **ThreadSanitizer and cancellation**: TSan mishandles a mutex unlocked by
    a cleanup handler while cancelling out of an intercepted blocking call
    (false "double lock"). `c/03` therefore cancels at `pthread_testcancel()`.
    TSan also needs ASLR off on recent kernels; the Makefile runs it under
    `setarch -R` for you.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_fork_exec_wait.c](c/01_fork_exec_wait.c) | `fork`/`exec`/`waitpid`, `W*` status macros, CoW, shared fd offset, zombie seen in `/proc`, orphan re-parented to a subreaper | `make run P=linux-internals/01-processes-and-threads/c/01_fork_exec_wait.c` |
| [c/02_pthread_create_join.c](c/02_pthread_create_join.c) | `pthread_create`/`join`, per-thread args, heap return value, private stacks vs shared globals, pid vs tid, `/proc/self/task`, thread names, stack-size attr, detached thread | `make run P=linux-internals/01-processes-and-threads/c/02_pthread_create_join.c` |
| [c/03_tls_and_cancellation.c](c/03_tls_and_cancellation.c) | `_Thread_local`/`__thread`, `pthread_key_create` destructor, `pthread_cancel` with cleanup handlers that free and unlock | `make run P=linux-internals/01-processes-and-threads/c/03_tls_and_cancellation.c` |
| [c/04_scheduling_and_affinity.c](c/04_scheduling_and_affinity.c) | Policies and priority ranges, per-thread nice, `SCHED_FIFO` attempt (skips on `EPERM`), `pthread_setaffinity_np` + `sched_getcpu` | `make run P=linux-internals/01-processes-and-threads/c/04_scheduling_and_affinity.c` |
| [c/05_loop_var_arg_bug.c](c/05_loop_var_arg_bug.c) | **Intentional bug**: `&i` passed to threads. ASan: stack-use-after-scope; TSan: data race | `make run P=linux-internals/01-processes-and-threads/c/05_loop_var_arg_bug.c` (ASan) / `... SAN=tsan` |
| [cpp/01_std_thread.cpp](cpp/01_std_thread.cpp) | `std::thread`, lambda capture by value, `std::ref`/`std::move` args, RAII joiner, `native_handle`, detach, exceptions via `exception_ptr`, `hardware_concurrency` | `make run P=linux-internals/01-processes-and-threads/cpp/01_std_thread.cpp` |
| [cpp/02_async_future_thread_local.cpp](cpp/02_async_future_thread_local.cpp) | `std::async` (async vs deferred), exceptions through futures, `promise`, `packaged_task`, `wait_for` timeout, `thread_local` ctor/dtor per thread | `make run P=linux-internals/01-processes-and-threads/cpp/02_async_future_thread_local.cpp` |

Every program also runs clean under ThreadSanitizer, e.g.
`make run P=linux-internals/01-processes-and-threads/c/02_pthread_create_join.c SAN=tsan`
(or the whole track: `make test-tsan`).

On the board, watch the threads live: run a program in one shell and
`top -H -p $(pidof <prog>)` or `ls /proc/$(pidof <prog>)/task` in another.

---

## 9. Exercises

**Easy**
1. In `c/01`, add a child that is killed by `SIGSEGV` (dereference NULL in
   the child) and decode the status. What does the shell print for it?
2. Change `c/02` to return the result through `pthread_exit(ptr)` instead of
   `return ptr`. Is there any difference?
3. Print `/proc/self/status` lines `Threads:` and `voluntary_ctxt_switches`
   from inside `c/02` while the workers are parked.

**Medium**
4. Write a mini-shell loop that runs a fixed list of commands with
   `fork`+`execvp`, supports `cmd > file` via `dup2`, and prints each exit
   status. Then rewrite the spawn with `posix_spawnp` and compare.
5. Install a `SIGCHLD` handler that reaps with `WNOHANG` in a loop, start
   five children that exit at random times, and prove no zombie remains.
   Why must the handler loop instead of calling `waitpid` once?
6. Measure `pthread_create`+`join` cost vs `fork`+`waitpid` cost over 1000
   iterations on your PC and on the BeagleBone AI-64 (print, don't assert).
7. Fix `c/05_loop_var_arg_bug.c` three different ways and confirm ASan and
   TSan are both silent.

**Hard**
8. Call `clone()` directly (glibc wrapper) with `CLONE_VM | CLONE_FS |
   CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD` and your own `mmap`'d stack.
   Show that `getpid()` is shared and `gettid()` differs. Why can't the
   new task safely call `printf` (hint: TLS was not set up)?
9. Demonstrate pitfall 7: one thread holds a mutex in a loop, another
   thread forks; the child tries to lock the mutex with a 1 s timed lock.
   Then fix it with `pthread_atfork()`.
10. On the board (as root), run two `SCHED_FIFO` busy threads pinned to the
    same CPU at priorities 10 and 20 for 100 ms and count how far each gets.
    Repeat with `SCHED_RR` at equal priorities. Explain the numbers.

---

## 10. Remember forever

```
fork(): returns child pid / 0 in child / -1.  child = CoW copy; fds SHARE offsets
exec(): replaces the program, keeps pid + open fds (unless O_CLOEXEC)
waitpid(pid,&st,0): WIFEXITED/WEXITSTATUS, WIFSIGNALED/WTERMSIG. Unreaped = ZOMBIE
fflush before fork; _exit in the child
threads share: memory, fds, handlers, pid.  private: stack, regs, TLS, errno, sigmask
pthread_* return the error code; join = wait + happens-before + free; else detach
arg must outlive the thread's use of it: never &loop_var; never return &local
Linux: thread = task from clone(CLONE_VM|CLONE_THREAD|...); getpid()=tgid, gettid()=task
SCHED_FIFO/RR 1..99 beat SCHED_OTHER (nice); RT needs CAP_SYS_NICE -> handle EPERM
C++: ~thread() on joinable = terminate; args copied (std::ref); async future blocks in dtor
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. List four things threads of one process share and four things each
   thread has privately.
2. After `fork()`, the child writes to a global. Why does the parent not see
   it, and when exactly is memory physically copied?
3. Parent and child both write to an fd inherited across `fork()`. Why do
   the writes not overwrite each other? What would change if the child had
   called `open()` on the same path itself?
4. What is a zombie, why does the kernel keep it, and what are three ways to
   avoid accumulating them?
5. Why is `pthread_create(&t, NULL, f, &i)` inside a `for` loop wrong? Name
   the two tools that catch it and what each reports.
6. What does `pthread_join` guarantee about memory, besides "the thread has
   finished"?
7. What does `getpid()` return in a non-main thread on Linux? And `gettid()`?
   Where can you see all threads of a process?
8. Which `clone()` flag makes a new task a thread of the same process rather
   than a new process sharing memory?
9. Why might a `SCHED_FIFO` thread created with attributes run as
   `SCHED_OTHER` anyway?
10. What happens if a `std::thread` object is destroyed while still
    joinable? What does C++20 add to fix this?
11. Why is `std::async(std::launch::async, f);` on its own line not
    asynchronous?
12. What is the danger of calling `malloc` in the child after forking a
    multithreaded process?

---

**Next:** [02 — Race conditions and atomics](../02-race-conditions-and-atomics/README.md)
— what goes wrong the moment two of these threads touch the same variable.
Later: [09 — Pipes and FIFOs](../09-ipc-pipes-and-fifos/README.md) builds
on `fork` + `pipe` from `c/01`; the kernel's view of tasks and locking is
in [linux-device-drivers/03-kernel-synchronization](../../linux-device-drivers/03-kernel-synchronization/README.md).
