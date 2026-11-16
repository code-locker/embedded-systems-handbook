# 09 — IPC overview, pipes and FIFOs

> Threads share an address space; **processes do not**. Every byte that goes
> from one process to another passes through something the kernel provides:
> a pipe, a queue, a socket, a shared page. This lesson maps all of those
> mechanisms first (so lessons 10–13 have a place to hang on), then digs
> into the oldest and simplest one: the **pipe**, and its named cousin, the
> **FIFO**.

---

## 1. Why this exists

A process is a fortress: its own virtual memory, its own file descriptor
table, its own crash domain. That isolation is *why* we use processes (a bug
in the network daemon cannot scribble over the camera pipeline's memory), but
it means that cooperation needs an explicit channel. On an embedded Linux
board such as the BeagleBone AI-64 you meet all of these every day:

- `journalctl | grep error`: a pipe between two programs.
- A sensor daemon feeding a control process with readings: message queue.
- A camera process handing 8 MB frames to an encoder or AI process on the
  C7x: shared memory or DMA-BUF, because copying 30 such frames a second is
  too expensive.
- `systemd` talking to services, `NetworkManager`, `bluetoothd`: D-Bus over
  Unix sockets.
- "Reload your config": a signal (`SIGHUP`).

### 1.1 The IPC map: every Linux mechanism on one page

| Mechanism | Lesson | Between | Carries | Shape | Copies per transfer | Lifetime (persistence) | Notes |
|---|---|---|---|---|---|---|---|
| **Pipe** `pipe()` | 09 | related (fd inherited via `fork`) | data | byte stream, one-way | 2 (user→kernel→user) | process: gone when all fds close | simplest; shells use it for `a \| b` |
| **FIFO** `mkfifo()` | 09 | unrelated, by path | data | byte stream, one-way | 2 | name: filesystem; data: process | a pipe with a name in the filesystem |
| **POSIX message queue** `mq_open()` | 10 | unrelated, by name `/q` | data | messages + priority | 2 | kernel (until `mq_unlink` / reboot) | pollable fd on Linux, `mq_notify` |
| **System V message queue** `msgget()` | 10 | unrelated, by key | data | messages + type | 2 | kernel (until `IPC_RMID` / reboot) | receive by type; old API |
| **POSIX shared memory** `shm_open()`+`mmap()` | 11 | unrelated, by name | data | raw memory | **0** (after setup) | kernel (tmpfs `/dev/shm`) | needs your own synchronisation |
| **System V shared memory** `shmget()` | 11 | unrelated, by key | data | raw memory | 0 | kernel (until `IPC_RMID` + last detach) | old API (X11 MIT-SHM) |
| **`mmap` of a file** `MAP_SHARED` | 11 | unrelated, by path | data | raw memory | 0 | filesystem (survives reboot) | page cache is the shared memory |
| **`memfd_create`** | 11 | via fd passing / fork | data | raw memory | 0 | process (last fd/mapping) | sealing; no name to leak |
| **Semaphores** POSIX `sem_open`, SysV `semget` | 05 | named or in shm | notification (counts) | — | — | kernel (named) / memory (unnamed) | wake-up + counting, no payload |
| **Unix domain socket** `AF_UNIX` | 12 | unrelated, by path or abstract name | data **and fds** | stream, datagram or seqpacket | 2 | process (path file stays) | `SCM_RIGHTS` fd passing, peer credentials; D-Bus, Wayland, X11 |
| **TCP/UDP on loopback** | 12 (compare) | unrelated, even other machines | data | stream / datagram | 2 + protocol stack | process | portable, slower than `AF_UNIX` |
| **Signals** `kill()`, `sigqueue()` | 13 | any process you may signal | notification (+1 int) | — | — | process | async, async-signal-safety rules |
| **`signalfd`** | 13 | — | signals as readable fd | records | 1 | process | turns signals into `read()`/epoll |
| **`eventfd`** | 13 | related (fd) | notification (64-bit counter) | counter | ~0 | process | cheap wake-up, epoll-able |
| **futex** `futex()` | 06 | threads, or processes on shared memory | notification (wait/wake on an int) | — | 0 | memory | the building block under every pthread lock |
| **D-Bus** (on Unix sockets) | — | system/session bus | data + method calls + signals | messages | ≥2 + daemon | daemon | systemd, NetworkManager, BlueZ |
| **Android binder** | — | Android services | data + objects + fds | transactions | **1** (mapped receive buffer) | kernel driver | Android's everything-IPC; in mainline Linux |

Columns explained:

- **Related vs unrelated**: *related* processes share an ancestor that
  created the channel before `fork()` (the fds are inherited). *Unrelated*
  processes need a **rendezvous name**: a path, a `/name`, a key.
- **Data vs notification**: does it carry bytes, or just "something
  happened"? Shared memory carries data but no notification. You pair it with
  a futex, semaphore, eventfd or condvar. Signals and eventfd notify but
  carry (almost) no data.
- **Byte stream vs message**: a *stream* forgets write boundaries. Two
  `write(…, 100)` calls may arrive as one `read()` of 200 bytes, or as 37 + 163.
  A *message* channel delivers exactly what each send sent.
- **Copies**: `write()` copies user→kernel, `read()` copies kernel→user.
  Shared memory copies nothing after setup. That is why it wins for video.
- **Persistence**: *process*: gone when the last process closes it.
  *Kernel*: survives until explicitly removed or reboot (leaks if you
  forget!). *Filesystem*: survives reboots.

### 1.2 Which one should I pick?

```
Do the two sides share a parent that can set things up before fork()?
 ├─ yes, and it is a stream of bytes / text ................. pipe  (09)
 └─ no: they find each other by a NAME
Is it bulk data (frames, audio, tensors), MB/s and up?
 ├─ yes ... shared memory (11) + a notification (futex/semaphore/eventfd)
 │          or a lock-free ring in it; across devices: DMA-BUF
 └─ no
Do you need request/reply, many clients, fd passing, or to know WHO the peer is?
 ├─ yes .................................... Unix domain socket (12)
 └─ no
Discrete messages, maybe with priorities, fire-and-forget? .... POSIX mq (10)
Just "wake up" / "something changed"? ......... eventfd / signal (13)
Waiting on several channels at once? .......... epoll over the fds (13)
Must also work across machines? ................. TCP/UDP (or a broker)
Desktop / system service API? ................... D-Bus (on Unix sockets)
```

Rules of thumb:

1. **Start with a Unix socket or a pipe.** They have clear EOF and
   peer-death semantics and are easy to debug (`strace`, `socat`).
2. **Shared memory is an optimisation.** Use it when profiling shows the
   copy costs too much. You then own synchronisation, crash recovery
   (lesson 11, robust mutexes) and versioning.
3. **Prefer fd-based mechanisms.** Everything that is a file descriptor
   (pipe, socket, mqd on Linux, eventfd, signalfd, timerfd) can wait together in one
   `epoll` loop (lesson 13).
4. **Mind persistence.** Kernel-persistent objects (`mq_open`, `shm_open`,
   SysV) outlive crashes. Name them uniquely and clean them up.

---

## 2. Mental model

**A pipe is a pneumatic tube between two rooms.** One room puts things in,
the other takes them out in the same order. The tube holds a limited amount
(64 KiB). If it is full, the sender waits. If it is empty, the receiver waits.
When the sender walls up its end, the receiver eventually finds the tube
empty *and sealed*: that is **EOF**. A **FIFO** is the same tube with a
**room number painted on the door** (a path), so strangers can find it.

```
             process A                    kernel                     process B
       +------------------+     +-------------------------+     +------------------+
       | fd 4 (write end) | --> | pipe buffer (ring of    | --> | fd 3 (read end)  |
       +------------------+     | page refs, 16 x 4 KiB)  |     +------------------+
                                +-------------------------+
         write(): copy #1 into the kernel     read(): copy #2 out of the kernel

       read() returns: >0 bytes | 0 = EOF (no write end open ANYWHERE) | -1 EAGAIN (non-blocking, empty)
       write() to a pipe with no read end: SIGPIPE (or EPIPE if ignored)
```

The pipe object itself is a ring buffer in the kernel (`struct
pipe_inode_info`, `fs/pipe.c`). Each slot is a `pipe_buffer` pointing to a
page. That page-reference design is what makes `splice()` zero-copy (§3.8).

---

## 3. How it works

### 3.1 `pipe()` + `fork()`  (`c/01_pipe_fork.c`)

```
  pipe(fd)            fork()                         close unused ends
  --------            ------                         -----------------
  fd[0] read          parent: fd[0] fd[1]            parent: fd[0]      (reader)
  fd[1] write         child : fd[0] fd[1]            child :       fd[1] (writer)
```

`fork()` duplicates the fd table, so **both** processes hold **both** ends.
Each side closes the end it does not use, right after `fork()`. The pipe is a
byte stream: the parent may get the child's five lines in one `read()` or in
five. Framing (newlines, length prefixes, fixed-size records) is the
application's job. Use `pipe2(fd, O_CLOEXEC)` so the ends are not leaked into
programs you `exec`. Plain `pipe()` + `fcntl(FD_CLOEXEC)` leaves a window in
which another thread may fork+exec.

### 3.2 EOF and closing unused ends  (`c/02_pipe_eof_bug.c`)

`read()` returns 0 **only when no write end is open in any process**. If
the reading parent forgets to close its own copy of `fd[1]`, the kernel
cannot know that nobody will write any more. The parent itself still could.
So `read()` blocks forever after the child exits. The `_bug` program shows
this hang. `timeout 3 ./02_pipe_eof_bug; echo $?` prints `124`, and
`ls -l /proc/<pid>/fd` shows both pipe ends still open. The same bug hides
in shell pipelines you build by hand (§3.3), in `popen` implementations, and
in any fd leaked into an exec'd child (which is why `O_CLOEXEC` matters).

### 3.3 `dup2`, pipelines and `popen`  (`c/03_dup2_pipeline_popen.c`)

A program writes to **fd 1** and does not care what fd 1 is. The shell runs
`ls | wc -l` like this:

```
            pipe p                  dup2(p[1], 1)              dup2(p[0], 0)
  [ls]  fd1 =======> [ kernel pipe ] =======> fd0  [wc -l]  fd1 --> terminal
   fork, dup2(p[1],1), close both, exec ls
                       fork, dup2(p[0],0), close both, exec wc
   parent (shell): close both ends, waitpid both
```

`dup2(old, new)` closes `new` and makes it point at the same open file as
`old`. It also clears `FD_CLOEXEC` on `new`, so the exec'd program keeps
it. **Every** process closes every pipe end it does not need. If the shell kept `p[1]`, `wc` would
never finish.

`popen(cmd, "r"|"w")` does the fork + `/bin/sh -c cmd` + one pipe for you
and returns a `FILE*`. Always `pclose()` it, because that reaps the child and
returns its status. Never build `cmd` from untrusted input: it goes through
the shell.

### 3.4 Two pipes for request/reply  (`c/04_bidirectional_pipes.c`)

Pipes are one-way, so a conversation needs two. Wrapping them in `FILE*`
gives `fgets`/`fprintf`, but **stdio buffers**. If the client does not
`fflush()` its request, the request sits in the client's buffer. The server
waits for it, the client waits for the reply, and both block forever. The
same deadlock appears with `popen`-style tools that wait for full output
before reading. (For real bidirectional IPC, a `socketpair()` (lesson 12)
gives one fd per side, full duplex.)

```
            req[1] ---- "ADD 2 3\n" ---->  req[0]
  CLIENT                                           SERVER
            rep[0] <---- "= 5\n" --------  rep[1]
```

### 3.5 `PIPE_BUF` atomicity  (`c/05_pipe_buf_atomicity.c`)

POSIX: a `write()` of **at most `PIPE_BUF` bytes** (4096 on Linux) to a
pipe or FIFO is **atomic**. It is never interleaved with other writers' data.
Larger writes may be split: the kernel copies what fits, sleeps until the
reader makes room, and another writer can get in between.

```
 writer A: write(512 B "AAAA...")      writer B: write(512 B "BBBB...")
 pipe:  ...[AAAA...512...][BBBB...512...][AAAA...]...     never [AABBAA]

 writer A: write(128 KiB)  -> may become  [A x 64K][B x 64K][A x 64K][B ...]
```

The program runs 4 writers × 200 records of 512 B and checks every record
for foreign bytes. Then it does 128 KiB writes and *reports* (does not
assert) whether they interleaved. Use case: many processes logging fixed
records into one FIFO. Each record must be **one** `write()` call. A header
`write()` followed by a body `write()` is two atomic writes, not one.

### 3.6 Capacity, non-blocking I/O, SIGPIPE  (`c/06_capacity_nonblock_sigpipe.c`)

- **Capacity**: default 64 KiB (16 pages). `fcntl(fd, F_GETPIPE_SZ)` reads
  it, `F_SETPIPE_SZ` changes it. The kernel rounds up to a power-of-two
  number of pages. The limit is `/proc/sys/fs/pipe-max-size` (1 MiB) unless you
  have `CAP_SYS_RESOURCE`. Shrinking below the queued data fails with
  `EBUSY`. A bigger pipe absorbs bursts, for example a camera writing faster
  than the consumer for a moment.
- **`O_NONBLOCK`**: write on a full pipe → `EAGAIN`; read on an empty pipe
  with a live writer → `EAGAIN`; read on an empty pipe with no writers → `0`.
  **`EAGAIN` ≠ EOF.**
- **`SIGPIPE`**: writing when no read end exists sends `SIGPIPE`. Its
  default action terminates the process quietly. That is how `yes | head -1`
  ends. Daemons set `SIGPIPE` to `SIG_IGN` and handle `EPIPE` from `write()`.
  On sockets, `send(…, MSG_NOSIGNAL)` does the same per call.

```
   read()  on pipe        writers alive         no writers
   ---------------        -------------         ----------
   data queued            > 0                   > 0  (drain first)
   empty, blocking        sleeps                0 (EOF)
   empty, O_NONBLOCK      -1 / EAGAIN           0 (EOF)

   write() on pipe        readers alive         no readers
   full, blocking         sleeps                SIGPIPE / EPIPE
   full, O_NONBLOCK       -1 / EAGAIN (or short write if > PIPE_BUF)
```

### 3.7 Named pipes (FIFOs)  (`c/07_named_fifo.c`)

`mkfifo(path, 0600)` creates a filesystem entry of type `p`. It is only a
rendezvous: the data still goes through a kernel pipe buffer, never to disk,
and `st_size` stays 0.

```
   shell 1: mkfifo /tmp/x/fifo          shell 2:
            cat /tmp/x/fifo   (blocks in open() ...)
                                         echo hi > /tmp/x/fifo
            hi                <-- open() returned when the writer opened
```

Open semantics, the part everyone forgets:

| open() | no peer yet | peer present |
|---|---|---|
| `O_RDONLY` | **blocks** until a writer opens | returns |
| `O_WRONLY` | **blocks** until a reader opens | returns |
| `O_RDONLY \| O_NONBLOCK` | returns at once (`read` → 0 until a writer appears) | returns |
| `O_WRONLY \| O_NONBLOCK` | **fails, `ENXIO`** | returns |
| `O_RDWR` (Linux only) | returns (it is its own writer, never sees EOF) | returns |

The program forks a child that keeps **only the path** (it closes every
inherited fd), like a separately started program. The parent's blocking
`open()` waits ~200 ms for it. The FIFO stays in the filesystem until
`unlink()`. A crashed program leaves it behind, and the next `mkfifo`
fails with `EEXIST`. Make the name unique and put it in a private
`mkdtemp()` directory.

### 3.8 Zero-copy: `splice`, `tee`, `vmsplice`  (`c/08_splice_tee_vmsplice.c`)

Because a pipe buffer is a list of page references, the kernel can **move
page references instead of bytes**:

```
  read/write :  file --copy--> user buffer --copy--> socket      (2 copies, 2 syscalls/chunk)
  splice     :  file --refs--> pipe --refs--> socket              (0 CPU copies of the data)
  tee        :  pipe A --refs--> pipe B   (A keeps its data: duplicate without consuming)
  vmsplice   :  user pages --refs--> pipe (don't touch the buffer until consumed!)
```

One end of `splice()` must be a pipe. `sendfile()` is built on the same
machinery and is how web servers send files. The program copies a 200 KiB
file through a pipe with `splice`, duplicates a pipe with `tee`, and pushes
user memory with `vmsplice`, checking the bytes every time. GStreamer's `tee`
element and DMA-BUF sharing follow the same idea: pass references, not bytes
(see [gstreamer/08](../../gstreamer/08-queues-threads-tee/README.md)).

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `pipe()` / `pipe2()` | O(1), one syscall | allocates the inode and the ring (buffers lazily) |
| `write(n)` / `read(n)` | O(n) copy + syscall (~1 µs) | data copied user↔kernel; 2 copies end to end |
| Throughput | ~1–5 GB/s on a desktop, less on the A72 | bounded by memcpy and context switches |
| Latency (wake reader) | ~5–20 µs | scheduler wakeup of the blocked reader |
| `PIPE_BUF` write | atomic, O(n) | done under the pipe mutex in one go |
| `splice` / `tee` | O(pages), no data copy | moves/duplicates page references |
| `F_SETPIPE_SZ` | O(slots) | reallocates the ring of `pipe_buffer`s |
| `mkfifo` | one filesystem op | creates an inode only, no data storage |
| Memory | capacity × pages (64 KiB default) | charged to the user (`pipe-user-pages-soft`) |

---

## 5. C vs C++

| Concern | C (this lesson) | C++ (this lesson) | Standard / library equivalent |
|---|---|---|---|
| Owning an fd | `int` + manual `close()` on every path | `Fd`: move-only RAII (`cpp/01`) | none in std (`std::fstream` cannot wrap an fd); Boost.Process, `folly::File` |
| A pipe | `int fd[2]` | `Pipe{Fd read_end, write_end}` | Boost.Process `bp::pipe`, C++26 has nothing yet |
| Errors | `-1` + `errno` | `std::system_error` with `std::errc` | same |
| Running a command | `popen`/`pclose` or fork+exec+dup2 | `ChildProcess` with line reader (`cpp/02`) | Boost.Process, Qt `QProcess`, `posix_spawn` |
| Line framing | `fgets` on a `FILE*` from `fdopen` | own buffer + `std::string::find('\n')` | `std::getline` on a stream (needs a streambuf) |
| Zombies | remember `waitpid` | destructor reaps | — |

Key C++ lesson: `fork()` duplicates objects **without running
constructors**. If the child returns from `main()` instead of calling
`_exit()`, the destructors of the parent's objects run *in the child*. For a
temp file or IPC "owner" object, that deletes the parent's resource.
Children call `_exit()`.

`posix_spawn()` is the modern alternative to fork+exec for "just run
this program". It has file actions (`posix_spawn_file_actions_adddup2`) for
redirection and is cheaper on large processes. glibc implements it with
`clone(CLONE_VM|CLONE_VFORK)`.

---

## 6. Where it is used in the real world

- **Shells**: every `|`. `bash` builds pipelines exactly as in `c/03`, and
  `set -o pipefail` exists because only the last process's status is
  returned by default.
- **Linux kernel**: `fs/pipe.c`, `fs/splice.c`. The kernel's
  `call_usermodehelper` and `core_pattern = |/usr/lib/systemd/systemd-coredump`
  pipe core dumps into a user program.
- **glibc**: `popen`, `system`, `posix_spawn`.
- **Build tools**: `make -j` uses a pipe as a **job-server token pool**.
  Each byte in the pipe is one allowed parallel job, and `PIPE_BUF`
  atomicity keeps the tokens intact.
- **Self-pipe trick**: a signal handler `write()`s one byte into a pipe so
  that a `select`/`poll` loop wakes up. eventfd/signalfd (lesson 13) are the
  modern replacements.
- **GStreamer**: `fdsrc`/`fdsink` read/write any fd (pipe in, pipe out), so
  `gst-launch-1.0 ... ! fdsink | ffplay -` works. `GstPoll` keeps an internal
  wake-up fd (the self-pipe idea) so another thread can interrupt a poll. See
  [gstreamer/08](../../gstreamer/08-queues-threads-tee/README.md) for the
  thread-level equivalent (`queue` = an in-process pipe).
- **systemd**: service stdout/stderr go through a pipe or socket to the
  journal. `systemd-run --pipe` plumbs stdin/stdout through.
- **Logging on embedded boards**: many processes → one FIFO → one log writer
  (fixed records ≤ `PIPE_BUF`), with a logrotate-free ring on flash.
- **RTOS equivalents**: FreeRTOS **stream buffers** and **message buffers**
  (`xStreamBufferSend`/`xStreamBufferReceive`) are the pipe and the
  framed-pipe. Zephyr has `k_pipe`. Like a Linux pipe, a stream buffer is
  single-writer/single-reader safe without extra locking.

---

## 7. Common mistakes and pitfalls

1. **Not closing the unused write end**: the reader never sees EOF and hangs
   (`c/02_pipe_eof_bug.c`). The same happens when an fd is leaked into an
   exec'd child, so use `O_CLOEXEC`.
2. **Treating a pipe as message-oriented**: one `write` ≠ one `read`. Frame
   your data (newline, length prefix, fixed records).
3. **Confusing `EAGAIN` and EOF** in non-blocking loops (`-1/EAGAIN` vs `0`).
4. **Forgetting `fflush`** with `fdopen`/`popen` streams → mutual blocking
   (`c/04`).
5. **Deadlock on both pipes full**: parent writes a big input to the child
   while the child writes a big output back, and neither reads. Read and
   write concurrently (poll, threads) or use a temp file.
6. **Ignoring `SIGPIPE`**, so one disconnected reader kills your daemon. Or
   ignoring it *and* not checking `write()`'s return value.
7. **Records larger than `PIPE_BUF`** from several writers interleave.
8. **`exit()` instead of `_exit()` in a forked child**: stdio buffers are
   flushed twice, `atexit` handlers and C++ destructors run in the wrong
   process. Also `fflush(stdout)` *before* `fork()`.
9. **Not reaping children** leaves zombies. `pclose`, not `fclose`, for
   `popen`.
10. **Stale FIFOs** after a crash: `mkfifo` → `EEXIST`. Use unique names in
    a `mkdtemp` directory, and `unlink` on exit.
11. **Blocking forever in FIFO `open()`** because the peer never comes. Use
    `O_NONBLOCK` for the reader or add a timeout/watchdog.
12. **Using ThreadSanitizer for multi-process bugs.** TSan sees only one
    process. (These programs are still run under TSan,
    `make run P=linux-internals/09-ipc-pipes-and-fifos/c/05_pipe_buf_atomicity.c SAN=tsan`,
    to keep each process clean. Do not `fork()` from a multi-threaded
    process under TSan.)

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_pipe_fork.c](c/01_pipe_fork.c) | pipe + fork, closing unused ends, byte-stream framing, EOF, `pipe2(O_CLOEXEC)` | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/01_pipe_fork.c` |
| [c/02_pipe_eof_bug.c](c/02_pipe_eof_bug.c) | **bug**: forgotten write end → reader hangs; find it with `timeout` + `/proc/<pid>/fd` | `make build/linux-internals/09-ipc-pipes-and-fifos/c/02_pipe_eof_bug && timeout 3 ./build/linux-internals/09-ipc-pipes-and-fifos/c/02_pipe_eof_bug; echo $?` |
| [c/03_dup2_pipeline_popen.c](c/03_dup2_pipeline_popen.c) | `dup2` redirection, hand-built `ls DIR \| wc -l`, `popen` "r" and "w" | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/03_dup2_pipeline_popen.c` |
| [c/04_bidirectional_pipes.c](c/04_bidirectional_pipes.c) | two pipes, request/reply server in a child, the `fflush` deadlock | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/04_bidirectional_pipes.c` |
| [c/05_pipe_buf_atomicity.c](c/05_pipe_buf_atomicity.c) | 4 writers, records ≤ `PIPE_BUF` never interleave; big writes may | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/05_pipe_buf_atomicity.c` |
| [c/06_capacity_nonblock_sigpipe.c](c/06_capacity_nonblock_sigpipe.c) | `F_GETPIPE_SZ`/`F_SETPIPE_SZ`, `EAGAIN` vs EOF, SIGPIPE kills, `EPIPE` when ignored | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/06_capacity_nonblock_sigpipe.c` |
| [c/07_named_fifo.c](c/07_named_fifo.c) | `mkfifo`, blocking/non-blocking open rules, unrelated-process rendezvous, `O_RDWR` trick, `unlink` | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/07_named_fifo.c` |
| [c/08_splice_tee_vmsplice.c](c/08_splice_tee_vmsplice.c) | zero-copy file→pipe→file with `splice`, `tee`, `vmsplice` | `make run P=linux-internals/09-ipc-pipes-and-fifos/c/08_splice_tee_vmsplice.c` |
| [cpp/01_raii_fd_pipe.cpp](cpp/01_raii_fd_pipe.cpp) | move-only `Fd`, `Pipe`, no leaks on exceptions (counted via `/proc/self/fd`), `system_error` | `make run P=linux-internals/09-ipc-pipes-and-fifos/cpp/01_raii_fd_pipe.cpp` |
| [cpp/02_child_process_reader.cpp](cpp/02_child_process_reader.cpp) | `ChildProcess`: fork+execvp+dup2, line reader across chunk boundaries, exit codes, reaping in the destructor | `make run P=linux-internals/09-ipc-pipes-and-fifos/cpp/02_child_process_reader.cpp` |

Every program (except `_bug`) also runs clean under ThreadSanitizer, e.g.
`make run P=linux-internals/09-ipc-pipes-and-fifos/c/01_pipe_fork.c SAN=tsan`.

---

## 9. Exercises

**Easy**
1. Change `c/01` so the child sends binary `int`s instead of text. How does
   the reader know where one ends? (Fixed size: `sizeof(int)`, and loop on
   short reads.)
2. Run `c/02_pipe_eof_bug` under `strace -f` and find the `read()` the
   parent is stuck in. Then add the missing `close()` and see it finish.
3. With `c/06`, measure how `F_SETPIPE_SZ` changes the number of
   context switches when a fast writer meets a slow reader
   (`/usr/bin/time -v`, "voluntary context switches").

**Medium**
4. Build a three-stage pipeline `cat FILE | tr a-z A-Z | wc -c` by hand
   (generalise `c/03` to N stages in a loop). Close every unused end.
   *Hint:* create pipe *i* just before forking stage *i*.
5. Write a mini job server like `make -j4`: a pipe preloaded with 4 token
   bytes. Workers `read` one byte before a job and `write` it back after.
   Prove that at most 4 run at once.
6. Turn `c/04` into a **length-prefixed** binary protocol (4-byte length +
   payload) and make the server handle partial reads.

**Hard**
7. Implement the self-pipe trick: a `SIGCHLD` handler writes a byte to a
   non-blocking pipe, and a `poll()` loop reaps children. Then redo it with
   `signalfd` (lesson 13) and compare.
8. Write `pipe_cat`: copy stdin to stdout using only `splice` (stdin → pipe
   → stdout). Benchmark it against a `read`/`write` loop with a 1 GB file.
   When does `splice` fail with `EINVAL`, and why?
9. Cross-compile `c/05` for the BeagleBone AI-64 (`make arm` style flags,
   or `aarch64-linux-gnu-gcc`), run it on the board, and compare how often
   the big writes interleave on 2 cores versus your PC.

---

## 10. Remember forever

```
pipe(fd): fd[0]=READ, fd[1]=WRITE.  fork -> both have both -> CLOSE what you don't use
read()==0  <=> no write end open anywhere.  -1/EAGAIN = empty but writers alive
no reader + write -> SIGPIPE (ignore it -> EPIPE).   capacity 64 KiB, F_SETPIPE_SZ
write <= PIPE_BUF (4096) is atomic between writers; bigger may interleave
pipe = BYTE STREAM: frame your messages.  stdio over pipes: fflush!
shell a|b: pipe, fork, dup2(p[1],1)/dup2(p[0],0), close all, exec; parent closes both
FIFO: mkfifo; open(RDONLY) waits for writer & vice versa; O_WRONLY|O_NONBLOCK -> ENXIO; unlink
splice/tee/vmsplice: move page refs through a pipe = zero copy
pick: pipe(related) | FIFO/UDS(by name) | mq(messages) | shm(bulk) | eventfd/signal(wakeup)
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name four IPC mechanisms that work between **unrelated** processes and
   say how each one finds its peer.
2. Which IPC mechanisms copy the data twice, and which zero times? Why is
   shared memory not automatically "best"?
3. What does *kernel persistence* mean, and which mechanisms have it? What
   happens to them when your program crashes?
4. A parent forks a child that writes to a pipe and exits. The parent's read
   loop never ends. What is the bug, and which commands prove it?
5. What exactly does `dup2(p[1], 1)` do, and why is `close(p[1])`
   still needed afterwards?
6. Two processes each write 1000-byte records into the same FIFO. Can a
   reader ever see half a record mixed with another? What about 10 000-byte
   records?
7. In non-blocking mode, what are the three possible results of `read()` on
   a pipe, and what does each mean?
8. Why does `yes | head -1` terminate? What would change if `yes` ignored
   `SIGPIPE`?
9. Why does `open(fifo, O_WRONLY | O_NONBLOCK)` fail with `ENXIO`, while
   `O_RDONLY | O_NONBLOCK` succeeds?
10. How can `splice()` avoid copying data, and why must one side be a pipe?
11. Why must a forked child call `_exit()` rather than `exit()` or return
    from `main()` in a C++ program with RAII objects?
12. When would you pick a pipe over a Unix domain socket, and vice versa?
