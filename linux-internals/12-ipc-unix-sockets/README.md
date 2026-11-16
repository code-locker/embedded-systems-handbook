# 12 — Unix Domain Sockets

> The socket API you already know from networking, but with a file name
> instead of an IP address and port, and with the kernel as a witness. Unix
> sockets are the IPC behind systemd, D-Bus, X11, Wayland, Docker and most
> Linux daemons. They give you two-way streams *or* messages, they tell
> you **who** the peer is, and they can hand over open **file descriptors**.
> No other IPC mechanism does both of those last two things.

For the big comparison of all IPC mechanisms ("which one should I pick?")
see [09 — Pipes and FIFOs](../09-ipc-pipes-and-fifos/README.md). This lesson
assumes you know `fork()` and `waitpid()` from
[01 — Processes and threads](../01-processes-and-threads/README.md).

---

## 1. Why this exists

A pipe is one-way, has no name (or a FIFO name with one reader and one
writer), and carries anonymous bytes. Real systems need more:

| Need | Pipe / FIFO | Unix socket |
|---|---|---|
| Two-way conversation | two pipes | one socket |
| Many clients to one server, each with its own connection | no | `listen()` + `accept()` |
| Keep message boundaries | no | `SOCK_DGRAM`, `SOCK_SEQPACKET` |
| Know the peer's pid / uid / gid, vouched for by the kernel | no | `SO_PEERCRED`, `SCM_CREDENTIALS` |
| Send an open fd (a file, a socket, a DMA-BUF, a memfd) | no | `SCM_RIGHTS` |
| Same code could later run over the network | no | change the address family |

A service on the BeagleBone (say, a camera daemon) typically exposes
`/run/camd.sock`: clients connect, the daemon checks their uid with
`SO_PEERCRED`, then answers requests or hands them a DMA-BUF fd of a
camera frame so they can read it **without copying**.

---

## 2. Mental model

**A Unix socket is a telephone exchange inside the kernel.** The server
"rents a number" (bind to a path), "waits for calls" (listen), and "picks
up" (accept), which gives it a *private line* to that caller. Each call is
a separate connected socket. The exchange knows exactly who is calling
(credentials) and can pass physical objects through the line (fds).

```
                      kernel (one machine, no network stack)
   client A                                                    server
  +--------+  connect("/run/x.sock")   +-------------------+  +--------+
  | fd 3 --+-------------------------->| listening socket  |  | fd 3   |
  +--------+                           |  backlog queue    |<-+ listen |
                                       +---------+---------+  |        |
   client B                                      | accept()   |        |
  +--------+                           +---------v---------+  |        |
  | fd 3 --+===========================| connected socket  |--+ fd 4   |  private line A
  +--------+    bytes are copied        +-------------------+  |        |
               straight into the        +-------------------+  |        |
               peer's receive queue  ===| connected socket  |--+ fd 5   |  private line B
                                        +-------------------+  +--------+
```

The one sentence to remember: **a Unix socket is a local socket that
skips TCP/IP, keeps the peer's identity, and can carry file descriptors.**

---

## 3. How it works

### 3.1 Server and client (`c/01_stream_server_client.c`)

```
   server                                  client
   ------                                  ------
   fd = socket(AF_UNIX, SOCK_STREAM, 0)
   bind(fd, "/tmp/uds-demo-XXXX/server.sock")      <- creates an inode, type 's'
   listen(fd, backlog)
                                           c = socket(AF_UNIX, SOCK_STREAM, 0)
                                           connect(c, same path)
   cfd = accept(fd)   <---- new connected socket per client
   read(cfd) ...                           write(c, request)
                                           shutdown(c, SHUT_WR)   <- "I'm done sending"
   read(cfd) == 0 (EOF)
   write(cfd, reply)  ------------------>  read(c) ... until EOF
   close(cfd)
```

- `connect()` to a listening Unix socket completes **immediately** as long
  as the backlog has room. There is no SYN/ACK handshake: the kernel just
  links the two socket structures.
- `shutdown(SHUT_WR)` is a *half-close*: the peer reads EOF, but you can
  still receive the reply. `close()` would end both directions.
- Connecting needs **write permission** on the socket file. That is the
  first layer of access control (Docker's `/var/run/docker.sock` is
  `srw-rw---- root docker`).

### 3.2 Names: filesystem path vs abstract namespace (`c/01`)

```
 filesystem name                         abstract name (Linux only)
 sun_path = "/run/camd.sock\0"            sun_path = "\0camd" (+ exact addrlen)
   - creates an inode (ls -l: srwx...)      - no file, lives in the network
   - permissions = access control             namespace, shown as @camd in
   - stays after close(): YOU unlink()          /proc/net/unix
   - stale file -> bind() EADDRINUSE        - disappears with the last socket
   - max 107 chars + NUL (sizeof 108)       - no permissions: anyone in the
                                              same net namespace can connect
                                              (check SO_PEERCRED!)
```

The program shows the classic pitfall: after `close()` the file is still
there and a restarted server's `bind()` fails with `EADDRINUSE`. Daemons
`unlink()` before `bind()`, or let systemd own the socket (socket
activation). The abstract name needs the **exact** address length:
`offsetof(struct sockaddr_un, sun_path) + 1 + strlen(name)`. Passing
`sizeof(addr)` makes the trailing NULs part of the name, and nobody can find
it.

In this lesson every name is unique per run: a `mkdtemp()` directory
(mode 0700, so nobody else can even reach the socket) or an abstract name
containing `getpid()`.

### 3.3 Socket types and message boundaries (`c/02_dgram_and_seqpacket.c`)

```
 SOCK_STREAM     write("one") write("two") write("three")
                 read(64) -> "onetwothree"            bytes; boundaries LOST
 SOCK_DGRAM      sendto x3 -> recvfrom x3: "one" | "two" | "three"
                 connectionless, addressed; on AF_UNIX reliable + ordered
 SOCK_SEQPACKET  send x3   -> recv x3:     "one" | "two" | "three"
                 connected (accept, EOF on close) AND boundaries kept
```

| Type | Connection | Boundaries | Ordered, reliable | EOF on peer close | Typical use |
|---|---|---|---|---|---|
| `SOCK_STREAM` | yes | no | yes | yes (`read()==0`) | most daemons, X11, Wayland, D-Bus |
| `SOCK_DGRAM` | no | yes | yes on AF_UNIX (not UDP!) | no | `/dev/log` (syslog), journald, notify socket |
| `SOCK_SEQPACKET` | yes | yes | yes | yes | command protocols, Android `lmkd`, many IPC libs |

Datagram details the program proves:

- **Truncation:** receive a 36-byte datagram into 8 bytes and the other 28
  bytes are *gone*. `recvmsg` sets `MSG_TRUNC`; `recv(fd, NULL, 0,
  MSG_PEEK | MSG_TRUNC)` (Linux) tells you the size first.
- **Replies need a sender name:** an unbound `SOCK_DGRAM` sender is
  anonymous, `recvfrom` returns an empty address, and there is nobody to
  answer. Bind the client too (or use Linux autobind).
- **No silent drops:** when the receiver does not read, a non-blocking
  sender gets `EAGAIN` (a blocking one sleeps). UDP would drop instead.

### 3.4 Framing a stream (`c/03_socketpair_framing.c`)

`socketpair(AF_UNIX, SOCK_STREAM, 0, sv)` gives two already-connected
sockets, like `pipe()` but **bidirectional**. After `fork()`, the parent
keeps `sv[0]`, the child `sv[1]`, and each closes the other end.

A byte stream needs **framing** so the reader knows where a message ends.
The simplest robust format is a length prefix:

```
  +--------+--------+--------+--------+--------------------------+
  |   length (uint32, big-endian)     |  payload (length bytes)  |
  +--------+--------+--------+--------+--------------------------+

  reader:  read_full(4 bytes)  -> len  (reject len > MAX_FRAME!)
           read_full(len bytes) -> payload
```

`read()` may return **fewer** bytes than asked (a 512 KiB frame took
several reads in the run) and `write()` may write fewer (a non-blocking
socket with a full buffer: the program writes 8 MiB and gets ~210 KiB).
So both sides loop:

```c
while (got < len) {                 /* read_full */
    n = read(fd, p + got, len - got);
    if (n < 0 && errno == EINTR) continue;   /* interrupted: retry */
    if (n == 0) -> EOF (error if got > 0: truncated frame)
    got += n;
}
```

The child in `03` even sends some frames **one byte per write()**; the
parent parses them correctly because it never assumed anything about
chunking. Other framing choices: a delimiter (`\n`, as in text protocols;
must escape it), a fixed size, or type-length-value (TLV, like netlink and
D-Bus).

### 3.5 Passing file descriptors with `SCM_RIGHTS` (`c/04_scm_rights_fd_passing.c`)

```
  parent process                kernel                    child process
  fd table                                                fd table
  3 -> [open file A] ----+                          +---- 4
                         |   sendmsg(SCM_RIGHTS {3}) |
                         +--> file A (refcount+1) <--+     (new fd number,
                              "in flight" in the            SAME open file
                              socket buffer                 description:
                                                            same offset!)
```

- The data part must carry **at least one byte** (on a stream socket the
  fds ride on that byte).
- The control buffer is built with `CMSG_SPACE`, `CMSG_LEN`,
  `CMSG_FIRSTHDR`, `CMSG_DATA`, and must be aligned (union with
  `struct cmsghdr`).
- The receiver gets **new fd numbers** pointing at the **same open file
  description**: the program shows the child's `write()` moving the
  parent's file offset, exactly like `dup()`.
- Use `MSG_CMSG_CLOEXEC` on `recvmsg` so received fds do not leak into
  `exec`'d programs. Check `MSG_CTRUNC`: if your control buffer was too
  small, the kernel **closed** the fds that did not fit.
- The sender can close its copy right after `sendmsg`: the in-flight
  reference keeps the file alive.
- Holding the fd *is* the permission. The child reads a memfd that has no
  path at all, and a temp file that was already unlinked.

### 3.6 Peer credentials (`c/05_peer_credentials.c`)

```
  server: cfd = accept(lfd);
          getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &ucred)
          -> { pid = 4242, uid = 1000, gid = 1000 }   filled in by the KERNEL
```

- `SO_PEERCRED` on a connected socket gives the peer's pid/uid/gid **as
  they were at `connect()`** (for the client side: at `listen()`;
  for `socketpair()`: at creation).
- `SCM_CREDENTIALS` attaches credentials to each message. The receiver
  enables it with `SO_PASSCRED`; if the sender adds nothing, the kernel
  fills in the truth. A sender that *claims* someone else's pid gets
  `EPERM` (unless it has `CAP_SYS_ADMIN`). The program proves this.
- pids can be reused after the peer exits: Linux 6.5+ has `SO_PEERPIDFD`
  for a race-free handle. TCP has none of this (`07` shows it).

### 3.7 A request/response server for several clients (`c/06_rpc_server_poll.c`)

```
            +-------------------- poll() --------------------+
            | lfd  (POLLIN = a client waits in the backlog)  |
            | c1   (POLLIN = bytes or EOF)                   |
            | c2                                             |
            +------------------------------------------------+
   ready fd:  lfd -> accept, add slot
              cN  -> ONE read() into cN's buffer
                     while buffer holds a complete frame:
                        execute it, send the reply, drop the bytes
                     read() == 0 -> EOF -> close, remove slot
```

Four forked clients send 25 requests each (`ADD`, `MUL`, `PING`, unknown);
some frames are split over two writes on purpose. The server is a single
thread that never blocks on one client. Each connection has its own
**reassembly buffer**; incomplete frames wait there for the next `poll()`
round. This is the design of every event-driven server; the
[13 — epoll](../13-signals-eventfd-and-epoll/README.md) lesson replaces
`poll()` with `epoll` for thousands of connections. The alternative designs
are fork-per-client (simple, isolated, heavier) and thread-per-client
(`cpp/02`).

### 3.8 Unix vs loopback TCP (`c/07_unix_vs_loopback_tcp.c`)

The same ping-pong function runs over `AF_UNIX` and over TCP on
`127.0.0.1`; only the code that fills `struct sockaddr_storage` differs.

```
  AF_UNIX:  write -> copy into peer's receive queue -> read
  TCP lo :  write -> tcp_sendmsg -> segment -> ip_output -> lo device
            -> ip_rcv -> tcp_v4_rcv -> receive queue (+ ACKs back) -> read
```

On a PC AF_UNIX is typically about 2x faster per round trip (the run
prints the numbers; they are not asserted). On the A72 cores of the TDA4VM
the gap is usually larger. Use TCP only when the peer may move to another
machine (or a container without a shared socket path).

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `socket` / `bind` / `listen` | O(1) syscalls; path bind = 1 inode create | filesystem name lookup O(path length) |
| `connect` + `accept` | O(1), no handshake packets | kernel links two socket structs |
| `send`/`recv` of n bytes | O(n) copy, **one** copy (sender buffer -> skb -> receiver) | no checksums, no headers, no routing |
| Loopback TCP of n bytes | O(n) + full TCP/IP path | segments, ACKs, netfilter hooks |
| `SCM_RIGHTS` with k fds | O(k) | reference counts + fd table install |
| Framing per message | O(1) header + O(n) payload | 4-byte length, then the bytes |
| `poll()` over N fds | O(N) per call | kernel scans every fd; `epoll` is O(ready) |
| Reassembly `memmove` per frame | O(bytes left in buffer) | a ring buffer makes it O(1) |
| Memory | socket buffers: `SO_SNDBUF`/`SO_RCVBUF` (~208 KiB default) | `net.core.wmem_default` |

---

## 5. C vs C++

| Concern | C (this lesson) | C++ (this lesson) |
|---|---|---|
| Owning an fd | `close()` on every path | `UniqueFd`: move-only, closes in destructor |
| Socket | raw int + helper functions | `Socket` class: `bind/listen/accept/connect`, move-only, `std::system_error` on failure |
| Address | fill `sockaddr_un` by hand | `UnixAddress::path()` / `::abstract()` factories that check the length |
| Socket file cleanup | `unlink()` + `rmdir()` at the end | `TempSocketDir` destructor (runs on exceptions too) |
| Framing | `send_frame` / `recv_frame` functions | `Channel::send()` / `recv()` returning `std::optional<Message>` |
| Received fd | `int` you must remember to close | `UniqueFd` inside `Message` |
| RPC dispatch | `if/else` on the request text | `std::map<std::string, std::function<...>>` |
| Concurrency | single-thread `poll()` loop | thread per connection, `std::atomic` counter |

There is no socket class in the C++17 standard library. Real projects use
Boost.Asio (`local::stream_protocol`), standalone Asio, or libuv; Qt has
`QLocalSocket`; GLib has `GSocket`/`GUnixFDMessage`. All of them wrap
exactly the calls in this lesson.

A forked C++ child must call `_exit()`, not return from `main` or call
`exit()`: otherwise it runs static destructors and flushes buffers that
belong to the parent. Fork **before** creating threads (`cpp/02` does).

---

## 6. Where it is used in the real world

- **systemd.** Socket activation: systemd binds `/run/foo.sock`, starts the
  service on the first connection and passes the listening fd (with
  `SCM_RIGHTS`-like inheritance, `sd_listen_fds()`). `sd_notify()` sends
  `READY=1` as a datagram to `$NOTIFY_SOCKET`. journald reads
  `/run/systemd/journal/socket` (DGRAM) and `/dev/log` and uses
  `SCM_CREDENTIALS` to stamp each log line with the real pid/uid.
- **D-Bus.** The system bus is `/run/dbus/system_bus_socket` (STREAM). The
  daemon authenticates clients with `SO_PEERCRED` and can pass fds
  (`UNIX_FD` type) in messages.
- **X11 and Wayland.** X11: `/tmp/.X11-unix/X0`. Wayland:
  `$XDG_RUNTIME_DIR/wayland-0`; clients send buffer fds (shm memfd or
  DMA-BUF) with `SCM_RIGHTS`, which is how zero-copy rendering works.
- **Docker / containerd / Podman.** `/var/run/docker.sock` speaks HTTP over a
  Unix stream socket; access = membership of the `docker` group.
- **Android.** Binder is a kernel driver, not a socket, because it adds
  object references, thread pools and synchronous call semantics. Android
  still uses Unix sockets for `logd`, `lmkd`, `zygote` (abstract and
  `/dev/socket/*` names created by init).
- **GStreamer.** `unixfdsink` / `unixfdsrc` (GStreamer 1.24+) pass
  DMA-BUF/memfd frames between processes with `SCM_RIGHTS`, zero-copy;
  `ipcpipeline` splits a pipeline across processes over a socket/fd pair;
  `shmsink`/`shmsrc` use a Unix socket to coordinate shared memory. See
  [gstreamer/08-queues-threads-tee](../../gstreamer/08-queues-threads-tee/README.md)
  for the threading side of a pipeline.
- **Embedded/RTOS.** FreeRTOS and Zephyr have no processes, so they use
  queues and mailboxes instead; between the A72 Linux side and the R5F/C7x
  cores on the TDA4VM you use RPMsg (`/dev/rpmsg*`), which also exposes a
  message-boundary channel and is often bridged to a Unix socket by a
  user-space daemon.
- **Databases and web.** PostgreSQL `/var/run/postgresql/.s.PGSQL.5432`,
  MySQL, Redis, PHP-FPM and nginx upstreams all prefer Unix sockets locally
  because they are faster than loopback TCP.

---

## 7. Common mistakes and pitfalls

1. **Assuming one `read()` = one message on `SOCK_STREAM`.** Messages
   merge or split. Frame them, or use `SOCK_SEQPACKET`.
   [`c/08_message_per_read_bug.c`](c/08_message_per_read_bug.c) shows it
   deterministically: three commands come back as `"STARTSET gain=4STOP"`.
2. **Ignoring short reads/writes.** Every `read`/`write` needs a loop
   (`read_full`/`write_full`) and `EINTR` handling.
3. **Stale socket file.** `bind()` -> `EADDRINUSE` after a crash. `unlink()`
   first (and only your own path), or use the abstract namespace.
4. **Wrong abstract address length.** Use the exact length; `sizeof(addr)`
   includes trailing NULs in the name.
5. **Path longer than 107 bytes.** `sun_path` is 108 bytes; `snprintf` and
   check, never truncate silently. Use a short directory (`/run/<app>/`).
6. **Trusting a length from the wire.** Cap it (`MAX_FRAME`) before
   `malloc`.
7. **`SIGPIPE`.** Writing to a socket whose peer has closed kills the
   process by default. Use `MSG_NOSIGNAL` on `send`, or ignore `SIGPIPE`
   and handle `EPIPE`.
8. **fd passing details.** No data byte -> nothing sent on a stream socket;
   too-small control buffer -> `MSG_CTRUNC` and lost fds; forgetting
   `MSG_CMSG_CLOEXEC`; forgetting to close received fds (they leak like
   any other fd; `ls -l /proc/<pid>/fd` shows them).
9. **Reading the peer's pid as an authorisation token after it exited.**
   pids are recycled. Decide at connect time, or use `SO_PEERPIDFD`.
10. **Deadlock by waiting for EOF while still holding the write end.** In
    `cpp/01` the server must `shutdown(SHUT_WR)` *before* `waitpid()`,
    otherwise client and server wait for each other. Same with
    `socketpair()` + `fork()`: close the end you do not use.
11. **Forking from a multi-threaded process.** Only the calling thread
    survives in the child; locks held by other threads stay locked forever.
    ThreadSanitizer refuses to support it. Fork first, then start threads.
12. **Abstract sockets are not private.** Anyone in the same network
    namespace can connect. Check credentials.

ThreadSanitizer: the C++ RPC demo is multi-threaded; run it with
`make run P=linux-internals/12-ipc-unix-sockets/cpp/02_framed_channel.cpp SAN=tsan`.
TSan only sees threads inside one process; for the multi-process programs
the self-checks (and `waitpid` exit codes) are the test.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_stream_server_client.c](c/01_stream_server_client.c) | `socket/bind/listen/accept/connect`, half-close, path in `mkdtemp` dir + `unlink`, `EADDRINUSE` on a stale file, abstract name in `/proc/net/unix` | `make run P=linux-internals/12-ipc-unix-sockets/c/01_stream_server_client.c` |
| [c/02_dgram_and_seqpacket.c](c/02_dgram_and_seqpacket.c) | STREAM merges writes; DGRAM `sendto/recvfrom`, truncation `MSG_TRUNC`, anonymous sender, `EAGAIN` instead of drop; SEQPACKET boundaries + EOF | `make run P=linux-internals/12-ipc-unix-sockets/c/02_dgram_and_seqpacket.c` |
| [c/03_socketpair_framing.c](c/03_socketpair_framing.c) | `socketpair` + `fork`, length-prefix frames, `read_full`/`write_full`, byte-by-byte sender, measured short reads and a short write | `make run P=linux-internals/12-ipc-unix-sockets/c/03_socketpair_framing.c` |
| [c/04_scm_rights_fd_passing.c](c/04_scm_rights_fd_passing.c) | Pass a memfd and an unlinked temp file with `SCM_RIGHTS`; shared file offset; `MSG_CMSG_CLOEXEC` | `make run P=linux-internals/12-ipc-unix-sockets/c/04_scm_rights_fd_passing.c` |
| [c/05_peer_credentials.c](c/05_peer_credentials.c) | `SO_PEERCRED` on both ends, `SO_PASSCRED` + `SCM_CREDENTIALS`, a spoofed pid rejected with `EPERM` | `make run P=linux-internals/12-ipc-unix-sockets/c/05_peer_credentials.c` |
| [c/06_rpc_server_poll.c](c/06_rpc_server_poll.c) | Single-threaded `poll()` RPC server, 4 forked clients, per-connection reassembly buffer | `make run P=linux-internals/12-ipc-unix-sockets/c/06_rpc_server_poll.c` |
| [c/07_unix_vs_loopback_tcp.c](c/07_unix_vs_loopback_tcp.c) | Same code over `AF_UNIX` and TCP `127.0.0.1`, round-trip times, no credentials on TCP | `make run P=linux-internals/12-ipc-unix-sockets/c/07_unix_vs_loopback_tcp.c` |
| [c/08_message_per_read_bug.c](c/08_message_per_read_bug.c) | **Bug:** "one read = one message" on a stream socket; its own CHECK fails (no sanitizer can see it) | `make run P=linux-internals/12-ipc-unix-sockets/c/08_message_per_read_bug.c` |
| [cpp/01_raii_socket.cpp](cpp/01_raii_socket.cpp) | `UniqueFd`, move-only `Socket`, `UnixAddress`, `TempSocketDir` cleanup, errors as `std::system_error` | `make run P=linux-internals/12-ipc-unix-sockets/cpp/01_raii_socket.cpp` |
| [cpp/02_framed_channel.cpp](cpp/02_framed_channel.cpp) | `Channel` with frames + optional fd, fd passing across fork, RPC server with `std::function` handlers and client threads | `make run P=linux-internals/12-ipc-unix-sockets/cpp/02_framed_channel.cpp` |

Every program also runs under ThreadSanitizer, e.g.
`make run P=linux-internals/12-ipc-unix-sockets/cpp/02_framed_channel.cpp SAN=tsan`.
Inspect live sockets while a program runs with `ss -xp` (Unix sockets with
owning processes) or `cat /proc/net/unix`.

---

## 9. Exercises

**Easy**
1. In `01`, replace the filesystem name with `/tmp/uds-demo-XXXXXX/` plus a
   name of 120 characters. What does the program do? Why must you never
   just truncate?
2. In `02`, run `ss -xa | grep uds-dgram` in another terminal (add a
   `sleep(10)` first). Which sockets are listed, and with what type?
3. Change `08_message_per_read_bug.c` to use `SOCK_SEQPACKET`. Does it pass
   now? Why?

**Medium**
4. Add a "type" byte to the frame header in `03` (TLV: type, length,
   value) and send two kinds of messages. The receiver copies the header
   out with `memcpy`: why is casting the buffer to a `struct` pointer a
   bad idea (alignment, padding, endianness)?
5. Make `06` serve 100 clients. Replace `poll()` with `epoll` (lesson 13).
   Then add a per-client write buffer so the server never blocks in
   `write()` when a client stops reading (wait for `POLLOUT`).
6. In `05`, make the server refuse clients whose uid differs from its own
   and print a message in the client when the connection is closed.
7. Pass a **socket** with `SCM_RIGHTS`: a parent accepts a connection and
   hands the connected fd to a worker child (this is the pre-fork server
   model of nginx/Apache and how `systemd` passes sockets).

**Hard**
8. Write `sd_notify()` yourself: read `$NOTIFY_SOCKET` (abstract if it
   starts with `@`), send `READY=1` as a datagram. Test it with
   `systemd-run --user -p Type=notify ./prog`.
9. Use `SCM_RIGHTS` to send a DMA-BUF fd from one process to another on the
   BeagleBone AI-64 (e.g. from a V4L2 capture with `VIDIOC_EXPBUF`) and
   `mmap` it in the receiver. Compare with copying the frame over the
   socket.
10. Make `cpp/02` non-blocking and single-threaded: one `epoll` loop, a
    per-connection read buffer and write queue, handlers unchanged.

---

## 10. Remember forever

```
socket(AF_UNIX, STREAM|DGRAM|SEQPACKET) -> bind(path | "\0abstract") -> listen -> accept
client: socket -> connect(same addr).  Path: unlink() it yourself. Abstract: exact addrlen.
STREAM = bytes, NO boundaries -> frame: [u32 BE len][payload], read_full/write_full + EINTR
DGRAM = boundaries, no connection, reliable on AF_UNIX; too-small buffer TRUNCATES
SEQPACKET = boundaries + connection + EOF.  socketpair() = connected pair, no name.
SCM_RIGHTS: >=1 data byte, CMSG_SPACE/LEN/DATA, MSG_CMSG_CLOEXEC, check MSG_CTRUNC
  -> receiver gets NEW fd number, SAME open file (shared offset)
SO_PEERCRED: kernel-verified pid/uid/gid of the peer. TCP has neither creds nor fds.
SIGPIPE: MSG_NOSIGNAL or SIG_IGN.  Fork before threads; children _exit().
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Name three things a Unix socket can do that a pipe cannot.
2. What happens to a filesystem socket file when the server closes the
   socket? What error does the next `bind()` give, and how do daemons avoid
   it?
3. How is an abstract socket name written, and why must you pass the exact
   `addrlen`? Who can connect to it?
4. You `write()` "A", "B", "C" on a stream socket. What can a single
   `read()` on the other end return? What about on `SOCK_SEQPACKET`?
5. Write the loop of `read_full()` from memory, including `EINTR` and EOF.
6. A datagram of 100 bytes is received into a 10-byte buffer. What happens
   to the other 90 bytes, and how can you detect it?
7. After passing an fd with `SCM_RIGHTS`, the receiver calls `write()` on
   it. Why does the sender's file offset change?
8. Why must the data part of an `SCM_RIGHTS` message contain at least one
   byte on a stream socket?
9. What does `SO_PEERCRED` return on the *client* side of a connection?
   Why can a client not lie about its uid, and what happens if it tries
   with `SCM_CREDENTIALS`?
10. In a `poll()` server, why does each connection need its own buffer?
    What goes wrong if you call `read_full()` on a client fd instead?
11. Why is AF_UNIX faster than TCP on `127.0.0.1`? Name two features TCP
    loopback lacks.
12. Why should a multi-threaded program not `fork()` and then keep running
    library code in the child?
