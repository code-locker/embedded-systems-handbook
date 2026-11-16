# 10 — Message queues (POSIX and System V)

> A pipe moves *bytes*. A message queue moves *messages*: each send is
> delivered as exactly one receive, never split or glued, and the kernel can
> order them by **priority** (POSIX) or let you pick them by **type**
> (System V). It is the kernel-provided version of the bounded
> producer/consumer queue you build yourself in lesson 04, and the closest
> Linux relative of an RTOS queue.

The map of all IPC mechanisms and the "which one should I pick" guide are in
[lesson 09, section 1](../09-ipc-pipes-and-fifos/README.md#1-why-this-exists).

---

## 1. Why this exists

With a pipe, the receiver must re-discover message boundaries. It needs
delimiters, length prefixes, partial-read loops. Many embedded designs only
ever exchange small fixed records: "temperature = 41.5 °C", "start
recording", "button 3 pressed". For those you want:

- **whole messages**: one `send` = one `receive`;
- **a bound**: at most N messages queued, so a stuck consumer causes
  back-pressure, not unbounded memory growth;
- **urgency**: an alarm should overtake 100 routine readings;
- **a name**: unrelated processes (a sensor daemon, a UI, a logger) find the
  queue without a common parent;
- **no consumer needed at send time**: the message waits in the kernel.

POSIX message queues (`mq_open`, 2001) and System V message queues
(`msgget`, 1983) both provide this. Their APIs differ a lot.

---

## 2. Mental model

**A message queue is the hospital A&E waiting room.** Each patient (message)
is a whole person, never half. The room has a fixed number of chairs
(`mq_maxmsg`). When it is full, the ambulance waits outside (sender blocks).
The triage nurse calls the most urgent patient first (priority). Among
equally urgent patients, the one who arrived first goes first (FIFO within a
priority). The room exists whether or not a doctor is on shift (kernel
persistence).

```
  POSIX mq "/sensors"   (maxmsg = 8, msgsize = 16)

  mq_send(prio 9) ─┐      kernel, one list per priority (highest first)
  mq_send(prio 5) ─┼─>  ┌─────────────────────────────────────────────┐
  mq_send(prio 0) ─┘    │ prio 9: [D]                                 │ ──> mq_receive
                        │ prio 5: [B] [E]                             │     gets D, B, E, A, C
                        │ prio 1: [A] [C]                             │
                        └─────────────────────────────────────────────┘
        every message is COPIED in (send) and COPIED out (receive)

  System V queue (key 0x51…): one FIFO list, each message tagged with mtype
     [3:c1] [1:a1] [2:b1] [3:c2] [1:a2]
     msgrcv(type 0)  -> c1  (head)        msgrcv(type 2)  -> b1  (exact type)
     msgrcv(type -3) -> a1  (lowest type <= 3)
```

---

## 3. How it works

### 3.1 POSIX message queues  (`c/01_posix_mq_basics.c`)

```c
struct mq_attr attr = { .mq_maxmsg = 8, .mq_msgsize = 64 };
mqd_t q = mq_open("/name", O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600, &attr);
mq_send(q, buf, len, prio);              /* len <= mq_msgsize, prio 0..31+ */
ssize_t n = mq_receive(q, buf, attr.mq_msgsize, &prio);  /* buffer >= msgsize! */
mq_close(q);   mq_unlink("/name");
```

- **Name**: `/` + no other slash. On Linux the queue appears as
  `/dev/mqueue/name`. `cat` it to see `QSIZE`, `NOTIFY`, `NOTIFY_PID`, and
  `rm` it to unlink.
- **Attributes are fixed at creation**: `mq_maxmsg`, `mq_msgsize`.
  `mq_setattr` can only toggle `O_NONBLOCK`.
- **Receive buffer must be ≥ `mq_msgsize`**, even for a 2-byte message.
  Otherwise you get `EMSGSIZE`. Sending more than `mq_msgsize` → `EMSGSIZE`.
- **Priority**: higher number = more urgent, FIFO within one priority.
  `MQ_PRIO_MAX` ≥ 32 (Linux: 32768).
- **Blocking**: `mq_send` blocks when full, `mq_receive` when empty. With
  `O_NONBLOCK` both return `EAGAIN`. `mq_timedsend`/`mq_timedreceive` take
  an **absolute `CLOCK_REALTIME`** deadline and return `ETIMEDOUT`.
- **Persistence**: `mq_close` does not delete. Messages wait for the next
  opener. `mq_unlink` removes the name, and the queue is freed when the last
  descriptor closes.
- **Limits** (`/proc/sys/fs/mqueue/`): `msg_max` (default 10) and
  `msgsize_max` (8192) cap what an unprivileged user may request.
  `queues_max` (256) caps queues system-wide. `RLIMIT_MSGQUEUE` (`ulimit -q`,
  819 200 bytes) caps the total memory per user. Asking for more →
  `EINVAL`/`EMFILE`.
- **Linking**: in glibc ≥ 2.34 the `mq_*` functions are in libc itself, so
  no `-lrt` is needed. Older systems need `-lrt`.

### 3.2 Producer/consumer over an mq  (`c/02_mq_producer_consumer.c`)

```
   child (producer)                      kernel mq (8 slots)          parent (consumer)
   mq_open(name, O_WRONLY)                                            mq_open(..., O_NONBLOCK)
   for 500: mq_send(&msg, prio 0) ──>  [m][m][m][m][m][m][m][m]  ──>  poll(mqd, POLLIN, 2 s)
   after #250: mq_send(URGENT, prio 10)  (jumps the line)              drain until EAGAIN
   mq_send(END, prio 0)  <- same prio as data, or it overtakes data!
```

- The message is a **plain struct** with fixed-width fields, explicit
  padding and no pointers, plus a checksum the consumer verifies.
- **Back-pressure** is free: with 8 slots, the producer sleeps whenever the
  consumer falls behind.
- **On Linux an `mqd_t` is a file descriptor.** You can `poll`/`select`/`epoll`
  it with sockets and timers (lesson 13). This is not portable POSIX, but
  it is the most practical way to wait on a queue in an event loop.
- **Pitfall shown**: an END/"shutdown" message sent at *higher* priority
  overtakes the data still queued, so the consumer stops early.

### 3.3 `mq_notify`  (`c/03_mq_notify.c`)

Register once to be told when a message arrives on an **empty** queue:

```c
struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1,
                        .sigev_value.sival_int = cookie };
mq_notify(q, &sev);      /* or SIGEV_THREAD: glibc starts a thread for you */
```

| Rule | Consequence |
|---|---|
| fires only on **empty → non-empty** | registering on a non-empty queue gives no signal until it is drained and refilled |
| **one-shot** | after it fires, register again |
| **one process** per queue | a second `mq_notify` → `EBUSY` |
| not if someone is blocked in `mq_receive` | the blocked receiver gets the message instead |
| `mq_notify(q, NULL)` | unregister |

Correct loop: *on notification → re-register first → then drain until
`EAGAIN`*. If you drain first and re-register second, a message that arrives
in between is never notified. The program blocks `SIGUSR1` and collects it
with `sigtimedwait()`, which is synchronous, race-free and has a timeout. It
checks `si_code == SI_MESGQ`, the cookie, and the sender pid. With
`SIGEV_THREAD`, glibc runs your function in a new thread for each
notification. That is convenient, but the thread is created inside libc.
`poll()` on the mqd is usually the simplest choice on Linux.

### 3.4 System V message queues  (`c/04_sysv_msgqueue.c`)

```c
key_t key = ftok("/some/existing/path", 'Q');      /* or IPC_PRIVATE */
int id = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
struct { long mtype; char text[32]; } m = { 3, "hello" };
msgsnd(id, &m, sizeof m.text, 0);                  /* size WITHOUT mtype */
msgrcv(id, &m, sizeof m.text, type, flags);        /* type selects! */
msgctl(id, IPC_RMID, NULL);
```

- **`ftok(path, id)`** mixes the file's inode/device with an 8-bit id into a
  key. Every program agreeing on `(path, id)` gets the same key. Collisions
  are possible, which `IPC_EXCL` detects. `IPC_PRIVATE` makes a queue
  reachable only through its numeric id (e.g. by fork children).
- **Type selection** is the killer feature:
  `0` = FIFO head; `>0` = first of exactly that type; `<0` = lowest type
  ≤ |t| (a priority queue where *lower* is more urgent); `MSG_EXCEPT`
  (Linux) = first *not* of that type.
- **Request/reply multiplexing**: clients send with `mtype = 1` and put
  their pid in the body. The server replies with `mtype = client_pid`.
  Many clients share **one** queue, and each `msgrcv(…, getpid(), …)` sees
  only its own replies.
- Errors differ from POSIX: empty + `IPC_NOWAIT` → `ENOMSG`. Buffer too
  small → `E2BIG`, unless `MSG_NOERROR` truncates silently.
- **`IPC_RMID` destroys immediately.** Blocked senders/receivers wake with
  `EIDRM`. This differs from `mq_unlink`, which waits for the last close.
- **Tools**: `ipcs -q` lists queues (key, id, owner, bytes, messages).
  `ipcrm -q <id>` removes one. Limits: `/proc/sys/kernel/msgmax` (max message
  size, 8192), `msgmnb` (max bytes per queue, 16384), `msgmni` (number of
  queues).

### 3.5 POSIX vs System V

| | POSIX `mq_*` | System V `msg*` |
|---|---|---|
| Naming | `"/name"` (string) | `key_t` from `ftok` or `IPC_PRIVATE` → int id |
| Handle | `mqd_t` (an **fd** on Linux, pollable, `O_CLOEXEC`) | int id (not an fd, cannot poll) |
| Ordering | priority (high first), FIFO within | FIFO; receive by type, `-type` = lowest first |
| Selective receive | no | yes (`mtype`) |
| Notification | `mq_notify` (signal or thread), `poll`/`epoll` | none (block, or poll with `IPC_NOWAIT`) |
| Timeouts | `mq_timedsend`/`mq_timedreceive` | none (use `IPC_NOWAIT` + sleep, or signals) |
| Removal | `mq_unlink` (freed after last close) | `IPC_RMID` (immediate, `EIDRM`) |
| Limits | `/proc/sys/fs/mqueue/*`, `RLIMIT_MSGQUEUE` | `/proc/sys/kernel/msg{max,mnb,mni}` |
| Inspect | `ls /dev/mqueue`, `cat /dev/mqueue/x` | `ipcs -q`, `ipcrm -q` |
| Use in new code | **yes** | only for type-based receive or legacy |

### 3.6 Compared with RTOS queues

On FreeRTOS (and on the R5F cores of the TDA4VM if they run an RTOS) the
queue is the main IPC primitive:

```c
QueueHandle_t q = xQueueCreate(8, sizeof(struct reading));   /* like mq_open + attrs */
xQueueSend(q, &r, pdMS_TO_TICKS(10));        /* copies sizeof(item) bytes in, timeout */
xQueueReceive(q, &r, portMAX_DELAY);         /* copies them out, blocks */
xQueueSendToFront(q, &alarm, 0);             /* "urgent": crude priority */
xQueueSendFromISR(q, &r, &woken);            /* from an interrupt */
```

| | Linux mq | FreeRTOS queue |
|---|---|---|
| Copy semantics | by value (2 copies through the kernel) | **also by value** (copied into queue storage). Large data: send a pointer to a buffer pool, the RTOS form of "shared memory" |
| Priority | per message (0..32767) | none, only `SendToFront` (LIFO insert) |
| Boundaries | whole messages | whole items of fixed size |
| Between | processes (separate address spaces) | tasks in one address space (no MMU isolation) |
| From an interrupt | no (only kernel code runs there) | `...FromISR` variants |
| Waiting on several | `poll`/`epoll` on mqds | queue sets (`xQueueAddToSet`) |
| Persistence | kernel object with a name | lives in RAM until deleted, no name |

Zephyr's `k_msgq` is the same idea. Between the A72 Linux side and the R5F
RTOS side of the TDA4VM, messages travel over **RPMsg** (virtio rings in
shared memory, see lesson 11 and dsa/07). On Linux the endpoint is a
character device or a socket, not an mq.

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| `mq_open` / `msgget` | O(1) + name lookup | creates or finds the kernel object |
| `mq_send` | O(len) copy + O(log P) | Linux keeps an rbtree of priority levels, each a FIFO list |
| `mq_receive` | O(len) copy + O(1) | takes the head of the highest priority list (cached) |
| `msgsnd` | O(len) copy + O(1) | append to one list |
| `msgrcv(type 0)` | O(1) | list head |
| `msgrcv(type >0 / <0)` | **O(n)** in queued messages | walks the list to find a match |
| Latency | ~5–20 µs send→wake receiver | syscall + scheduler wakeup |
| Throughput | ~1 M small msgs/s on a desktop | dominated by two syscalls per message |
| Memory | maxmsg × msgsize (+ overhead), charged to `RLIMIT_MSGQUEUE` | preallocated limit, allocated on send |

---

## 5. C vs C++

| Concern | C (`c/*.c`) | C++ (`cpp/01_typed_message_queue.cpp`) |
|---|---|---|
| Message type | `const char *` + length, cast your struct | `MessageQueue<T>`: send/receive a `T` |
| Type safety | none, sender and receiver must agree by convention | `static_assert(is_trivially_copyable_v<T>)` rejects `std::string` & co. at compile time; `open()` checks `msgsize == sizeof(T)` at run time |
| Cleanup | `mq_close` + `mq_unlink` on every path | destructor closes; the **creator** object also unlinks |
| Errors | `-1` + `errno` | `std::system_error` |
| Timeouts | absolute `timespec` on `CLOCK_REALTIME` | `receive_for(std::chrono::milliseconds)` → `std::optional<T>` |
| Std equivalent | — | none in the standard; Boost.Interprocess `message_queue` (in shared memory, not the kernel) |

Why trivially copyable? A message is copied **byte by byte** into the kernel
and out in another process. A `std::string` member is a pointer to heap
memory that exists only in the sender. A `std::vector` too. A type with a
vtable carries a pointer to the sender's code. `is_trivially_copyable` is
the compiler's way of saying "its bytes are its value".

The C++ program also shows the **RAII + fork** trap. The child holds a
fork-copy of the owner object. If it returned normally, the destructor would
`mq_unlink` the parent's queue. Children `_exit()`.

---

## 6. Where it is used in the real world

- **Embedded control software**: a sensor-acquisition process posts
  readings to an mq, and a control/HMI process `epoll`s on it together with
  a CAN socket and a timerfd. Alarms go at a higher priority.
- **Industrial and automotive in-house frameworks** often use POSIX mqs
  as the local transport between a few long-running processes on one ECU.
  Priorities separate diagnostics and alarms from routine data, and the
  bounded size gives back-pressure for free.
- **Linux kernel**: `ipc/mqueue.c` (POSIX, a tiny filesystem `mqueue` behind
  `/dev/mqueue`), `ipc/msg.c` (System V). Both are namespaced per IPC
  namespace, so containers do not see each other's queues.
- **glibc**: `mq_*` wrappers, `mq_notify` with `SIGEV_THREAD` (uses a helper
  netlink socket and a thread).
- **Legacy Unix software**: print spoolers, Oracle/SAP components, telecom
  stacks and old PLC gateways still use System V queues with typed
  receive.
- **GStreamer**: does not use kernel mqs between elements, because all
  elements run in one process. The `queue` element and `GstAtomicQueue`
  are the in-process equivalent (see
  [gstreamer/08](../../gstreamer/08-queues-threads-tee/README.md)). Across
  processes GStreamer uses `shmsink`/`shmsrc` (lesson 11) or Unix sockets
  (`ipcpipeline`).
- **RTOS**: FreeRTOS `xQueueSend`/`xQueueReceive`, Zephyr `k_msgq`, VxWorks
  `msgQSend`, QNX native message passing (`MsgSend`/`MsgReceive`, synchronous
  and copy-based), all §3.6.

---

## 7. Common mistakes and pitfalls

1. **Receive buffer smaller than `mq_msgsize`** → `EMSGSIZE`, even for tiny
   messages. Always use `mq_getattr` and allocate `mq_msgsize`.
2. **Asking for more than `msg_max`/`msgsize_max`** as a normal user →
   `EINVAL` from `mq_open`. Defaults are only 10 messages × 8 KiB.
3. **Forgetting `mq_unlink` / `IPC_RMID`**: queues are kernel-persistent and
   pile up until reboot (`ls /dev/mqueue`, `ipcs -q`). Use unique names
   (include the pid) and clean up. A crashed run leaves its queue behind.
4. **Re-using a stale queue** created by an older version of the program
   with a different `msgsize`. Use `O_EXCL` when creating, and check
   attributes when opening (as `MessageQueue<T>::open` does).
5. **Shutdown message at high priority** overtakes queued data
   (`c/02`).
6. **Sending pointers or `std::string`** inside messages: the receiver gets
   an address that means nothing in its process.
7. **`msgsnd` size including `mtype`**: the size argument is the payload
   only. Using `sizeof(struct)` reads 8 bytes past the end.
8. **`mq_notify` misuse**: expecting it to fire for every message (it is
   one-shot, and only on empty → non-empty), or draining before
   re-registering.
9. **Relative timeouts**: `mq_timedreceive` wants an *absolute*
   `CLOCK_REALTIME` time. Passing `{1, 0}` means "1970 + 1 s" and returns
   immediately.
10. **Using an mq for bulk data**: two copies per message and an 8 KiB cap.
    Frames belong in shared memory (lesson 11), with the mq carrying only
    "frame #42 is ready in slot 3".
11. **Expecting ThreadSanitizer to check the other process**: it sees one
    process. The programs here still run under TSan
    (`make run P=linux-internals/10-ipc-message-queues/c/02_mq_producer_consumer.c SAN=tsan`)
    with a single-threaded parent at `fork()` time. Cross-process
    correctness comes from the self-checks (sequence numbers, checksums).

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_posix_mq_basics.c](c/01_posix_mq_basics.c) | `mq_open` with attrs, `/dev/mqueue`, priority order D B E A C, `EMSGSIZE`, timed send/receive, `O_NONBLOCK`/`EAGAIN`, persistence, `mq_unlink`, limits | `make run P=linux-internals/10-ipc-message-queues/c/01_posix_mq_basics.c` |
| [c/02_mq_producer_consumer.c](c/02_mq_producer_consumer.c) | producer process → consumer process, struct messages with checksum, back-pressure, `poll()` on an mqd, urgent message overtaking | `make run P=linux-internals/10-ipc-message-queues/c/02_mq_producer_consumer.c` |
| [c/03_mq_notify.c](c/03_mq_notify.c) | `mq_notify(SIGEV_SIGNAL)` + `sigtimedwait`, `SI_MESGQ`, one-shot, `EBUSY`, empty→non-empty rule, unregister | `make run P=linux-internals/10-ipc-message-queues/c/03_mq_notify.c` |
| [c/04_sysv_msgqueue.c](c/04_sysv_msgqueue.c) | `ftok`, `msgget`, receive by type 0 / >0 / <0 / `MSG_EXCEPT`, `E2BIG`/`MSG_NOERROR`, multi-client reply-by-pid, `IPC_STAT`, `IPC_RMID` | `make run P=linux-internals/10-ipc-message-queues/c/04_sysv_msgqueue.c` |
| [cpp/01_typed_message_queue.cpp](cpp/01_typed_message_queue.cpp) | `MessageQueue<T>` RAII template, `static_assert` trivially copyable, size check on open, `std::optional` timed receive, owner unlinks | `make run P=linux-internals/10-ipc-message-queues/cpp/01_typed_message_queue.cpp` |

All of them also run clean under ThreadSanitizer, e.g.
`make run P=linux-internals/10-ipc-message-queues/c/04_sysv_msgqueue.c SAN=tsan`.

---

## 9. Exercises

**Easy**
1. Run `c/01` but comment out `mq_unlink`. Find the leftover queue in
   `/dev/mqueue`, `cat` it, and remove it with `rm`.
2. While `c/04` runs (add a `sleep(5)` before `IPC_RMID`), look at the queue
   with `ipcs -q` and `ipcs -q -i <id>`.
3. Make `c/02` send END at priority 10 and watch the check fail. Explain the
   message count.

**Medium**
4. Two producers, one consumer on one POSIX queue: tag messages with the
   producer id and verify per-producer order. Is the order *between*
   producers defined?
5. Rewrite `c/03` with `SIGEV_THREAD`. Where does your callback run, and
   what must it do before draining the queue?
6. Implement a request/reply RPC with POSIX queues: one well-known server
   queue, and one reply queue per client named `/reply_<pid>`. Compare with
   the single-queue `mtype = pid` trick in `c/04`.

**Hard**
7. Write an event loop with `epoll` that waits on an mqd, a `timerfd`
   (1 Hz heartbeat) and stdin at the same time (preview of lesson 13).
8. Measure messages per second for (a) POSIX mq, (b) a pipe with
   fixed-size records, (c) the shared-memory SPSC ring from lesson 11, for
   16-byte messages. Explain the ranking. Run it on the BeagleBone AI-64 too.
9. Port the producer/consumer to FreeRTOS (or its POSIX simulator) with
   `xQueueSend`/`xQueueReceive`. Keep the message struct identical, and
   list what changes: priorities, timeouts, ISR variants.

---

## 10. Remember forever

```
mq = MESSAGES (boundaries kept) + bounded + priority; kernel-persistent until unlink
mq_open("/n", O_CREAT|O_EXCL|O_RDWR, 0600, &{maxmsg, msgsize})  -- attrs fixed forever
receive buffer >= mq_msgsize, else EMSGSIZE.   prio: HIGH first, FIFO within
mq_timed*: ABSOLUTE CLOCK_REALTIME deadline.   O_NONBLOCK -> EAGAIN
Linux: mqd_t is an fd -> poll/epoll it.  mq_notify: one-shot, empty->nonempty, 1 process
SysV: ftok -> msgget -> msgsnd(size w/o mtype) -> msgrcv(type 0 | >0 exact | <0 lowest<=|t|)
IPC_RMID kills now (EIDRM); mq_unlink waits for last close.  ipcs -q / ipcrm / /dev/mqueue
messages are COPIED (twice): plain structs only, no pointers; bulk data -> shm (11)
RTOS: xQueueSend/xQueueReceive also copy by value
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What does "message-oriented" give you that a pipe does not? Give one case
   where a pipe is still the better choice.
2. Five messages are sent with priorities 1, 5, 1, 9, 5. In what order are
   they received, and why?
3. Why must the `mq_receive` buffer be at least `mq_msgsize` bytes, even if
   all messages are small?
4. What happens to the messages in a POSIX queue when every process closes
   it? When it is unlinked? When the machine reboots?
5. Your program crashed during testing and now `mq_open(O_CREAT|O_EXCL)`
   fails. Why, and how do you find and remove the culprit?
6. Explain the three cases of `msgtyp` in `msgrcv`. How do you build "one
   server queue, many clients, private replies" with them?
7. Why is it wrong to send a struct containing a `std::string` through a
   message queue? What does `static_assert(std::is_trivially_copyable_v<T>)`
   protect against?
8. When does `mq_notify` fire, and why should you re-register *before*
   draining?
9. How would you wait for "a message on the queue OR data on a socket OR a
   timeout" in one place on Linux?
10. Compare a POSIX mq with a FreeRTOS queue: copy semantics, priorities,
    ISR use, isolation.
11. Why do message queues not suit 1080p video frames, and what should carry
    them instead?
