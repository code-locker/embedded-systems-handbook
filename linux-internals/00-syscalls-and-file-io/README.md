# 00 — System Calls and File I/O

> Everything a Linux program does beyond pure computation, such as printing,
> reading a sensor, sending a packet or starting a thread, is a request to
> the kernel: a **system call**. The most common target of those requests is a
> **file descriptor**, because Linux shows almost everything as a file:
> regular files, pipes, sockets, terminals, `/dev/i2c-1`, `/proc/self/status`.
> This lesson covers the boundary between your program and the kernel. Every
> later lesson in this track sits on top of it.

---

## 1. Why this exists

A user program cannot be trusted with the hardware. If any process could
program the SD controller, change page tables or turn off interrupts, one
bug would bring the whole board down and nothing would be private. So the
CPU has **privilege levels**, and only the kernel runs at the privileged one.
A program that needs something done asks the kernel through one narrow,
checked doorway: the system call.

Files are the second idea. Unix gives most resources one interface (`open`,
`read`, `write`, `close`) so that one set of tools works on all of them:
`cat`, shell redirection, `poll`, `strace`. A camera driver, a UART and a log
file are all used the same way. Device-specific extras go through `ioctl`.

On an embedded board this matters more than on a PC:

| Problem on the board | What you need from this lesson |
|---|---|
| config file corrupted after a power cut | the page cache, `fsync`, atomic rename (§3.9) |
| daemon dies after 3 days with "Too many open files" | fd ownership, `close`, RAII (§3.4, `c/08`, `cpp/01`) |
| logs missing the last lines before a crash | stdio buffering (§3.8) |
| a serial protocol loses bytes | short reads, `read_full` (§3.6) |
| two processes corrupt one data file | file locking (§3.11) |
| "how do I talk to my driver?" | `read`/`write`/`ioctl`, `/sys` (§3.12, §3.13) |
| "why is this loop slow?" | syscall cost, the vDSO, `strace -c` (§3.2, §3.14) |

---

## 2. Mental model

**The kernel is a bank teller behind glass.** You (user space) cannot walk
into the vault. You fill in a form (syscall number + arguments in
registers), push it through the slot (`svc #0`), the teller checks it, does
the work, and pushes back a result (a number, or a negative error code). A
**file descriptor** is the numbered ticket the teller gave you when you
opened an account. You show the ticket, never the account itself.

```
              EL0 / ring 3  (user mode)                  EL1 / ring 0  (kernel mode)
  +-----------------------------------------+    +------------------------------------------+
  | your code: write(fd, buf, n)             |    |                                          |
  |   -> glibc write():                      |    |  el0_svc / entry_SYSCALL_64              |
  |        x8 = 64 (__NR_write, arm64)       |    |    -> sys_call_table[x8]                 |
  |        x0 = fd, x1 = buf, x2 = n         |    |    -> ksys_write(fd, buf, n)             |
  |        svc #0  ------------------------- exception -->  fdget(fd) -> struct file          |
  |                                          |    |       -> file->f_op->write_iter()        |
  |        x0 = result  <------------------- eret ------    (ext4, pipe, tty, your driver)   |
  |      if (x0 < 0) { errno = -x0; ret -1 } |    |                                          |
  +-----------------------------------------+    +------------------------------------------+
     the vDSO page (kernel code mapped into user space) answers clock_gettime without
     entering the kernel at all
```

ARM64 has four exception levels: **EL0** applications, **EL1** the kernel,
**EL2** a hypervisor, **EL3** secure firmware (TF-A on the TDA4VM). x86 calls
the same idea rings: **ring 3** user, **ring 0** kernel. Remember this
sentence: **a system call is a controlled exception into EL1, and everything
the kernel gives back is a number in a register.**

---

## 3. How it works

### 3.1 User space, kernel space, system calls (`c/01_syscalls_and_vdso.c`)

A system call is one instruction plus a register convention:

| | ARM64 (BeagleBone AI-64) | x86-64 (host PC) |
|---|---|---|
| instruction | `svc #0` | `syscall` |
| number in | `x8` | `rax` |
| arguments | `x0`–`x5` | `rdi rsi rdx r10 r8 r9` |
| result in | `x0` | `rax` |
| `getpid` / `write` / `openat` | 172 / 64 / 56 | 39 / 1 / 257 |
| table in the kernel | `include/uapi/asm-generic/unistd.h`, `scripts/syscall.tbl` in 6.11+ (generic table) | `arch/x86/entry/syscalls/syscall_64.tbl` |

The numbers are different on every architecture. arm64 uses the newer
"generic" table, which has no `open()` at all, only `openat()`. glibc hides
this. Three ways to make the same call, all in `c/01`:

```c
pid_t a = getpid();              /* libc wrapper: the normal way           */
long  b = syscall(SYS_getpid);   /* generic wrapper: for calls glibc lacks */
long  c = raw_getpid();          /* inline asm: svc #0 / syscall           */
```

`syscall(2)` is how you call things that glibc has no wrapper for, such as
`gettid` (before glibc 2.30), `futex`, `pidfd_open`, `io_uring_setup` and
`perf_event_open`.

What happens in between: the CPU saves the user PC and status, switches to the
kernel stack, jumps to the exception vector, the kernel checks the arguments
(it copies user memory with `copy_from_user`, so a bad pointer gives `EFAULT`,
not a kernel crash), does the work, and returns with `eret`. On a modern x86
with Spectre/Meltdown mitigations a trivial syscall costs ~100–300 ns. On a
Cortex-A72 it is a few hundred ns. Cheap, but not free inside a hot loop.

### 3.2 The vDSO

Some calls are needed so often and are so simple that entering the kernel
would be most of their cost: `clock_gettime`, `gettimeofday`, `time`,
`getcpu`. For these, the kernel maps a small ELF library, the **vDSO**
(virtual dynamic shared object), into every process, plus a read-only data page
(**vvar**) that it keeps up to date with the current time base. glibc calls
the vDSO function, which reads the vvar page and the CPU counter (`CNTVCT_EL0`
on ARM, `rdtsc` on x86) entirely in user mode.

```
 clock_gettime()  ->  [vdso] __kernel_clock_gettime  ->  read vvar + counter  -> done  (~20 ns)
 syscall(SYS_clock_gettime) -> svc #0 -> kernel -> eret                           (~150 ns)
```

`c/01` measures both and finds the vDSO address two ways: `getauxval(AT_SYSINFO_EHDR)`
(the kernel passes it to every new program in the auxiliary vector) and the
`[vdso]` line of `/proc/self/maps`. If you call
`syscall(SYS_clock_gettime, ...)` directly, you lose this speed.

### 3.3 errno and EINTR (`c/01`)

The kernel returns `-ENOENT` (−2) in `x0`. glibc turns any value from −4095
to −1 into "return −1, set `errno = 2`". The rules:

1. **Check the return value first, then errno.** errno is *not* cleared on
   success, and it may hold an old value. `c/01` shows that.
2. **Save errno immediately** (`int e = errno;`). `printf`, `free` and other calls in
   your error path can overwrite it.
3. errno is **thread-local**, so threads do not clobber each other.
4. `strerror(e)` gives the text, `perror("open")` prints `open: No such file...`.
5. pthread functions do *not* use errno: they *return* the error number.

**EINTR.** A "slow" call (`read` on a pipe/socket/tty, `accept`, `wait`,
`nanosleep`, `sem_wait`) that is blocked when a signal handler runs returns
−1 with `errno == EINTR`, unless the handler was installed with
`SA_RESTART` (and even then some calls, like `nanosleep`, `poll` and
`epoll_wait`, are never restarted). The fix is a retry loop:

```c
do r = read(fd, buf, n); while (r < 0 && errno == EINTR);
```

`c/01` has a child send `SIGUSR1` while the parent is blocked in `read()`.
Without `SA_RESTART` you see one EINTR and the loop recovers. With it, the
kernel restarts the read itself. Signals in depth:
[13](../13-signals-eventfd-and-epoll/README.md).

### 3.4 Everything is a file: fd → open file description → inode (`c/02`, `c/03`)

```
   process A                                  kernel (shared by all processes)
   fd table (files_struct)       open file descriptions          inodes / objects
   +----+------------------+     (struct file, refcounted)
   | 0  | -> /dev/pts/0    |---> [ tty   off=-  O_RDWR      ] --> [ tty device       ]
   | 1  | -> /dev/pts/0    |--/
   | 2  | -> /dev/pts/0    |-/
   | 3  | FD_CLOEXEC       |---> [ off=4096  O_RDONLY       ] --+
   | 4  |                  |---> [ off=0     O_WRONLY|APPEND] --+-> [ inode 1234: data.bin ]
   | 5  | (dup of 3)       |-+                                  |    size, mode, owner,
   +----+------------------+ +-> same description as fd 3  ----+    pages in page cache
   process B (after fork)
   | 3  |------------------------> same description as A's fd 3 (shared offset!)
```

Three levels, and **which level a property lives in explains every surprise**:

| Property | Lives in | Consequence |
|---|---|---|
| `FD_CLOEXEC` | fd table slot | `dup()` clears it. It is per fd |
| file offset, `O_APPEND`, `O_NONBLOCK`, status flags | open file description | `dup()`/`fork()` share it, a second `open()` does not |
| size, permissions, data, `flock` vs POSIX locks | inode / description (see §3.11) | the same for every opener |

- `open()` returns the **lowest free** fd number. That is how `close(1); open(file)`
  redirects stdout, and why `dup2` exists (§3.7).
- The kernel's fd → `struct file` → `f_op` (file operations) chain is what a
  driver plugs into:
  [linux-device-drivers/01](../../linux-device-drivers/01-char-devices-and-file-operations/README.md)
  implements `.read`, `.write` and `.unlocked_ioctl` for a character device.
- An fd is a resource. If you never close it, it is leaked until the process exits
  (`c/08_fd_leak_bug.c`).

### 3.5 open() flags (`c/02_open_read_write.c`)

| Flag | Meaning | When |
|---|---|---|
| `O_RDONLY` / `O_WRONLY` / `O_RDWR` | access mode | always one of them |
| `O_CREAT` (+ mode) | create if missing. The mode is filtered by `umask` | new files |
| `O_EXCL` (with `O_CREAT`) | fail with `EEXIST` if it exists. Check and create are **one atomic step** | lock files, pid files, "create once" |
| `O_TRUNC` | cut to length 0 on open | rewriting a file (but see §3.9 for safe rewrites) |
| `O_APPEND` | every `write()` seeks to the end **atomically** first | shared log files: writers never overwrite each other |
| `O_CLOEXEC` | set `FD_CLOEXEC`: the fd is closed by `exec()` | **always**, unless a child must inherit it |
| `O_NONBLOCK` | return `EAGAIN` instead of sleeping | pipes, sockets, ttys, devices in event loops ([13](../13-signals-eventfd-and-epoll/README.md)) |
| `O_SYNC` / `O_DSYNC` | every `write()` waits until the data is on the device | rare. Usually `fsync` at the right moments is better |
| `O_DIRECT` | bypass the page cache, DMA from your buffer. Buffer, length and offset must be block-aligned | databases, raw video capture. tmpfs refuses it |
| `O_PATH`, `O_DIRECTORY`, `O_NOFOLLOW`, `O_TMPFILE` | special cases | see `man 2 open` |

`c/02` shows each one: `EEXIST`, a sparse file (seek 1 MiB past the end and
write 1 byte: `st_size` is 1 MiB but only a few KiB are allocated), two
writers overwriting each other without `O_APPEND` (`"BBAA"`) and not with it
(`"AAAABBCC"`), the lowest-free-fd rule, `O_SYNC` timing, and `O_DIRECT`
rejecting a misaligned write with `EINVAL`.

`O_CLOEXEC` exists because `fcntl(fd, F_SETFD, FD_CLOEXEC)` after `open()`
leaves a window: another thread can `fork()+exec()` in between and leak the fd.
The same reasoning gives `pipe2(O_CLOEXEC)`, `socket(SOCK_CLOEXEC)`,
`accept4`, `dup3`, `F_DUPFD_CLOEXEC`.

### 3.6 Short reads and writes (`c/02`)

`read(fd, buf, 1000)` may return **anything from 0 to 1000**. So may
`write()`. That is not an error:

| Source | Why fewer bytes |
|---|---|
| pipe, socket, FIFO | only what is there now (read), only the free buffer space (non-blocking write) |
| terminal | one line at a time (canonical mode) |
| serial port / UART | whatever the driver has buffered |
| regular file | at EOF; a signal interrupted a large write; the disk filled up mid-write |
| any | a signal arrived after some bytes were copied (no EINTR then: you get the partial count) |

`read() == 0` means **EOF** (the writer closed the pipe, end of file), not
"try again". The two loops are in `c/02` and in every serious codebase:

```c
ssize_t read_full(int fd, void *buf, size_t n)   /* < n only at EOF          */
ssize_t write_full(int fd, const void *buf, size_t n)   /* == n or -1        */
```

`c/02` provokes both: a non-blocking 256 KiB write to a 64 KiB pipe returns
65536, and a child that asks for everything still needs several `read()`
calls.

### 3.7 dup, fcntl, pread/pwrite, readv/writev (`c/03_dup_fcntl_vectored.c`)

```
 dup(fd)              -> lowest free fd, same open file description
 dup2(old, new)       -> make `new` refer to old's description (closes new first, atomically)
 dup3(old, new, O_CLOEXEC)
 fcntl(fd, F_DUPFD_CLOEXEC, min)  -> lowest free fd >= min, with CLOEXEC
 fcntl(fd, F_GETFD/F_SETFD)       -> FD_CLOEXEC            (fd table)
 fcntl(fd, F_GETFL/F_SETFL)       -> O_APPEND, O_NONBLOCK  (description)
 pread(fd, buf, n, off)  pwrite(...)   -> explicit offset, shared offset untouched
 readv(fd, iov, cnt)     writev(...)   -> several buffers, one syscall
```

**Shell redirection is `dup2`.** `prog > out.txt` means: `fd = open("out.txt")`,
`dup2(fd, 1)`, `close(fd)`, `exec(prog)`. `c/03` redirects its own stdout into
a file and back. Call `fflush(stdout)` *before* moving fd 1, or buffered text
ends up in the wrong place.

**pread/pwrite** are the thread-safe way to do random access on a shared fd.
`lseek` + `read` from two threads is a race on the shared offset.
Databases and record files use them.

**writev** sends a header and a payload from two places in memory as one
contiguous write: no `malloc` + `memcpy` into a temp buffer, and on a socket
or pipe the data is not split between two writes. GStreamer, network stacks
and loggers use it.

### 3.8 stdio buffering vs raw syscalls (`c/04_stdio_buffering.c`)

`FILE *` (and `std::ostream`) is a **user-space buffer** in front of `write(2)`:

```
 printf("x=%d\n", x)  ->  [ FILE buffer (4 KiB) ] --when?--> write(1, buf, n)  -> kernel
                                                    full / '\n' / fflush / fclose / exit()
```

| Mode | Set by | Flushes when | Default for |
|---|---|---|---|
| fully buffered `_IOFBF` | `setvbuf` | buffer full | files, **pipes**, sockets |
| line buffered `_IOLBF` | `setvbuf` | `'\n'` or full | stdout **when it is a terminal** |
| unbuffered `_IONBF` | `setvbuf` | every call | stderr |

`c/04` counts the real write calls using `fopencookie` (a FILE whose
low-level write is your own function): 1000 `fprintf` calls cost **2**
writes when fully buffered, **1000** when line buffered, **≥1000**
unbuffered. Writing 20000 single bytes takes ~60 ms with `write()` and under
1 ms with `fputc()`.

Two classic pitfalls, both reproduced in `c/04`:

1. **Output printed twice after `fork()`.** Unflushed text in the buffer is
   copied into the child, and both processes flush it. It "works" on the
   terminal (line buffered, already flushed at `\n`) and duplicates when you
   run `./prog | tee log` (pipe = fully buffered). Fix: `fflush(NULL)` before
   `fork()`, and `_exit()` (not `exit()`) in a child that does not exec.
2. **Last lines missing after a crash.** `_exit()`, `abort()`, a segfault
   or `kill -9` never flush. The buffer, holding exactly the last log
   lines before the crash, is lost. For logs use stderr, line buffering, or
   `fflush` after important lines. On the board `journald`/`syslog()` sends
   each message right away.

### 3.9 The page cache and fsync (`c/05_fsync_stat_dirs.c`)

```
  write() ─┐
           v
   [ page cache: dirty pages in RAM ] ── kernel flusher threads, after ~5–30 s ──> [ SD / eMMC ]
           ^ write() returns HERE              fsync(fd) waits until this is done ^
```

`write()` copies into the page cache and returns. The data reaches the
device later (`vm.dirty_expire_centisecs`, default 30 s;
`vm.dirty_writeback_centisecs`, 5 s). Power loss before then means the data
is gone. Worse, with `O_TRUNC` + write you can lose **both** versions: the
truncate reached the disk, the new data did not.

| Call | Waits for |
|---|---|
| `fsync(fd)` | file data + all metadata (size, mtime) on the device, and a device cache flush |
| `fdatasync(fd)` | data + only the metadata needed to read it back (size yes, mtime no). Cheaper |
| `sync()` / `syncfs(fd)` | everything / one filesystem |
| `O_SYNC` / `O_DSYNC` | like fsync / fdatasync after every write |
| `msync(MS_SYNC)` | the same for an `mmap`'d file ([11](../11-ipc-shared-memory/README.md), [14](../14-virtual-memory/README.md)) |

**The atomic-replace pattern** (used for every config file on an embedded
device):

```c
fd = open("app.conf.tmp", O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC, 0644);
write_full(fd, new, len);
fsync(fd);  close(fd);                 /* 1. new bytes durable              */
rename("app.conf.tmp", "app.conf");    /* 2. atomic switch of the name      */
dfd = open(".", O_RDONLY|O_DIRECTORY); fsync(dfd);   /* 3. the rename durable */
```

After a power cut you see the complete old file or the complete new one,
never half of each. SD cards and cheap eMMC can still lie about flushing
their internal cache, so for critical data use a journaling filesystem
(ext4 with `data=ordered`, f2fs) and a good card, or a raw-flash filesystem
(UBIFS) on NAND.

### 3.10 stat and directories (`c/05`)

`stat(path)`, `fstat(fd)` and `lstat(path)` (does not follow a symlink) fill
`struct stat` from the **inode**: `st_ino`, `st_mode` (type bits +
permissions, test the type with `S_ISREG`/`S_ISDIR`/`S_ISLNK`), `st_nlink`,
`st_uid`, `st_size`, `st_blocks` (512-byte units actually allocated), and
`st_mtim`.

- A **directory** is a table of `name → inode number`. A **hard link** is a second
  name for the same inode (`nlink` = 2). A **symlink** is a tiny file that
  contains a path.
- `unlink()` removes a **name**. The inode and its data are freed only when
  `nlink == 0` *and* no fd has it open. `c/05` reads from a file after
  unlinking both its names. This is how `O_TMPFILE` and "delete the temp file
  right after opening it" work.
- `opendir`/`readdir`/`closedir`: entries come in **no particular order**,
  include `.` and `..`, and `d_type` may be `DT_UNKNOWN` on some
  filesystems (then call `lstat`). At the end `readdir` returns NULL with errno
  unchanged. NULL with errno set means an error.

### 3.11 File locking (`c/06_file_locking.c`)

All three kinds are **advisory**. They only stop processes that also take the
lock. `read()`/`write()` ignore them.

| | `flock(fd, LOCK_EX)` | `fcntl(F_SETLK)` POSIX | `fcntl(F_OFD_SETLK)` OFD |
|---|---|---|---|
| range | whole file | byte range | byte range |
| owner | open file description | **(process, inode)** | open file description |
| released by | last close of that description | **any close of any fd** for that file in the process, or exit | last close of that description |
| conflicts within one process | yes, between different `open()`s | **never** | yes |
| blocking form | `LOCK_EX` (without `LOCK_NB`) | `F_SETLKW` | `F_OFD_SETLKW` |
| "who holds it?" | — | `F_GETLK` gives the pid | `F_OFD_GETLK` (pid = −1) |

The **POSIX trap**, shown in `c/06`: you lock `data.db` with fd 3, then a
library opens and closes `data.db` to read a header, and your lock is gone,
silently. OFD locks (Linux 3.15+) fix this and also work between threads
that each open the file. Use OFD locks for new code. Use `flock` for
whole-file "only one instance" locks (`flock -n /run/lock/app.lock app` in
shell). Mandatory locking was removed from Linux in 5.15.

### 3.12 ioctl (`c/07_ioctl_proc_sys.c`)

`read`/`write` move bytes. Everything else a device can do goes through
`ioctl(fd, request, arg)`: set the UART baud rate (`TCSETS`), ask how many
bytes are queued (`FIONREAD`), get the terminal size (`TIOCGWINSZ`), set an
I2C slave address (`I2C_SLAVE`), queue a V4L2 buffer (`VIDIOC_QBUF`), map a GPU
buffer.

The request number is a packed descriptor built with `_IO/_IOR/_IOW/_IOWR`:

```
  31 30 | 29 ............ 16 | 15 ...... 8 | 7 ...... 0
  dir   |  size of the arg   |  type 'f'   |   nr
  FS_IOC_GETFLAGS = _IOR('f', 1, long) = 0x80086601   (decoded by c/07)
```

The kernel calls the driver's `.unlocked_ioctl(file, cmd, arg)`. An
unknown request gives `ENOTTY` ("inappropriate ioctl for device"), which is what
`TIOCGWINSZ` returns when stdout is a pipe. **Why drivers expose ioctls:** they
carry typed, structured commands that do not fit a byte stream, and the size
encoded in the number lets the kernel check and copy the argument.
Prefer `sysfs` attributes for simple single values (§3.13). You will write
one in [linux-device-drivers/01](../../linux-device-drivers/01-char-devices-and-file-operations/README.md).

### 3.13 /proc and /sys (`c/07`)

Neither is on disk. Reading a file calls a kernel function that formats the
answer at that moment.

| Path | What |
|---|---|
| `/proc/self/status` | name, pid, ppid, threads, `VmRSS`, signal masks, capabilities |
| `/proc/self/fd/` | your fd table: one symlink per fd (`3 -> /tmp/x`, `5 -> pipe:[1234]`) |
| `/proc/self/maps`, `smaps` | your address space ([14](../14-virtual-memory/README.md)) |
| `/proc/self/limits` | rlimits, e.g. "Max open files" |
| `/proc/meminfo`, `/proc/interrupts`, `/proc/sys/...` | system state and knobs (`sysctl`) |
| `/sys/class/<class>/<dev>/` | the device model: one value per file (`/sys/class/net/eth0/mtu`, `/sys/class/thermal/thermal_zone0/temp`) |
| `/sys/bus`, `/sys/devices`, `/sys/firmware/devicetree` | buses, the device tree the board booted with |

`c/07` parses `/proc/self/status`, walks `/proc/self/fd` (and checks it grows
by exactly 4 after `mkstemp` + `dup` + `pipe`), and reads `/sys/class`. On the
BeagleBone `/sys/class/gpio`, `/sys/class/leds/*/brightness` and
`/sys/bus/iio/devices/*/in_voltage*_raw` are how you test hardware from a
shell. Writing your own entries is
[linux-device-drivers/05](../../linux-device-drivers/05-sysfs-procfs-debugfs/README.md).

### 3.14 Watching syscalls: strace

`strace` uses `ptrace` to stop the program at every syscall entry and exit and
print it. It is the first tool to reach for when a program misbehaves at the
OS boundary.

```bash
make SAN=0 BUILD=build/nosan build/nosan/linux-internals/00-syscalls-and-file-io/c/04_stdio_buffering
B=build/nosan/linux-internals/00-syscalls-and-file-io/c

strace -c $B/04_stdio_buffering > /dev/null        # summary table: calls, errors, time per syscall
#   % time     seconds  usecs/call     calls    errors syscall
#    98.42    0.061656           3     20010           write     <- the 20000 one-byte writes
strace -e trace=openat,close $B/03_dup_fcntl_vectored  # only these calls
strace -e trace=%file -f $B/06_file_locking        # all path-taking calls, follow children (-f)
strace -e trace=%desc,%memory ...                  # classes: %file %desc %process %network %signal %memory
strace -tt -T -p <pid>                             # attach to a running daemon, timestamps + time in call
strace -e inject=write:error=EINTR:when=3 ./prog   # fault injection: test your EINTR handling!
strace -f -c -e trace=openat,close $B/08_fd_leak_bug   # 91 openat vs 61 close -> the leak
```

Build with `SAN=0` for strace. Sanitizer runtimes add hundreds of their own
calls, and LeakSanitizer refuses to run under ptrace. `ltrace` does the same
for library calls, and `perf trace` is a low-overhead alternative. On the board,
`strace` is in the Debian image (`apt install strace`).

---

## 4. Complexity table

| Operation | Cost | Why |
|---|---|---|
| trivial syscall (`getpid`) | ~100–300 ns x86 with mitigations, ~0.3–1 µs A72 | mode switch, register save, entry/exit work |
| vDSO `clock_gettime` | ~15–60 ns | no mode switch: read vvar page + CPU counter |
| `open` | O(path components) lookups, µs | dentry cache walk, permission checks per component |
| `read`/`write` (page cache hit) | O(n) memcpy + syscall | copy between user buffer and cached pages |
| `read` (cache miss) | device latency: ~0.1 ms eMMC, ~1 ms SD, ms–10 ms HDD | page fault / block I/O |
| `fsync` | ms (SD/eMMC can be 10–100 ms) | write dirty pages + journal + device cache flush |
| `lseek`, `dup`, `fcntl` | O(1) | only the fd table / description is touched |
| `writev` with k buffers | O(total bytes) + one syscall | saves k−1 syscalls and a copy |
| `fputc` (buffered) | ~ns per byte, 1 syscall per 4 KiB | user-space memcpy into the FILE buffer |
| `readdir` | O(entries) | getdents64 returns many entries per call |
| `flock` / `fcntl` lock (uncontended) | O(1) / O(locks on the inode) | list of byte ranges per inode |
| Memory per fd | 8 B slot + ~256 B `struct file` (shared by dups) | fd table entry, open file description |

---

## 5. C vs C++

| Concept | C (POSIX) | C++17 |
|---|---|---|
| own an fd | `int fd` + discipline (`goto out_close`) | move-only `File` class (`cpp/01`): destructor closes, copy deleted |
| errors | `-1` + `errno` | `throw std::system_error(errno, std::generic_category())`. Test with `e.code() == std::errc::...` |
| buffered text I/O | `FILE*`, `fprintf`, `setvbuf` | `std::ofstream` / `std::ifstream` (also RAII. State bits, no exceptions by default) |
| raw I/O, fsync, locks, ioctl | `open/read/write/fsync/fcntl/ioctl` | the same calls: the standard library has no fd API |
| paths, directories | `snprintf` + `opendir/readdir` + `stat` | `std::filesystem::path`, `directory_iterator`, `file_size`, `create_directories` (`cpp/02`) |
| non-throwing variant | always | `fs::xxx(p, std::error_code& ec)` overloads |
| temporary dir | `mkdtemp` | none in the standard (`temp_directory_path()` is just `/tmp`): use `mkdtemp` |

`cpp/01` shows that the fd is closed at the end of scope, after a move into a
`std::vector`, and while an exception unwinds the stack, which is exactly the
bug in `c/08`. It also shows the limit of `fstream`: you cannot `fsync` it,
open it with `O_EXCL`, lock it or `ioctl` it, because it hides the fd. For
embedded code that must survive power loss, the RAII fd class is what you
want.

---

## 6. Where it is used in the real world

- **Linux kernel.** `fs/read_write.c` (`ksys_read`, `vfs_read`), `fs/open.c`,
  `fs/file.c` (the fd table: `alloc_fd` picks the lowest free bit in a
  bitmap). Every driver fills `struct file_operations`. The syscall entry is
  `arch/arm64/kernel/entry-common.c` (`el0_svc`), the vDSO is `arch/arm64/kernel/vdso/`.
- **glibc / musl.** The `write()` wrapper is a few instructions around `svc #0`
  plus errno and cancellation-point handling. `stdio` lives in `libio/`.
  BusyBox systems with musl behave the same.
- **systemd and daemons.** Socket activation passes listening fds as 3, 4, …
  (`sd_listen_fds`), which only works because fds survive `exec()` when *not*
  `O_CLOEXEC`. Every other fd must be `O_CLOEXEC`. `journald` gets your stderr
  through a pipe.
- **Databases.** SQLite uses `pwrite` + `fsync`/`fdatasync` + POSIX
  advisory locks (and documents the "close drops locks" trap at length).
  PostgreSQL's 2018 "fsyncgate" showed what happens when apps assume a
  failed `fsync` can simply be retried.
- **Embedded update systems.** RAUC, SWUpdate and Mender write to an inactive
  partition, `fsync`, then switch the bootloader variable atomically. Config
  files use tmp + rename. `/var/log` is often on tmpfs to save flash wear.
- **Hardware access from user space.** `/dev/i2c-N` + `ioctl(I2C_RDWR)`,
  `/dev/spidevB.C` + `ioctl(SPI_IOC_MESSAGE)`, `/dev/gpiochipN` + the GPIO
  character-device ioctls (libgpiod), V4L2 `/dev/video0` with ~100 ioctls,
  `termios` for UARTs. All of them are fds and ioctls.
- **TDA4VM.** The R5F and C7x remote cores are reached through `/dev/rpmsg*`
  and `remoteproc` sysfs files. TI's TIOVX/vision-apps memory comes from a
  `/dev/dma_heap/*` fd. Thermal and clock info is in `/sys/class/thermal` and
  `debugfs`.

---

## 7. Common mistakes and pitfalls

1. **Ignoring short reads/writes.** `write(fd, buf, n)` returning less than `n`
   is legal. Loop (`write_full`), especially on pipes, sockets and UARTs.
2. **Treating `read() == 0` as "no data yet".** It means EOF. A blocking fd
   with no data simply blocks, and a non-blocking one returns −1/`EAGAIN`.
3. **Reading `errno` without a failure, or after another call.** Check the
   return value, then save errno at once.
4. **No EINTR handling** in blocking calls of a program that has signal
   handlers (timers, SIGCHLD, SIGTERM for clean shutdown). Test it with
   `strace -e inject=read:error=EINTR`.
5. **fd leaks on error paths** (`c/08_fd_leak_bug.c`). LeakSanitizer does not
   track fds. Use RAII, `goto out_close`, and watch `ls /proc/<pid>/fd | wc -l`.
6. **Forgetting `O_CLOEXEC`.** Children started with `system()`/`popen()`/`exec`
   inherit your sockets and device fds, keep them open, and hold resources
   (a TCP port stays bound, a pipe never reaches EOF).
7. **Closing an fd twice.** The second `close()` may close an fd number that
   another thread just got from `open()`. Set your variable to −1 after
   closing. RAII does this for you.
8. **Retrying `close()` on EINTR.** On Linux the fd is always released, even
   when `close` returns an error. Do not retry: you might close someone else's
   fd.
9. **Assuming `write()` means "on disk".** Use `fsync`, the atomic-replace
   pattern, and `fsync` the directory after `rename`.
10. **`printf` then `fork()` without `fflush`.** Output is duplicated (`c/04`).
    And `exit()` in a forked child flushes the parent's copied buffers again,
    so use `_exit()`.
11. **Mixing `FILE*` and the raw fd** of the same file without `fflush`:
    writes land out of order.
12. **POSIX `fcntl` locks + a library that opens the same file** silently drop
    the lock (`c/06`). Use OFD locks.
13. **`O_DIRECT` with an unaligned buffer** gives `EINVAL`. Use `posix_memalign`
    and block-sized lengths.
14. **Calling `syscall(SYS_clock_gettime)`** or a debug wrapper in a hot loop
    instead of the libc function bypasses the vDSO and is ~5× slower.
15. **Parsing `/proc` with fixed-size assumptions.** Fields get added between
    kernel versions. Search by key (`"VmRSS:"`), not by line number.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_syscalls_and_vdso.c](c/01_syscalls_and_vdso.c) | `getpid` vs `syscall(SYS_getpid)` vs raw `svc #0`/`syscall` asm; raw `-ENOENT` vs errno; vDSO vs real-syscall cost; vDSO in auxv and maps; EINTR with and without `SA_RESTART` | `make run P=linux-internals/00-syscalls-and-file-io/c/01_syscalls_and_vdso.c` |
| [c/02_open_read_write.c](c/02_open_read_write.c) | open/read/write/lseek/close, `O_EXCL`, `O_TRUNC`, sparse file, lowest free fd, `O_CLOEXEC`, `O_APPEND` vs overwrite, `O_NONBLOCK` + short write, `read_full`/`write_full`, `O_SYNC` timing, `O_DIRECT` alignment | `make run P=linux-internals/00-syscalls-and-file-io/c/02_open_read_write.c` |
| [c/03_dup_fcntl_vectored.c](c/03_dup_fcntl_vectored.c) | two opens vs `dup` (shared offset), fork shares the offset, flags per fd vs per description, `F_DUPFD_CLOEXEC`, `dup2` redirection of stdout, `pread`/`pwrite`, `writev`/`readv` | `make run P=linux-internals/00-syscalls-and-file-io/c/03_dup_fcntl_vectored.c` |
| [c/04_stdio_buffering.c](c/04_stdio_buffering.c) | counts write calls for full/line/no buffering with `fopencookie`; duplicate output after fork; lost output after `_exit`; `fputc` vs 1-byte `write` speed | `make run P=linux-internals/00-syscalls-and-file-io/c/04_stdio_buffering.c` |
| [c/05_fsync_stat_dirs.c](c/05_fsync_stat_dirs.c) | write vs fsync vs fdatasync timing, `Dirty:` in meminfo, atomic replace (tmp + fsync + rename + fsync dir), stat/lstat/fstat, hard links and symlinks, unlink while open, opendir/readdir | `make run P=linux-internals/00-syscalls-and-file-io/c/05_fsync_stat_dirs.c` |
| [c/06_file_locking.c](c/06_file_locking.c) | `flock` between processes, advisory only, POSIX byte ranges + `F_GETLK` holder pid, the close-any-fd trap, OFD locks fix it | `make run P=linux-internals/00-syscalls-and-file-io/c/06_file_locking.c` |
| [c/07_ioctl_proc_sys.c](c/07_ioctl_proc_sys.c) | `FIONREAD` on a pipe, `TIOCGWINSZ` (`ENOTTY` on a pipe), decoding an ioctl number, `FS_IOC_GETFLAGS`; `/proc/self/status`, `/proc/self/fd`, rlimits, `/sys/class` | `make run P=linux-internals/00-syscalls-and-file-io/c/07_ioctl_proc_sys.c` |
| [c/08_fd_leak_bug.c](c/08_fd_leak_bug.c) | **BUG**: fd leak on an early return; lowers `RLIMIT_NOFILE` to 32, hits `EMFILE`, counts `/proc/self/fd`, exits 1. Not caught by ASan/LSan | `make run P=linux-internals/00-syscalls-and-file-io/c/08_fd_leak_bug.c` (exits 1), then `strace -f -c -e trace=openat,close` on the SAN=0 build |
| [cpp/01_raii_file.cpp](cpp/01_raii_file.cpp) | move-only `File` (closes in destructor, `system_error`, `write_all`/`read_all`), moves into a vector, no leak on exception; `std::ofstream`/`ifstream` and their limits | `make run P=linux-internals/00-syscalls-and-file-io/cpp/01_raii_file.cpp` |
| [cpp/02_filesystem.cpp](cpp/02_filesystem.cpp) | `std::filesystem`: path parts, `create_directories`, symlink, recursive walk, rename, permissions, `error_code` vs exceptions, `space`, `remove_all` | `make run P=linux-internals/00-syscalls-and-file-io/cpp/02_filesystem.cpp` |

Every non-bug program is clean under ASan/UBSan and under ThreadSanitizer:

```bash
make run P=linux-internals/00-syscalls-and-file-io/c/06_file_locking.c SAN=tsan BUILD=build/tsan
```

Tools that go with this lesson: `strace` (§3.14), `ls -l /proc/<pid>/fd`,
`lsof -p <pid>`, `valgrind --track-fds=yes` (build with `SAN=0`), and gdb from
[dsa/03-debugging](../../dsa/03-debugging/README.md).

---

## 9. Exercises

**Easy**
1. Run `strace -c` on every program in this lesson (SAN=0 build). Which
   syscall dominates each one, and why?
2. In `c/04`, change the buffer size passed to `setvbuf` to 512 and to 64 KiB.
   Predict the write counts before running.
3. Write `mycat`: copy argv files (or stdin) to stdout with `read_full`/`write_full`
   and a 64 KiB buffer. Compare its `strace -c` with `cat`.

**Medium**
4. Fix `c/08_fd_leak_bug.c` twice: once with a single `goto out` exit, once by
   porting it to the `File` class from `cpp/01`. Confirm with `strace -c` that the
   number of openat calls equals the number of close calls.
5. Write `tee`-like code that uses `writev` to write the same buffer to two
   fds, handling a short write on either one.
6. Add an `EINTR` test to `c/02`'s `read_full`: run it under
   `strace -e inject=read:error=EINTR:when=2+` and check it still gets all the
   data.
7. Implement a single-instance daemon: `open("/tmp/app.lock", O_CREAT|O_RDWR|O_CLOEXEC)`,
   `flock(LOCK_EX|LOCK_NB)`, write the pid. Start it twice. What happens when
   the first one is `kill -9`ed?

**Hard**
8. Power-cut test on the BeagleBone with a spare SD card: write a counter to a
   file 10×/s with (a) `O_TRUNC` + write, (b) atomic replace without fsync,
   (c) full atomic replace. Pull the power 20 times and record corrupt/empty
   files for each.
9. Write a tiny syscall tracer with `ptrace(PTRACE_SYSCALL)` that prints the
   syscall number (`orig_rax` on x86, `regs[8]` on arm64) of a child process.
   It is the core of `strace`.
10. Find the vDSO's exported symbols: copy the `[vdso]` range from
    `/proc/self/mem` to a file and run `readelf -Ws` on it. Which functions
    does the arm64 vDSO export, compared with x86-64?

---

## 10. Remember forever

```
syscall = svc #0 (arm64: x8=nr, x0..x5 args, x0=ret) / syscall (x86: rax). EL0->EL1, ring3->ring0
ret in -4095..-1 = -errno; libc: return -1 + errno. Check ret FIRST, save errno at once
vDSO: clock_gettime/gettimeofday in user space (~20 ns vs ~150 ns real syscall)
fd -> open file description (offset, O_APPEND/O_NONBLOCK) -> inode (data, size, mode)
dup/fork share the description (offset!); FD_CLOEXEC is per fd; open() = lowest free fd
read/write may be SHORT; read()==0 is EOF; loop + retry EINTR (read_full/write_full)
O_CLOEXEC always; O_EXCL = atomic create; O_APPEND = atomic end-write; O_NONBLOCK -> EAGAIN
stdio: full (files/pipes) / line (tty) / none (stderr). fflush before fork; _exit loses buffer
write() = page cache only. Durable: tmp + fsync + rename + fsync(dir)
Locks are advisory. POSIX fcntl lock dies on ANY close of the file -> use OFD or flock
ioctl = everything that is not bytes (_IOR(type,nr,T)); ENOTTY = wrong device. strace -c / -e trace=
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What exactly happens, register by register, when glibc's `getpid()` runs on
   the Cortex-A72? Which exception level runs which part?
2. Why can the kernel return `-ENOENT` while your program sees `-1`? Where does
   `errno` live, and why is it safe with threads?
3. What is the vDSO, why does `clock_gettime` not need a real syscall, and how
   would you find the vDSO in a running process?
4. When does a blocked `read()` return `EINTR`, and what does `SA_RESTART` change?
   Name a call that is never restarted.
5. Draw fd table → open file description → inode. Where do the offset,
   `O_APPEND` and `FD_CLOEXEC` live? What do `dup()`, `fork()` and a second
   `open()` share?
6. List four situations where `read()` returns fewer bytes than asked. What
   does a return value of 0 mean?
7. Why is `O_CREAT|O_EXCL` safe for lock files when `access()` + `open()` is
   not? Why does `O_CLOEXEC` exist as an open flag?
8. A program prints "starting" and then forks. On the terminal you see it
   once, in `./prog | cat` twice. Explain, and give two fixes.
9. Your device writes `settings.json` with `O_TRUNC` + `write` and sometimes
   boots with an empty file after a power cut. Explain and give the correct
   sequence of calls.
10. Why does a hard link have the same inode number while a symlink has its
    own? What does `unlink` do to an open file?
11. What is the difference between `flock`, POSIX `fcntl` locks and OFD locks?
    Describe the "close drops the lock" trap.
12. Why do drivers expose ioctls, what is encoded in an ioctl request number,
    and what does `ENOTTY` mean?
13. Which `strace` invocation would you use to find (a) which config file a
    program opens, (b) where a slow program spends its syscall time,
    (c) whether a program handles EINTR?
