# 16 — Zero-Copy and Async I/O

> Moving data is often the most expensive thing an embedded Linux system
> does: camera frames, video streams, logs, firmware images, network
> traffic. This lesson is about moving it **fewer times** (zero-copy:
> `sendfile`, `splice`, `copy_file_range`, `mmap`, `O_DIRECT`,
> `MSG_ZEROCOPY`, DMA-BUF) and **without blocking a thread per request**
> (async I/O: epoll, POSIX AIO, io_uring).

---

## 1. Why this exists

A 1080p30 NV12 camera stream is 3 MB per frame, 93 MB/s. Copy it four
times on its way from the sensor to the encoder and the network and you
spend ~370 MB/s of memory bandwidth on copies alone, plus the CPU time and
the cache pollution. The two Cortex-A72 cores of the TDA4VM share one
LPDDR4 interface with the GPU, the C7x DSP, the video codec and the display.
Every copy you skip is bandwidth those blocks get back.

The second problem is waiting. A thread that calls `read()` on a slow device
sleeps until the data arrives. Serving 1000 clients or keeping an NVMe queue
full with blocking calls needs 1000 threads. Async I/O lets one thread keep
many requests in flight.

| You need... | Use |
|---|---|
| send a file to a socket | `sendfile()` (§3.3) |
| copy a file | `copy_file_range()` (reflink on btrfs/XFS), else `sendfile()` (§3.2) |
| move data between two fds, one is a pipe | `splice()` ([09 §3.8](../09-ipc-pipes-and-fifos/README.md)) |
| random access to a big file, or share it between processes | `mmap()` (§3.5) |
| stream huge files without trashing the page cache | `O_DIRECT` or `posix_fadvise(DONTNEED)` (§3.4) |
| very large socket sends at 10 Gbit/s+ | `MSG_ZEROCOPY` (§3.6) |
| many sockets in one thread | non-blocking + `epoll` ([13](../13-signals-eventfd-and-epoll/README.md)) |
| many file/disk requests in flight, few syscalls | `io_uring` (§3.8) |
| hand a camera/GPU/codec buffer to another device or process | DMA-BUF fd (§3.9) |

---

## 2. Mental model

**Copies are trips through a narrow corridor.** The page cache, your user
buffer, the socket buffer and the device are rooms. Every `read()` or
`write()` carries the data through the corridor (the CPU doing `memcpy`,
using memory bandwidth both ways). Zero-copy means handing over a **note
saying where the data already is** (a page reference, a file descriptor,
a DMA address) instead of carrying the data.

```
 read()+write():  4 trips, 2 by the CPU, 4 syscalls per chunk
   disk --DMA--> page cache --CPU copy--> user buf --CPU copy--> socket buf --DMA--> NIC
                             (read)                  (write)

 mmap()+write():  1 CPU copy, but page faults to map the file
   disk --DMA--> page cache ==mapped== user  --CPU copy--> socket buf --DMA--> NIC

 sendfile() / splice():  no user-space copy; pages referenced in the kernel
   disk --DMA--> page cache --page refs--> socket --DMA (scatter-gather)--> NIC

 O_DIRECT:  no page cache at all
   disk --DMA--> user buf                        (alignment rules apply)

 DMA-BUF:  devices share one buffer by fd
   camera --DMA--> [ dmabuf ] <--DMA-- encoder      CPU never touches the pixels
```

Remember this sentence: **every copy costs memory bandwidth twice (read +
write) and pollutes the caches; zero-copy hands over references, and the
price is alignment, page faults, pinning or lifetime rules.**

---

## 3. How it works

### 3.1 Why copies cost

- **Bandwidth.** `memcpy` of N bytes reads N and writes N (and on a
  write-allocate cache, first reads the destination line too). Single-core
  `memcpy` on an A72 reaches a few GB/s, a fraction of what the DDR delivers
  to all masters together. The GPU, codec and display compete for the same
  DDR.
- **Caches.** Copying 3 MB through a 1 MB L2 (the A72 cluster's) evicts
  everything else the CPU was using.
- **The user/kernel boundary.** `copy_to_user`/`copy_from_user` are
  ordinary copies plus fault handling (and on ARMv8.x with PAN, extra
  instructions to enable user access). Each syscall also costs an
  exception entry/exit (~0.1–1 µs, more with mitigations).
- **Syscall count.** Four syscalls per 64 KiB chunk for read+write. Batching
  (bigger chunks, `io_uring`, `sendmmsg`) amortises that.

### 3.2 Copying a file: five ways (`c/01_copy_file_to_file.c`)

| Method | User-space copies | Syscalls | Notes |
|---|---|---|---|
| `read` + `write` | 2 | 2 per chunk | portable, simple, the baseline |
| `mmap` + `write` | 1 | 1 + page faults | faults cost, see §3.5 |
| `sendfile(out, in, &off, n)` | 0 | 1 per ≤2 GiB | any `out_fd` since 2.6.33 |
| `copy_file_range(in, NULL, out, NULL, n, 0)` | 0 | 1 | filesystem decides: reflink on btrfs/XFS (no data moved), server-side copy on NFS 4.2, in-kernel splice elsewhere |
| `splice` file → pipe → file | 0 | 2 per chunk | one end must be a pipe |

`c/01` copies an 8 MiB file all five ways and asserts identical checksums.
With a warm page cache the in-kernel methods win by a margin that depends on
the machine (on this x86 host ~20 % with ASan, and under TSan, which
instruments user-space copies, read/write looks 7× slower). The times never
include the disk: writes land in the page cache. This is what `cp` does
since coreutils 9: try `copy_file_range`, fall back to `read`/`write`.

### 3.3 File to socket: sendfile and splice (`c/02_file_to_socket.c`)

The classic zero-copy case: nginx (`sendfile on;`), Apache, Kafka, a
firmware-update server, an RTSP server sending recorded video.

```
 sendfile(sock, file, &off, n):
   page cache pages --(references)--> TCP socket skbs --(scatter-gather DMA)--> NIC
   with checksum offload, the CPU never reads the payload
```

- The receiving side still copies on `read()`/`recv()` (unless it uses
  `mmap`ed TCP receive or io_uring zero-copy receive, both rare).
- Over **AF_UNIX** and **TCP loopback** the kernel copies the pages into
  the socket buffer anyway, so `c/02` shows sendfile and splice saving only
  the sender's user-space copy and syscalls. The full win needs a real NIC.
- `sendfile` with an offset pointer leaves the file position alone, so
  many threads can serve one fd.
- `TCP_CORK` / `MSG_MORE` (`SPLICE_F_MORE`) let you send a header with
  `write` and the body with `sendfile` in full-sized segments.

### 3.4 Buffered vs direct I/O (`c/03_direct_io.c`)

```
 buffered write():  user buf --copy--> page cache (dirty) --> return
                    ... later, writeback (or fsync) --DMA--> device
 O_DIRECT write():  user buf --DMA--> device --> return (still fdatasync for
                    the device's own cache and the metadata)
```

| | Buffered | `O_DIRECT` |
|---|---|---|
| copies | 1 (user ↔ page cache) | 0 |
| caching, read-ahead, write-back | kernel does it | **you** do it |
| small/unaligned I/O | fine | `EINVAL` |
| effect on other data in cache | evicts it | none |
| typical users | everything | databases with their own cache, video recorders, `dd oflag=direct` |

**Alignment rules.** Buffer address, file offset and length must be
multiples of the device's logical block size (512 or 4096), or, on newer
kernels, the buffer only needs the controller's DMA alignment. Since Linux
6.1, `statx(STATX_DIOALIGN)` tells you for a given file: on this host's NVMe
ext4 it says *memory alignment 4, offset alignment 512*. Ubuntu 22.04's
headers do not have the field names yet; `c/03` reads them by ABI offset.
Allocate with `posix_memalign(&p, 4096, n)` or `aligned_alloc`.

**Filesystem support.** ext4, XFS, btrfs: yes. tmpfs: only since 6.6.
Some FUSE/overlay setups: no. `open(O_DIRECT)` then fails with `EINVAL`, and
`c/03` explains and falls back.

`c/03` also uses `mincore()` on a mapping to count page-cache-resident pages:
1024/1024 after a buffered write, 0 after `posix_fadvise(DONTNEED)` (once
the pages are clean, after `fsync`), 0 after an O_DIRECT read or write.
`posix_fadvise(POSIX_FADV_DONTNEED)` is the gentle alternative to O_DIRECT
for "stream this, don't cache it" (a recorder writing hours of video).

### 3.5 mmap for file I/O, and when it loses (`c/04_mmap_vs_read.c`)

`mmap` puts the file's page-cache pages into your address space. No copy
into a buffer. But:

- **Page faults.** The first touch of each page faults; the kernel maps up
  to 16 already-cached neighbours per fault (**fault-around**), so an 8 MiB
  file costs 128 faults (`c/04`, `SAN=0`). `MAP_POPULATE` takes the same
  faults *inside* `mmap()` (useful before an RT loop, see
  [15](../15-scheduling-and-real-time/README.md)), `madvise(MADV_SEQUENTIAL)`
  makes read-ahead more aggressive, `MADV_WILLNEED` starts it now.
- **Setup and teardown.** `mmap` + `munmap` create and destroy a VMA, and
  `munmap` needs a TLB flush (an IPI to every CPU running the process). For
  300 files of 2 KB, open+read+close beats open+fstat+mmap+munmap+close
  (`c/04`: 2.0 vs 3.4 ms, 0 vs 300 faults).
- **SIGBUS.** If the file is truncated while mapped, touching the lost pages
  kills you with `SIGBUS`. Shared-memory receivers seal their memfds for this
  reason ([11 §3.3](../11-ipc-shared-memory/README.md)).
- **Errors.** An I/O error during a page fault is also a `SIGBUS`, not an
  error code.
- **32-bit address space** (not an issue on the A72's arm64, but on
  Cortex-A9/A8 boards): you cannot map a 4 GB file.

Rule of thumb: **mmap for big files accessed randomly or repeatedly, or
shared between processes; `read()` for streaming and for small files.**

### 3.6 MSG_ZEROCOPY (`c/05_msg_zerocopy.c`)

```
 setsockopt(fd, SOL_SOCKET, SO_ZEROCOPY, &one)      once per socket
 send(fd, buf, len, MSG_ZEROCOPY)  -> returns; pages PINNED, buf is in flight
 ... do not touch buf ...
 poll(fd) -> POLLERR                               notification ready
 recvmsg(fd, MSG_ERRQUEUE) -> cmsg IP_RECVERR -> sock_extended_err
     ee_origin = SO_EE_ORIGIN_ZEROCOPY, sends [ee_info .. ee_data] are done
     ee_code & SO_EE_CODE_ZEROCOPY_COPIED  -> the kernel copied after all
 now buf may be reused or freed
```

- Each `send` call gets a sequence number (0, 1, 2...); notifications cover
  ranges and are coalesced.
- `ENOBUFS` = too many outstanding notifications (`net.core.optmem_max`):
  drain the error queue and retry.
- Worth it only for large sends (~10 KB+) on a NIC with scatter-gather.
  Over loopback the kernel always copies (`c/05` gets 32 of 32 flagged
  `COPIED`). The demo is about the protocol and the lifetime rule.
- Receive side: `TCP_ZEROCOPY_RECEIVE` (mmap of received pages) and io_uring
  zero-copy receive (6.15+) exist, but need page-aligned payloads and
  hardware header split.

### 3.7 Async I/O models (`c/06_posix_aio.c`)

| Model | How you wait | Works for | Notes |
|---|---|---|---|
| blocking | the call sleeps | everything | one thread per outstanding request |
| non-blocking + readiness (`epoll`) | "fd is readable/writable now" | sockets, pipes, ttys, eventfd, timerfd | regular files are *always* "ready", so epoll does not help disk I/O ([13](../13-signals-eventfd-and-epoll/README.md)) |
| POSIX AIO (`aio_read`, `lio_listio`) | `aio_suspend`, signal, or thread callback | files | **glibc emulates it with helper threads** doing blocking `pread`/`pwrite` |
| Linux native AIO (`io_submit`, libaio) | `io_getevents` | only `O_DIRECT` files, really | legacy; blocks in many cases; replaced by io_uring |
| **io_uring** | completion queue in shared memory | files, sockets, almost every syscall | kernel-native, batched, 5.1+ |

POSIX AIO in glibc (`c/06`): `aio_read()` queues the request for a helper
thread. Requests on the **same fd** go to the same thread, in order. The
program counts `/proc/self/task` before and after submitting 8 reads on one
fd: 1 → 2 threads. It is portable and convenient, but it is a thread pool,
not kernel async I/O. Rules: keep the `struct aiocb` and the buffer alive
until `aio_return()`, call `aio_return()` exactly once per request, and use
`SIGEV_NONE` + `aio_suspend` (a `SIGEV_THREAD` callback runs in a thread
glibc creates, invisible to TSan, as in [10](../10-ipc-message-queues/README.md)).

### 3.8 io_uring (`c/07_io_uring_raw.c`, `cpp/02_io_uring_wrapper.cpp`)

```
          user space                         kernel
  +-----------------------------+   shared   +----------------------------+
  | SQ ring: head tail mask     |<---mmap--->| consumes SQEs from head    |
  |   array[] -> SQE indices    |            |                            |
  | SQEs: {op,fd,addr,len,off,  |            | runs each request (inline  |
  |        user_data}           |            | if cached, else async)     |
  | CQ ring: head tail mask     |<---mmap--->| produces CQEs at tail      |
  |   CQEs: {user_data,res,flags}|           |                            |
  +-----------------------------+            +----------------------------+
   1 fill SQE  2 store_release(sq_tail)  3 io_uring_enter(fd, to_submit,
   min_complete, GETEVENTS)  4 load_acquire(cq_tail), read CQEs,
   store_release(cq_head)
```

Three syscalls, all via `syscall()` because glibc has no wrappers:

1. `io_uring_setup(entries, &params)` returns a ring fd and the offsets of
   every field.
2. `mmap` the SQ ring, the CQ ring (one mapping with
   `IORING_FEAT_SINGLE_MMAP`, 5.4+) and the SQE array.
3. `io_uring_enter(fd, to_submit, min_complete, flags)` submits and/or
   waits in **one** call.

Each ring is SPSC with the kernel on the other end: the producer writes the
entry, then publishes the index with a release store; the consumer loads
the index with acquire. That is exactly the lock-free ring of
[dsa/07 §3.8](../../dsa/07-stacks-and-queues/README.md).
`res` in a CQE is the syscall's return value: bytes or `-errno`. A short read
at EOF is a normal `res < len` (`cpp/02` checks it).

`c/07`: a NOP round trip, a batch of 4 reads in one syscall (completing in
whatever order), the whole 4 MiB file as 32 reads at queue depth 8 in **4**
`io_uring_enter` calls instead of 32 `read`s, and `IORING_REGISTER_EVENTFD`
so an epoll loop can wait on io_uring completions together with sockets.

More features: `IORING_SETUP_SQPOLL` (a kernel thread polls the SQ, no
syscall at all while busy), registered files and buffers (skip per-request
fd lookup and page pinning), linked requests (`IOSQE_IO_LINK`), multishot
accept/recv, `IORING_OP_SEND_ZC`. In production use **liburing**; the raw
version is for understanding.

**When it is disabled:** `sysctl kernel.io_uring_disabled` (6.6+:
0 = on, 1 = only for members of `kernel.io_uring_group`, 2 = off), seccomp
filters (Docker's default profile blocks it), Android, ChromeOS. Its large
attack surface is the reason. Both programs explain and skip on `EPERM` /
`ENOSYS`.

### 3.9 DMA-BUF and DMA heaps (`c/08_dma_heap.c`)

The zero-copy that matters most on the TDA4VM is between **devices**. A
DMA-BUF is a buffer represented by a **file descriptor**:

```
  CSI-2 camera (V4L2)          VPAC/MSC scaler            video encoder
  VIDIOC_EXPBUF --fd--> import fd (DMABUF) --fd--> import fd   --> bitstream
        \_________________ same physical memory, no CPU copy _____/
                                  |
                       mmap(fd) + DMA_BUF_IOCTL_SYNC  (only if the CPU must look)
```

- **Exporters** allocate and own the memory: V4L2 (`VIDIOC_EXPBUF`), DRM
  (PRIME), the GPU, or a **DMA heap**: `open("/dev/dma_heap/system")` +
  `ioctl(DMA_HEAP_IOCTL_ALLOC, {len, fd_flags})` returns a dmabuf fd.
  `system` = ordinary pages (scattered, needs an IOMMU or scatter-gather
  DMA); a CMA heap = physically contiguous memory for devices without one.
- **Importers** (another driver) attach and get a DMA address
  (`dma_buf_attach` / `dma_buf_map_attachment` in the kernel, see
  [linux-device-drivers/08-dma-and-mmap](../../linux-device-drivers/08-dma-and-mmap/README.md)).
- **CPU access**: `mmap` the fd and bracket every access with
  `DMA_BUF_IOCTL_SYNC` `START`/`END` (`READ`/`WRITE`). On non-coherent
  devices that is when caches are cleaned or invalidated. Forgetting it
  gives stale pixels or a half-written frame on the A72, never on x86.
- **Sharing between processes**: pass the fd over a Unix socket with
  `SCM_RIGHTS` ([12](../12-ipc-unix-sockets/README.md)); Wayland and
  PipeWire do exactly that.
- **Lifetime**: the buffer lives while any fd, mapping or device attachment
  refers to it. `DMA_BUF_SET_NAME` labels it for `/proc/<pid>/fdinfo` and
  `/sys/kernel/debug/dma_buf/bufinfo`, which helps when hunting leaks.

On a desktop `/dev/dma_heap/system` is mode 0600 root, so `c/08` lists the
heaps, explains and skips (it also tries `/dev/udmabuf`, which turns a
sealed memfd into a dmabuf). On the board as root it allocates 1 MiB, names
it, maps it, writes it between `SYNC_START`/`SYNC_END`, and has a forked
child map the same fd and verify every word.

**User-space pointer vs physically contiguous buffer.** A `malloc`ed buffer
is virtually contiguous but physically scattered, and its pages can move or
be swapped. A device without an IOMMU needs physically contiguous, pinned
memory with a bus address. That is what CMA heaps, `dma_alloc_coherent` and
the TI-specific `/dev/dma_heap/carveout_*` heaps provide. Drivers that accept
user pointers (`V4L2_MEMORY_USERPTR`) must pin the pages and build a
scatter-gather list. Prefer DMA-BUF.

**GStreamer** wraps all this: `GstDmaBufMemory` (`gst_dmabuf_allocator`),
`v4l2src io-mode=dmabuf` (export) and `io-mode=dmabuf-import`, `GstVideoMeta`
for strides, and buffer pools that recycle dmabufs. See
[gstreamer/13-performance-zero-copy](../../gstreamer/13-performance-zero-copy/README.md)
for buffer/memory anatomy and pools, and
[gstreamer/14-embedded-tda4vm](../../gstreamer/14-embedded-tda4vm/README.md)
for the camera → TIOVX → encoder pipelines on the board.

---

## 4. Complexity table

| Operation | Data copies by the CPU | Syscalls for N bytes (chunk C) | Extra cost |
|---|---|---|---|
| `read` + `write` | 2N | 2N/C | cache pollution |
| `mmap` + `write` | N | 1 + N/C | N/(16×4 KiB) page faults, VMA + TLB flush |
| `sendfile` / `copy_file_range` | 0 (sender side) | ~1 | page refs; reflink O(extents) |
| `splice` | 0 | 2N/C (pipe size) | pipe allocation |
| `O_DIRECT` read | 0 | N/C | every request hits the device; alignment |
| `MSG_ZEROCOPY` send | 0 (if NIC can) | N/C + notifications | page pinning, error-queue reads |
| POSIX AIO (glibc) | as `pread` | 1 per request + thread handoff | helper threads, context switches |
| io_uring, batch of B | as the op | N/(C·B) | ring memory; 0 syscalls with SQPOLL |
| DMA-BUF between devices | 0 | a few ioctls to set up | cache maintenance on non-coherent SoCs |
| `mincore` | — | 1 | O(pages) vector |

---

## 5. C vs C++

| Concept | C | C++17 |
|---|---|---|
| Mapped file | `open/fstat/mmap/close` + `munmap` on every path | `MappedFile` RAII, move-only (`cpp/01`) |
| View of bytes | `const char *` + length | `std::string_view`, a span-like `ByteView` (`std::span` is C++20) |
| Read loop with partial lines | manual carry-over buffer | same logic, but `std::string` owns the carry (`cpp/01` `BufferedReader`) |
| io_uring | raw syscalls + struct offsets (`c/07`) | `IoUring` class: `create()` → `std::optional`, `prep_read`, `submit`, `for_each_completion(lambda)` (`cpp/02`) |
| Shared ring indices | `__atomic_load_n(..., __ATOMIC_ACQUIRE)` | same builtins (`std::atomic_ref` is C++20) |
| Errors | `errno`, `-errno` in CQE `res` | `std::system_error` in constructors, `std::optional` + error code where failure is expected |
| Aligned buffer for O_DIRECT | `posix_memalign` | `std::aligned_alloc` or `operator new(n, std::align_val_t{4096})` |

`std::filesystem::copy_file` in libstdc++ uses `sendfile` (and newer
versions `copy_file_range`) under the hood. `std::ifstream` is buffered
`read()`.

---

## 6. Where it is used in the real world

- **Web and file servers.** nginx `sendfile on; tcp_nopush on;`, Apache
  `EnableSendfile`, Samba, NFS server-side copy via `copy_file_range`.
- **Kafka** serves consumers straight from the page cache with `sendfile`;
  that is a large part of its throughput.
- **Databases.** PostgreSQL 16+ can use `O_DIRECT` (`debug_io_direct`) and
  18 adds asynchronous I/O with io_uring, MySQL InnoDB `O_DIRECT`, RocksDB io_uring
  `MultiRead`, ScyllaDB/Seastar O_DIRECT + AIO/io_uring.
- **Storage tools.** `fio --ioengine=io_uring`, `cp` (`copy_file_range`),
  `dd iflag=direct`, QEMU (io_uring disk backend, udmabuf for virtio-gpu).
- **Network.** `MSG_ZEROCOPY` in Google's internal RPC and in high-rate
  video streaming servers; AF_XDP for kernel-bypass packet I/O.
- **Graphics and video.** Wayland (`zwp_linux_dmabuf_v1`), PipeWire,
  Chrome/ffmpeg VAAPI, Android gralloc (`ion` → DMA heaps) all pass dmabuf
  fds.
- **TDA4VM board.** The camera (CSI-2 RX → V4L2), VPAC (MSC scaler, LDC),
  the GPU, the video decoder/encoder and the display (DSS) exchange frames as
  dmabufs. TI's OpenVX/TIOVX and the GStreamer `tiovx*` elements allocate
  them from `/dev/dma_heap/carveout_*` heaps reserved in the device tree.
  The A72 should only touch pixels when it really must (and then with
  `DMA_BUF_IOCTL_SYNC`).

---

## 7. Common mistakes and pitfalls

1. **Assuming `write()` means "on disk".** It means "in the page cache".
   Use `fsync`/`fdatasync`, even with `O_DIRECT` (device cache, metadata).
2. **Ignoring short reads/writes/sendfile results.** All of them may
   transfer less than asked (signals, socket buffers, EOF). Loop.
3. **O_DIRECT with a `malloc` buffer or an odd offset.** `EINVAL`. Ask
   `statx(STATX_DIOALIGN)`; use `posix_memalign`.
4. **Mixing buffered and O_DIRECT access to the same file** without care:
   the kernel flushes/invalidates the cache around direct I/O, but a
   concurrent `mmap` writer can still give you stale data.
5. **mmap for many small files** (slower) or for a file another process may
   truncate (`SIGBUS`). Seal memfds, handle `SIGBUS`, or use `read`.
6. **Touching a `MSG_ZEROCOPY` buffer before its completion notification**,
   or never reading the error queue (eventually `ENOBUFS`).
7. **Expecting POSIX AIO to be kernel async I/O on Linux.** It is a glibc
   thread pool; requests on one fd are serialised.
8. **Freeing or reusing an `aiocb`/buffer before `aio_return()`**, or
   calling `aio_return()` twice.
9. **io_uring ring indices without acquire/release.** On the A72 the
   kernel can see the new tail before the SQE contents. Use the barriers
   (or liburing).
10. **Letting a buffer used by an in-flight io_uring request go out of
    scope.** The kernel writes into it later. The buffer must live until its
    CQE is reaped.
11. **Assuming io_uring exists.** Containers, Android and hardened
    distributions disable it. Always have a fallback.
12. **CPU access to a dmabuf without `DMA_BUF_IOCTL_SYNC`.** Works on x86
    (coherent), gives corrupted frames on ARM SoCs with non-coherent
    devices.
13. **Benchmarking with sanitizers.** ASan/TSan instrument user-space
    copies (and TSan intercepts `read`/`write` ranges), which inflates
    user-copy methods. They also add shadow-memory page faults to `getrusage`
    counts. For numbers use `make SAN=0 OPT=1`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_copy_file_to_file.c](c/01_copy_file_to_file.c) | read/write vs mmap+write vs sendfile vs copy_file_range vs splice, 8 MiB, checksums asserted | `make run P=linux-internals/16-zero-copy-and-async-io/c/01_copy_file_to_file.c` |
| [c/02_file_to_socket.c](c/02_file_to_socket.c) | file → AF_UNIX socket four ways, receiver thread, every byte verified | `make run P=linux-internals/16-zero-copy-and-async-io/c/02_file_to_socket.c` |
| [c/03_direct_io.c](c/03_direct_io.c) | `statx(STATX_DIOALIGN)`, misaligned buffer/offset → EINVAL, aligned O_DIRECT read/write, `mincore` residency, `fadvise(DONTNEED)`, EINVAL fallback | `make run P=linux-internals/16-zero-copy-and-async-io/c/03_direct_io.c` |
| [c/04_mmap_vs_read.c](c/04_mmap_vs_read.c) | big file: read vs lazy mmap vs MAP_POPULATE vs MADV_SEQUENTIAL with fault counts; 300 small files: read wins | `make run P=linux-internals/16-zero-copy-and-async-io/c/04_mmap_vs_read.c` |
| [c/05_msg_zerocopy.c](c/05_msg_zerocopy.c) | `SO_ZEROCOPY`, `send(MSG_ZEROCOPY)` over TCP loopback, error-queue completions and the `COPIED` flag | `make run P=linux-internals/16-zero-copy-and-async-io/c/05_msg_zerocopy.c` |
| [c/06_posix_aio.c](c/06_posix_aio.c) | 8 concurrent `aio_read` + `aio_suspend`/`aio_return`, `lio_listio(LIO_WAIT)`, `aio_fsync`, glibc helper threads counted | `make run P=linux-internals/16-zero-copy-and-async-io/c/06_posix_aio.c` |
| [c/07_io_uring_raw.c](c/07_io_uring_raw.c) | io_uring from raw syscalls: setup + mmaps, NOP, batched READs, queue-depth loop, eventfd registration, disabled → skip | `make run P=linux-internals/16-zero-copy-and-async-io/c/07_io_uring_raw.c` |
| [c/08_dma_heap.c](c/08_dma_heap.c) | list DMA heaps; allocate from `/dev/dma_heap/system` or udmabuf, name, mmap, `DMA_BUF_IOCTL_SYNC`, share with a child; explains and skips without access | `make run P=linux-internals/16-zero-copy-and-async-io/c/08_dma_heap.c` |
| [cpp/01_mapped_file.cpp](cpp/01_mapped_file.cpp) | `MappedFile` RAII (string_view / ByteView access, move, empty file, errors) vs `BufferedReader` with split-line carry-over | `make run P=linux-internals/16-zero-copy-and-async-io/cpp/01_mapped_file.cpp` |
| [cpp/02_io_uring_wrapper.cpp](cpp/02_io_uring_wrapper.cpp) | `IoUring` RAII class over raw syscalls: optional factory, prep/submit/for_each_completion, depth-16 file read with a short read at EOF | `make run P=linux-internals/16-zero-copy-and-async-io/cpp/02_io_uring_wrapper.cpp` |

Every program is also clean under ThreadSanitizer. Add `SAN=tsan`:

```bash
make run P=linux-internals/16-zero-copy-and-async-io/c/07_io_uring_raw.c SAN=tsan
```

For meaningful throughput numbers build without sanitizers:

```bash
make SAN=0 OPT=1 build/linux-internals/16-zero-copy-and-async-io/c/01_copy_file_to_file
./build/linux-internals/16-zero-copy-and-async-io/c/01_copy_file_to_file
```

---

## 9. Exercises

**Easy**
1. In `c/01`, change the chunk size of read/write from 64 KiB to 4 KiB and
   to 1 MiB (with `SAN=0 OPT=1`). Where does the curve flatten, and why?
2. Run `c/01` with the source on tmpfs (`/dev/shm`) and on the SD card on
   the board. Which methods change most?
3. In `c/04`, read the big file twice through the same mapping. How many
   faults does the second pass take?

**Medium**
4. Add `fsync()` to every method in `c/01` and time it on the board's
   SD card or eMMC. What dominates now?
5. Make `c/02` use TCP loopback instead of AF_UNIX, then (on the board) a
   real connection to your PC with `sendfile` vs `read`/`write`. Watch CPU
   usage with `top` or `perf stat`.
6. Rewrite `c/06` with `SIGEV_SIGNAL` notifications collected by
   `sigwaitinfo` (as in [15](../15-scheduling-and-real-time/README.md) `c/07`).
7. Extend `cpp/02` with `prep_write` and copy a file with io_uring: chain a
   read and a write per block with `IOSQE_IO_LINK`.

**Hard**
8. Add `IORING_SETUP_SQPOLL` to `c/07` and count `io_uring_enter` calls
   while the kernel thread is awake. What does `sq_thread_idle` do?
9. Register the destination buffers (`IORING_REGISTER_BUFFERS`) and use
   `IORING_OP_READ_FIXED`. Measure with `O_DIRECT` on the board's eMMC.
10. On the board as root: allocate a dmabuf from a CMA or carveout heap in
    `c/08`, then import it into V4L2 (`V4L2_MEMORY_DMABUF`) for a capture
    with `v4l2-ctl` semantics, or into a GStreamer `appsrc` as
    `GstDmaBufMemory`.

---

## 10. Remember forever

```
read+write = 2 CPU copies + 2 syscalls/chunk; every copy = bandwidth x2 + cache pollution
sendfile(sock,file) / copy_file_range / splice (one end a pipe): data stays in the kernel
mmap: no copy, but faults (16 pages each), VMA+TLB cost, SIGBUS on truncate; loses on small files
O_DIRECT: bypass page cache; align buf/offset/len (statx STATX_DIOALIGN); still fdatasync
MSG_ZEROCOPY: buffer pinned until the MSG_ERRQUEUE notification; only for big sends + real NIC
epoll = readiness (sockets), useless for files; POSIX AIO = glibc threads; io_uring = native
io_uring: SQ/CQ rings in shared memory, release-publish tail, acquire-read tail; batch per enter
DMA-BUF = buffer as an fd shared by devices/processes; mmap + DMA_BUF_IOCTL_SYNC for CPU access
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Draw the data path of `read()` + `write()` from a file to a socket. Where
   are the CPU copies, and where is DMA?
2. What does `sendfile` save compared with read/write? Why is the gain small
   over AF_UNIX or loopback?
3. What can `copy_file_range` do on btrfs or XFS that `sendfile` cannot?
4. Why must one end of `splice` be a pipe? What does the pipe contain?
5. What are the three alignment requirements of O_DIRECT, and how do you
   find out the right values on Linux 6.1+?
6. Why do databases use O_DIRECT? What do they give up?
7. When does `mmap` beat `read`, and when does it lose? What is
   fault-around? What does `MAP_POPULATE` change?
8. What happens if a mapped file is truncated by another process?
9. Walk through the MSG_ZEROCOPY protocol. When may you reuse the buffer?
   What does `SO_EE_CODE_ZEROCOPY_COPIED` mean?
10. Why does epoll not help with regular-file I/O?
11. How does glibc implement POSIX AIO, and what does that mean for
    performance and for requests on the same fd?
12. Name the three io_uring syscalls and the three mmaps. Why does the
    producer use a release store on the tail?
13. Why might `io_uring_setup` fail with `EPERM` on a perfectly modern
    kernel?
14. What is a DMA-BUF, who exports and who imports, and why is
    `DMA_BUF_IOCTL_SYNC` needed on the TDA4VM but not on x86?
15. Why can a device without an IOMMU not DMA into a `malloc`ed buffer?
