# 03 — Debugging: gdb, sanitizers, valgrind, strace, and a method that always works

> Writing code is half the job. The other half is finding out why it does
> not do what you meant. This lesson gives you the tools and — more
> important — a *method*, then lets you practise on ten real bugs.

---

## 1. Why this exists

C and C++ do not check anything at run time. A bad pointer does not raise an
exception; it silently corrupts memory and the program crashes *somewhere
else, later*, or prints a wrong number, or works on your PC and fails on the
board. Guessing and adding random `printf`s does not scale. You need:

* **Tools that make invisible errors visible**: sanitizers, valgrind, warnings.
* **Tools that let you look inside a running (or dead) program**: gdb, core dumps.
* **Tools that show the program's conversation with the OS**: strace, ltrace.
* **A systematic method**, so you converge on the bug instead of wandering.

Every pointer and memory bug from [01-pointers](../01-pointers/README.md) and
[02-memory-management](../02-memory-management/README.md) has a runnable
`*_bug` program here, with the exact session that finds it.

---

## 2. Mental model

**Analogy — a detective at a crime scene.** The crash is the body. The *crash
site* (where it segfaulted) is rarely where the crime happened (where memory
was corrupted). gdb is the forensic kit you use at the scene; a core dump is
the photograph of the scene you can study later; sanitizers are CCTV cameras
installed *before* the crime that record the exact moment and the culprit;
strace is the phone log; your log lines are witness statements.

The method is a loop — the scientific method:

```
        ┌────────────────────────────────────────────┐
        ▼                                            │
  1. REPRODUCE  ── make it fail on demand, smallest input, same every time
        │
  2. OBSERVE    ── read the FULL message; run under ASan/UBSan; get a backtrace
        │
  3. HYPOTHESISE ── "p is NULL because find() returned NULL for 'ddr'"
        │
  4. TEST       ── one experiment that can prove it WRONG (breakpoint, print, watchpoint)
        │
        ├── wrong ─────────────────────────────────────┘ (new hypothesis)
        ▼
  5. FIX the cause (not the symptom) + add a test/assert that would have caught it
        │
  6. VERIFY    ── original repro passes, nothing else broke, sanitizers clean
```

```
 Which tool for which symptom?
 ┌─────────────────────────────────┬─────────────────────────────────────────────┐
 │ crash (SIGSEGV/SIGABRT)          │ gdb (run → bt), core dump, ASan             │
 │ wrong result, no crash           │ gdb breakpoints/watchpoints, logs, assert   │
 │ "works at -O0, fails at -O2"     │ UBSan, -Wall -O2 warnings, valgrind (uninit)│
 │ memory grows over time           │ LeakSanitizer, valgrind, /proc/PID/status   │
 │ random/heisenbug corruption      │ ASan first, then watchpoint on the victim   │
 │ hangs                            │ gdb -p PID → thread apply all bt; strace -p │
 │ "file not found"/permission/ioctl│ strace                                      │
 │ slow                             │ perf (04-complexity), strace -c, ltrace -c  │
 └─────────────────────────────────┴─────────────────────────────────────────────┘
```

---

## 3. How it works

### 3.1 gdb

Build with `-g` (debug info: maps addresses ↔ source lines, names variables)
and preferably `-O0` (at `-O2` variables are "optimized out" and lines jump
around). This repo's Makefile uses `-O0 -g3` (`-g3` even includes macros).

```bash
gdb -q ./build/dsa/03-debugging/c/08_gdb_practice          # start
gdb -q --args ./prog arg1 arg2                              # with arguments
gdb -q -p 12345                                             # attach to a running process
gdb -q ./prog core                                          # post-mortem on a core dump
gdb -q -tui ./prog          # or press Ctrl-x a inside gdb: source window + command window
```

A real session on `c/08_gdb_practice.c` (outputs are real, trimmed):

```
(gdb) break fill                       # stop when fill() is entered
Breakpoint 1 at 0x38a4: file dsa/03-debugging/c/08_gdb_practice.c, line 44.
(gdb) run
Breakpoint 1.1, fill (f=0x7ffff4a00040, seq=0) at dsa/03-debugging/c/08_gdb_practice.c:44
44          f->seq = seq;
(gdb) bt                               # backtrace: who called whom
#0  fill (f=0x7ffff4a00040, seq=0) at dsa/03-debugging/c/08_gdb_practice.c:44
#1  0x00005555555582f5 in main (argc=1, argv=0x7fffffffd218) at dsa/03-debugging/c/08_gdb_practice.c:68
(gdb) info args
f = 0x7ffff4a00040
seq = 0
(gdb) next                             # run one line, stepping OVER calls  (step = INTO calls)
45          f->width = f->height = 4;
(gdb) ptype struct pixel
type = struct pixel {
    uint8_t r;
    uint8_t g;
    uint8_t b;
}
(gdb) break 37 if y == 2 && x == 3     # conditional breakpoint
(gdb) continue
Breakpoint 2, checksum (f=0x7ffff4a00040) at dsa/03-debugging/c/08_gdb_practice.c:37
(gdb) info locals
p = 0x7ffff4a0006d
x = 3
y = 2
sum = 145817412
(gdb) p *p                             # print what a pointer points to
$2 = {r = 180 '\264', g = 120 'x', b = 0 '\000'}
(gdb) p/x sum                          # /x hex, /d decimal, /t binary, /c char
$3 = 0x8b0ff44
(gdb) finish                           # run until the current function returns
Run till exit from #0  checksum (...)
0x0000555555557eb7 in fill (f=0x7ffff4a00040, seq=0) at ...:49
Value returned is $4 = 2381902816
(gdb) delete                           # remove all breakpoints
(gdb) up                               # go to the caller's frame (down = back)
#1  main (...) at ...:68
(gdb) watch frames[1].checksum         # HARDWARE watchpoint: stop when this memory changes
Hardware watchpoint 3: frames[1].checksum
(gdb) continue
Hardware watchpoint 3: frames[1].checksum
Old value = 0
New value = 1568019680
fill (f=0x7ffff4a00080, seq=1) at ...:50
(gdb) up
(gdb) p frames[1].data[3][3]
$6 = {r = 180 '\264', g = 180 '\264', b = 1 '\001'}
(gdb) x/12xb &frames[1].data[0][0]     # eXamine memory: 12 units, hex, bytes
0x7ffff4a0008c: 0x00  0x00  0x01  0x3c  0x00  0x01  0x78  0x00
0x7ffff4a00094: 0x01  0xb4  0x00  0x01
(gdb) x/4dw &frames[1]                 # 4 words (4 bytes) in decimal: seq, width, height, ...
0x7ffff4a00080: 1   4   4   1006698496
(gdb) info breakpoints
(gdb) kill / quit
```

`x` format: `x/NFU addr` — N count, F format (`x` hex, `d` decimal, `u`
unsigned, `c` char, `s` string, `i` instructions), U unit (`b` 1, `h` 2,
`w` 4, `g` 8 bytes). `x/s str`, `x/10i $pc` (next 10 instructions), `x/8gx $sp`
(top of the stack).

**Watchpoints** are the weapon for memory corruption: "who overwrote this
field?" `watch var` (stop on write), `rwatch` (on read), `awatch` (both),
`watch -l expr` (watch the *location*, even after leaving the scope). x86 has
4 hardware debug registers, arm64 typically 4 as well: watch at most 4 small
locations, or gdb falls back to (very slow) software watchpoints.

**Attaching to a running process** (a hung daemon, a GStreamer pipeline):

```bash
./build/dsa/03-debugging/c/08_gdb_practice --wait &
gdb -q -p $!
(gdb) bt                         # it is inside nanosleep() called from main
(gdb) thread apply all bt        # every thread's stack: THE command for hangs/deadlocks
(gdb) set var g_keep_waiting = 0 # change program state
(gdb) detach                     # let it continue without gdb
```

If attach fails with "ptrace: Operation not permitted": Ubuntu sets
`/proc/sys/kernel/yama/ptrace_scope = 1` (you may only attach to your own
*children*). Start the program from gdb, use `sudo`, or
`echo 0 | sudo tee /proc/sys/kernel/yama/ptrace_scope` on a dev machine.

**C++ specifics** (`cpp/05_catch_throw.cpp`):

```
(gdb) catch throw            # stop at the THROW, before the stack is unwound
(gdb) run
Catchpoint 1 (exception thrown), 0x00007ffff74bb35a in __cxa_throw () from libstdc++.so.6
(gdb) bt
#0  __cxa_throw () from libstdc++.so.6
#1  parse_value (key="height", text="1080px") at .../05_catch_throw.cpp:33
#2  load (raw=std::vector of length 2, capacity 2 = {...}) at ...:41
#3  main () at ...:52
(gdb) break clamp_to<int>    # a template instance;  rbreak ^parse_  = regex breakpoints
```

Pretty printers show `std::string`, `std::vector`, `std::map` contents
directly (`p cfg`). `info vtbl obj`, `p *this`, `ptype obj`, `set print pretty on`.

**Remote debugging on the BeagleBone AI-64**: on the board
`gdbserver :2345 ./app`; on the PC
`gdb-multiarch ./app` (the unstripped binary) → `set sysroot <board rootfs>` →
`target remote bbai64:2345`. Everything above works the same.

### 3.2 Core dumps

A core dump is the process's memory + registers at the moment it died, saved
to a file so you can run `gdb prog core` later (on another machine, after a
field failure).

```bash
ulimit -c                      # 0 = core dumps disabled (the default shell setting)
ulimit -c unlimited            # enable for this shell
cat /proc/sys/kernel/core_pattern
```

Where the file goes depends on `core_pattern`:

| `core_pattern` | Where the core goes | How to open it |
|---|---|---|
| `core` (plain) | `./core` or `./core.PID` in the crash's cwd | `gdb prog core` |
| `\|/usr/share/apport/apport ...` (Ubuntu desktop) | `/var/lib/apport/coredump/core.<path>.<uid>.<boot-id>.<pid>.<time>` | `gdb prog /var/lib/apport/coredump/core....` |
| `\|/usr/lib/systemd/systemd-coredump ...` | systemd journal | `coredumpctl list`, `coredumpctl gdb <PID>`, `coredumpctl info` (package `systemd-coredump`) |

Session on this machine (Ubuntu 24.04, apport), with a `SAN=0` build of bug 6:

```
$ ulimit -c unlimited
$ ./07_null_deref_bug
Segmentation fault      (core dumped)
$ ls /var/lib/apport/coredump/
core._tmp_..._07_null_deref_bug.1000.6fd16640-....95454.475337
$ gdb -q ./07_null_deref_bug /var/lib/apport/coredump/core.*07_null*
Program terminated with signal SIGSEGV, Segmentation fault.
#0  0x0000558ddb85c21a in read_temp (name=0x558ddb85d00c "ddr") at .../07_null_deref_bug.c:35
35          return s->value;                               /* BUG: no NULL check */
(gdb) bt
#0  read_temp (name=0x558ddb85d00c "ddr") at .../07_null_deref_bug.c:35
#1  main () at .../07_null_deref_bug.c:42
(gdb) p s
$1 = (struct sensor *) 0x0
```

Notes: ASan disables core dumps by default (they would include terabytes of
shadow memory) — build with `SAN=0` or set `ASAN_OPTIONS=disable_coredump=0`.
Keep the **unstripped** binary that produced the core (or its separate debug
info), otherwise `bt` shows only addresses. On the board, set
`echo /tmp/core.%e.%p > /proc/sys/kernel/core_pattern` and copy the core back.

### 3.3 Sanitizers: ASan, UBSan, LSan

Sanitizers are **compiler instrumentation**: extra checks inserted around every
memory access. This repo builds everything with
`-fsanitize=address,undefined -fno-omit-frame-pointer`.

**How ASan works** — shadow memory: every 8 bytes of your memory have 1
shadow byte saying how many are addressable. Allocations are surrounded by
poisoned **redzones**; freed memory is poisoned and held in **quarantine**
(so it is not immediately reused). Each load/store first checks the shadow.

```
 malloc(11):   [ redzone fa fa ][ 00 = 8 ok ][ 03 = 3 ok ][ redzone fa fa ]
                                  0x...10      0x...18       0x...1b ← first bad byte
 after free:   [ fa fa ][ fd fd ] ...   fd = freed: any access → heap-use-after-free
```

Cost: ~2× CPU, ~3× memory. **How to read an ASan report** (top to bottom):

1. `ERROR: AddressSanitizer: <bug type>` — heap-buffer-overflow, heap-use-after-free,
   stack-buffer-overflow, global-buffer-overflow, double-free, stack-overflow,
   alloc-dealloc-mismatch, SEGV on unknown address.
2. `READ/WRITE of size N at ADDR` + stack #0 — **your** first frame is the
   bad access.
3. `ADDR is located K bytes after/inside N-byte region` — how far off you were.
4. `freed by thread T0 here:` and `previously allocated by ... here:` — the
   whole life story of the block.
5. Shadow bytes map: `[fd]` freed, `fa` heap redzone, `f1/f2/f3` stack redzones,
   `f9` global redzone, `0N` = only N bytes addressable.

**UBSan** prints `file:line:col: runtime error: <what>`. By default it then
*continues*; this repo builds with `-fno-sanitize-recover=all`, so the
program stops at the first report (`UBSAN_OPTIONS=print_stacktrace=1` adds a
stack). **LeakSanitizer** runs at exit and lists "Direct leak" (no pointer
to the block anywhere) and "Indirect leak" (only reachable from leaked blocks)
with the allocation stack. Useful options:
`ASAN_OPTIONS=detect_leaks=0`, `halt_on_error=0` (needs `-fsanitize-recover=address`),
`detect_stack_use_after_return=1`, `abort_on_error=1` (for gdb/core). In gdb,
`break __asan_report_error` stops right before the report.

Other sanitizers: `-fsanitize=thread` (data races; not combinable with ASan),
`-fsanitize=memory` (clang only: uninitialised reads), `-fsanitize=leak` alone.

### 3.4 printf / log debugging done well

`printf` debugging is legitimate when done properly (`c/01_assert_and_logging.c`):

* Log to **stderr** (unbuffered: you see the last line before a crash; stdout
  is buffered when piped and the last lines die with the process).
* Every line: **monotonic timestamp, level, `__FILE__:__LINE__ __func__()`**,
  the values *and their meaning*, `strerror(errno)` for failed syscalls.
* A **runtime level** (`LOG_LEVEL=3 ./prog`) instead of adding/removing printfs.
* `__attribute__((format(printf, ...)))` so the compiler checks arguments.
* **Hex dumps** for buffers/packets/registers.
* In the kernel: `pr_debug`/`dev_dbg` + dynamic debug
  (`echo 'file mydrv.c +p' > /sys/kernel/debug/dynamic_debug/control`),
  and `trace_printk` for hot paths. In GStreamer: `GST_DEBUG=myelement:6`.
* Remove or downgrade debug logs before committing; logs in a 60 fps loop
  change timing (the bug may vanish: a "Heisenbug").

### 3.5 `assert`

`assert(cond)` aborts with `file:line: func: Assertion 'cond' failed.` when
`cond` is false, and **compiles to nothing with `-DNDEBUG`** (release builds).

* Use it for **internal invariants** ("this list is sorted", "index < size")
  — things that are bugs if false.
* Do **not** use it for errors that can legitimately happen (bad input, missing
  file, `malloc` failure): handle those.
* Never put side effects inside: `assert(fd = open(...))` disappears in release.
* `assert(x && "message")` adds a message. `static_assert`/`_Static_assert`
  checks at compile time for free.
* An assertion failure in gdb stops at `abort()`: `bt`, then `frame N` into your code.

### 3.6 `strace` and `ltrace`

```bash
strace ./prog                       # every syscall with args and result
strace -f ./prog                    # follow forks/threads
strace -e trace=openat,read,write ./prog       # filter;  -e trace=%file,%net,%memory,%desc
strace -p PID                       # attach: what is it blocked on?
strace -c ./prog                    # summary table: count, errors, time per syscall
strace -tt -T -o trace.txt ./prog   # timestamps + time spent in each call, to a file
ltrace ./prog                       # library calls (sudo apt install ltrace)
ltrace -e malloc+free ./prog
```

Real output on `c/09_syscalls_for_strace.c`:

```
openat(AT_FDCWD, "/etc/does-not-exist.conf", O_RDONLY) = -1 ENOENT (No such file or directory)
openat(AT_FDCWD, "/tmp/strace-demo-iGPv5m", O_RDWR|O_CREAT|O_EXCL, 0600) = 3
write(3, "hello from user space\n", 22) = 22
fsync(3)                                = 0
lseek(3, 0, SEEK_SET)                   = 0
read(3, "hello from user space\n", 64)  = 22
fstat(3, {st_mode=S_IFREG|0600, st_size=22, ...}) = 0
mmap(NULL, 22, PROT_READ, MAP_PRIVATE, 3, 0) = 0x791dc7f3a000
munmap(0x791dc7f3a000, 22)              = 0
unlink("/tmp/strace-demo-iGPv5m")       = 0
mmap(NULL, 1052672, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) = 0x791dc7e26000   ← malloc(1 MiB)
```

On embedded Linux, strace answers 80% of "the app fails on the board but not
on my PC": wrong device node, missing firmware file, `ioctl(...) = -1 EINVAL`
from a V4L2 driver, a config file read from a different path.

### 3.7 valgrind memcheck

valgrind runs your program on a synthetic CPU and tracks every byte
(addressability *and* initialisation). No recompilation needed — but it must
**not** be combined with ASan, so build with `SAN=0`. 20–50× slower.

```bash
sudo apt install valgrind            # (not installed on this machine)
make SAN=0 build/dsa/03-debugging/c/06_uninit_bug     # the Makefile rebuilds when flags change
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
         ./build/dsa/03-debugging/c/06_uninit_bug
valgrind --vgdb=yes --vgdb-error=0 ./prog      # then in gdb: target remote | vgdb
```

What it finds that ASan does not: **uninitialised reads** (`Conditional jump
or move depends on uninitialised value(s)`), and it works on binaries you
cannot rebuild. What ASan finds that valgrind does not: overflows of stack and
global arrays. Rule: **ASan in every debug build; valgrind when you suspect
uninitialised memory or cannot recompile.** (On the BeagleBone, valgrind works
on arm64 too: `apt install valgrind` on the board.)

### 3.8 Compiler warnings as debuggers

The cheapest debugger runs before your program does. Real examples (gcc 13):

```
warning: function returns address of local variable [-Wreturn-local-addr]
warning: comparison of integer expressions of different signedness: 'int' and 'unsigned int' [-Wsign-compare]
warning: format '%d' expects a matching 'int' argument [-Wformat=]
warning: '%d' directive writing 5 bytes into a region of size 4 [-Wformat-overflow=]
warning: 'total' may be used uninitialized [-Wmaybe-uninitialized]     ← only at -O2!
warning: pointer 'first' may be used after 'realloc' [-Wuse-after-free]
```

* Always `-Wall -Wextra` (+ `-Wpedantic -Wshadow -Wformat=2` as here).
  `-Werror` in CI.
* Some warnings need the optimiser's data-flow analysis: **also build with
  `-O2 -Wall`** occasionally (`make OPT=1 SAN=0`), it finds uninitialised
  variables and overflows that `-O0` does not.
* `-fanalyzer` (gcc ≥ 10): a static analyser for leaks, double frees, NULL
  dereferences across functions. Also `clang --analyze`, `cppcheck`,
  `clang-tidy`.

### 3.9 A systematic method (keep this checklist)

1. **Reproduce** reliably. Reduce the input. Write the repro command down.
2. **Read the whole error message**, the first error first.
3. **Run under ASan+UBSan** (the default here). Most memory bugs end here.
4. **Get a backtrace** (gdb `bt` or core dump). Go to *your* first frame.
5. **Check assumptions** at that frame: print every pointer and index.
   Which one is wrong? Where did it come from? (`up`, watchpoints)
6. **Bisect**: the input (halve it), the code (comment out halves), the
   history (`git bisect run ./test.sh`).
7. **Explain it out loud** (rubber duck) — the gap in the explanation is the bug.
8. **Fix the cause**, add an assert or test that would have caught it, re-run
   everything with sanitizers.

---

## 4. The bug clinic — ten bugs and the exact session that finds each

Run a bug with `make run P=<file>`; it compiles cleanly (bugs that compilers
catch are not interesting) and the tool output appears. Outputs below are
real, trimmed (`#3+` libc frames and the shadow-byte legend removed).

### Bug 1: memory leak — [c/02_leak_bug.c](c/02_leak_bug.c)

```
$ make run P=dsa/03-debugging/c/02_leak_bug.c
==94155==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 128 byte(s) in 2 object(s) allocated from:
    #0 0x7fd4ad6fd9c7 in malloc ../../../../src/libsanitizer/asan/asan_malloc_linux.cpp:69
    #1 0x5956c9cf5314 in process dsa/03-debugging/c/02_leak_bug.c:27
    #2 0x5956c9cf541e in main dsa/03-debugging/c/02_leak_bug.c:48

Direct leak of 128 byte(s) in 4 object(s) allocated from:
    #1 0x5956c9cf52c1 in make_label dsa/03-debugging/c/02_leak_bug.c:18
    #2 0x5956c9cf541e in main dsa/03-debugging/c/02_leak_bug.c:41

SUMMARY: AddressSanitizer: 256 byte(s) leaked in 6 allocation(s).
```

Reading: 2 × 64 B from `process` line 27 (the early `return -1` skips
`free(tmp)`); 4 × 32 B from `make_label` (labels 0–3 overwritten in the loop).
Fix: free the old label before overwriting; use a single exit path
(`goto out;`) or RAII. Valgrind equivalent:

```
$ valgrind --leak-check=full ./02_leak_bug        (SAN=0 build)
==1234== 128 bytes in 4 blocks are definitely lost in loss record 2 of 2
==1234==    at 0x4846828: malloc (vg_replace_malloc.c:442)
==1234==    by 0x1091C6: make_label (02_leak_bug.c:18)
==1234==    by 0x109287: main (02_leak_bug.c:41)
==1234== LEAK SUMMARY:
==1234==    definitely lost: 256 bytes in 6 blocks
```

On a long-running board process without sanitizers: watch
`grep VmRSS /proc/PID/status` grow, then reproduce on the PC under LSan.

### Bug 2: use-after-free — [c/03_use_after_free_bug.c](c/03_use_after_free_bug.c)

```
==94211==ERROR: AddressSanitizer: heap-use-after-free on address 0x502000000014 at pc 0x596efdeff64f ...
READ of size 4 at 0x502000000014 thread T0
    #0 0x596efdeff64e in main dsa/03-debugging/c/03_use_after_free_bug.c:47
0x502000000014 is located 4 bytes inside of 16-byte region [0x502000000010,0x502000000020)
freed by thread T0 here:
    #0 0x782d9befc4d8 in free ...
    #1 0x596efdeff45b in close_session dsa/03-debugging/c/03_use_after_free_bug.c:36
    #2 0x596efdeff5c9 in main dsa/03-debugging/c/03_use_after_free_bug.c:44
previously allocated by thread T0 here:
    #0 0x782d9befd340 in calloc ...
    #1 0x596efdeff2c6 in open_session dsa/03-debugging/c/03_use_after_free_bug.c:27
=>0x502000000000: fa fa[fd]fd fa fa ...
```

"4 bytes inside a 16-byte region" = the `frames` field (offset 4) of a
`struct session`. Three stacks tell the whole story: allocated in
`open_session`, freed in `close_session`, used at line 47 through the stale
global `current`. Fix: clear `current` in `close_session` (or don't keep
non-owning copies). Without ASan (`SAN=0`) the program runs to the end and
prints `current session has 5 frames` (not 3): glibc's tcache stored its
free-list pointer in the freed block, over the old fields. No crash, a wrong
number — which is why UAFs reach production. Valgrind: `Invalid read of size 4 ... Address
0x4a8a044 is 4 bytes inside a block of size 16 free'd`.

### Bug 3: stack overflow by recursion — [c/04_stack_overflow_bug.c](c/04_stack_overflow_bug.c)

```
==94499==ERROR: AddressSanitizer: stack-overflow on address 0x7fff79eb0f6c (pc ... T0)
    #0 0x61a7f524e2a1 in count_nodes dsa/03-debugging/c/04_stack_overflow_bug.c:17
    #1 0x61a7f524e48a in count_nodes dsa/03-debugging/c/04_stack_overflow_bug.c:22
    #2 0x61a7f524e48a in count_nodes dsa/03-debugging/c/04_stack_overflow_bug.c:22
    ... (hundreds of identical frames)
```

(Occasionally the deadly frame is inside ASan's own runtime; the repeated
frames below it are the clue.) In gdb (a `SAN=0` build):

```
(gdb) run
Program received signal SIGSEGV, Segmentation fault.
0x0000555555555178 in count_nodes (depth=<error reading variable: Cannot access memory ...>, ...) at ...:17
(gdb) bt -3                              # the OUTERMOST 3 frames
#27566 count_nodes (depth=10, level=1) at ...:22
#27567 count_nodes (depth=10, level=0) at ...:22
#27568 main () at ...:27
(gdb) bt 3                               # the innermost 3
#1  count_nodes (depth=10, level=27566) at ...:22
(gdb) frame 1
(gdb) p level                            # 27566 levels deep for depth 10 → base case broken
```

27 568 frames × ~300 B ≈ 8 MiB = `ulimit -s`. The base case tests `depth`
(never changes) instead of `level == depth`. General cures: fix the base
case, convert deep recursion to iteration with an explicit stack, never
recurse on untrusted input depth.

### Bug 4: off-by-one heap overflow — [c/05_heap_overflow_bug.c](c/05_heap_overflow_bug.c)

```
==94321==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x50200000001b ...
WRITE of size 12 at 0x50200000001b thread T0
    #0 0x72454aca7922 in strcpy ../../../../src/libsanitizer/asan/asan_interceptors.cpp:563
    #1 0x568b6866d2f4 in my_strdup dsa/03-debugging/c/05_heap_overflow_bug.c:23
    #2 0x568b6866d315 in main dsa/03-debugging/c/05_heap_overflow_bug.c:29
0x50200000001b is located 0 bytes after 11-byte region [0x502000000010,0x50200000001b)
allocated by thread T0 here:
    #1 0x568b6866d2aa in my_strdup dsa/03-debugging/c/05_heap_overflow_bug.c:21
=>0x502000000000: fa fa 00[03]fa fa ...
```

"WRITE of size 12 … 0 bytes after 11-byte region": copying 12 bytes
(`"camera-left"` + `'\0'`) into 11. Shadow `00 03` = 8 + 3 = 11 addressable
bytes. Fix: `malloc(n + 1)`. Without ASan, glibc usually survives this one (the
byte lands in alignment slack) — or aborts later with `malloc(): corrupted top
size`, far from the cause. That "crash later in malloc/free" pattern = heap
corruption → run ASan.

### Bug 5: uninitialised read — [c/06_uninit_bug.c](c/06_uninit_bug.c)

ASan and UBSan do **not** report it; the program just computes garbage:

```
$ make run P=dsa/03-debugging/c/06_uninit_bug.c
1920x1080 @ -1094795586 fps -> -65687735160 frames per minute
WRONG: expected 30 fps (0xbebebebe is ASan's malloc fill pattern)
```

`0xbebebebe` is a gift: ASan fills new heap blocks with `0xbe`, so a value
made of `be` bytes means "never initialised". Without ASan the field is
often 0 — different garbage, different behaviour (the classic "works in debug").
The tool for it is valgrind:

```
$ valgrind --track-origins=yes ./06_uninit_bug      (SAN=0 build)
==2345== Conditional jump or move depends on uninitialised value(s)
==2345==    at 0x1091F4: main (06_uninit_bug.c:43)
==2345==  Uninitialised value was created by a heap allocation
==2345==    at 0x4846828: malloc (vg_replace_malloc.c:442)
==2345==    by 0x109189: load_config (06_uninit_bug.c:29)
```

For **local** variables the compiler often catches it — but only with
optimisation: `int total; for (...) total += v[i];` gives nothing at `-O0 -Wall`
and `warning: 'total' may be used uninitialized [-Wmaybe-uninitialized]` at
`-O2 -Wall`. Fix: `calloc`, `= {0}` initialisers, or set every field.

### Bug 6: segfault on NULL — [c/07_null_deref_bug.c](c/07_null_deref_bug.c)

```
$ make run P=dsa/03-debugging/c/07_null_deref_bug.c
dsa/03-debugging/c/07_null_deref_bug.c:35:13: runtime error: member access within null pointer of type 'struct sensor'
```

UBSan names it before the access happens (and, with this repo's
`-fno-sanitize-recover=all`, stops the program there). With ASan alone
(`-fsanitize=address`) you get the fault itself:

```
==94432==ERROR: AddressSanitizer: SEGV on unknown address 0x000000000008 (pc 0x589cfddaf5a9 ...)
==94432==The signal is caused by a READ memory access.
==94432==Hint: address points to the zero page.
    #0 0x589cfddaf5a9 in read_temp dsa/03-debugging/c/07_null_deref_bug.c:35
    #1 0x589cfddaf7d8 in main dsa/03-debugging/c/07_null_deref_bug.c:42
```

The address `0x8` = NULL + `offsetof(struct sensor, value)`. **A fault address below 4096 is always a NULL pointer plus a
field offset** — the offset tells you which field. In gdb (no sanitizer needed):

```
(gdb) run
Program received signal SIGSEGV, Segmentation fault.
0x000055555555521a in read_temp (name=0x55555555600c "ddr") at .../07_null_deref_bug.c:35
35          return s->value;                               /* BUG: no NULL check */
(gdb) p s
$1 = (struct sensor *) 0x0
(gdb) p *s
Cannot access memory at address 0x0
(gdb) frame 1
(gdb) info locals
i = 2
wanted = {0x555555556004 "cpu", 0x555555556008 "gpu", 0x55555555600c "ddr"}
```

Post-mortem with a core dump: see §3.2. Fix: check the documented NULL
return; return an error code to the caller.

### Bug 7: double free — [cpp/01_double_free_bug.cpp](cpp/01_double_free_bug.cpp)

```
==96576==ERROR: AddressSanitizer: attempting double-free on 0x753f76a04800 in thread T0:
    #0 operator delete[](void*) ...
    #1 FrameBuffer::~FrameBuffer() dsa/03-debugging/cpp/01_double_free_bug.cpp:20
    #2 main dsa/03-debugging/cpp/01_double_free_bug.cpp:39
freed by thread T0 here:
    #1 FrameBuffer::~FrameBuffer() dsa/03-debugging/cpp/01_double_free_bug.cpp:20
    #2 main dsa/03-debugging/cpp/01_double_free_bug.cpp:37
previously allocated by thread T0 here:
    #1 FrameBuffer::FrameBuffer(unsigned long) dsa/03-debugging/cpp/01_double_free_bug.cpp:19
```

Same destructor, two different call sites (line 37: the by-value copy dies;
line 39: the original dies) → two objects shared one pointer → missing copy
constructor (rule of 3). Without ASan this one dies with `Segmentation
fault` (the 2 MB buffer was mmapped, and unmapping it twice faults); for a
small block glibc aborts with `free(): double free detected in tcache 2`. In
gdb, `bt` from the crash shows the second destructor. Fix: `FrameBuffer(const FrameBuffer&) = delete;`, pass by
`const&`, or hold a `std::vector<unsigned char>` (rule of 0).

### Bug 8: mismatched `new[]` / `delete` — [cpp/02_mismatched_delete_bug.cpp](cpp/02_mismatched_delete_bug.cpp)

```
==96633==ERROR: AddressSanitizer: attempting free on address which was not malloc()-ed: 0x50f000000048 in thread T0
    #0 operator delete(void*, unsigned long) ...
    #1 destroy dsa/03-debugging/cpp/02_mismatched_delete_bug.cpp:25
0x50f000000048 is located 8 bytes inside of 168-byte region [0x50f000000040,0x50f0000000e8)
allocated by thread T0 here:
    #0 operator new[](unsigned long) ...
    #1 make_sensors dsa/03-debugging/cpp/02_mismatched_delete_bug.cpp:23
SUMMARY: AddressSanitizer: bad-free
```

168 = 8 (array cookie holding n = 4) + 4 × 40 (`sizeof(Sensor)`). `new[]`
returned `block + 8`; `delete` handed that interior pointer to the allocator.
For types without a destructor (no cookie) ASan reports
`alloc-dealloc-mismatch (operator new [] vs operator delete)`. Fix: `delete[]`,
or better `std::vector<Sensor>` / `std::make_unique<Sensor[]>(4)`.

### Bug 9: dangling reference after vector growth — [cpp/03_dangling_reference_bug.cpp](cpp/03_dangling_reference_bug.cpp)

```
==96688==ERROR: AddressSanitizer: heap-use-after-free on address 0x502000000010 ...
WRITE of size 4 at 0x502000000010 thread T0
    #0 main dsa/03-debugging/cpp/03_dangling_reference_bug.cpp:23
0x502000000010 is located 0 bytes inside of 12-byte region [0x502000000010,0x50200000001c)
freed by thread T0 here:
    #0 operator delete(void*, unsigned long) ...
    #1 std::__new_allocator<int>::deallocate(int*, unsigned long) .../new_allocator.h:172
    #3 std::_Vector_base<int, std::allocator<int> >::_M_deallocate(int*, unsigned long) .../stl_vector.h:390
    (further down: std::vector<int>::push_back)
```

A 12-byte region = the old buffer of 3 ints, freed by the vector itself during
`push_back`. Any reference, pointer, iterator or `string_view` into a container
can dangle after an operation that may reallocate. Fix: index instead of
reference, `reserve` first, or re-take the reference after the push.

### Bug 10: undefined behaviour (UBSan) — [cpp/04_ubsan_bug.cpp](cpp/04_ubsan_bug.cpp)

`make run` stops at the first report (the repo builds with
`-fno-sanitize-recover=all`, so `make test` can never pass while UB happens):

```
$ make run P=dsa/03-debugging/cpp/04_ubsan_bug.cpp
dsa/03-debugging/cpp/04_ubsan_bug.cpp:24:43: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
```

Plain `-fsanitize=undefined` (recovering, no ASan) shows all four and the
"working" wrong results:

```
$ g++ -std=c++17 -g -fsanitize=undefined dsa/03-debugging/cpp/04_ubsan_bug.cpp -o /tmp/ub && /tmp/ub
04_ubsan_bug.cpp:24:43: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
04_ubsan_bug.cpp:25:54: runtime error: shift exponent 32 is too large for 32-bit type 'unsigned int'
04_ubsan_bug.cpp:33:54: runtime error: load of misaligned address 0x7fff5d567f71 for type 'const uint32_t', which requires 4 byte alignment
04_ubsan_bug.cpp:29:19: runtime error: index 4 out of bounds for type 'int [4]'
INT_MAX + 1  = -2147483648
1u << 32     = 1
u32 at buf+1 = 0x5040302
table[4]     = 720896
```

On x86 each of these "works" (INT_MIN, `1u << 32` gives 1 because x86 masks
the count to 5 bits, the misaligned load succeeds, `table[4]` reads a
neighbour). At `-O2` the optimiser may delete code that depends
on them, and on ARM some misaligned accesses (to device memory, or with
`ldrd`/`ldm` on 32-bit cores) fault. Fixes: `__builtin_add_overflow`, check
the shift count, bounds-check the index, `memcpy` for unaligned loads.

---

## 5. Complexity table (cost of each tool)

| Tool | Slowdown | Memory | Needs rebuild | Finds |
|---|---|---|---|---|
| `-Wall -Wextra` (+ `-O2`) | compile time only | — | yes | many bugs before running |
| `assert` | ~0 | 0 | yes | violated invariants |
| ASan (+LSan) | ~2× | ~3× | yes (`-fsanitize=address`) | overflow (heap/stack/global), UAF, double free, leaks |
| UBSan | ~1.2× | small | yes | signed overflow, bad shifts, misalignment, null member access, OOB on arrays |
| TSan | 5–15× | 5–10× | yes | data races |
| valgrind memcheck | 20–50× | 2× | no | invalid access, uninitialised use, leaks |
| gdb (breakpoints) | 0 until hit | — | `-g` | anything you can think of asking |
| gdb software watchpoint | 100×+ | — | `-g` | use hardware watchpoints (≤ 4) |
| strace | 1.5–100× (per syscall) | — | no | syscall failures, blocking, file paths |
| core dump | 0 at run time | disk = RSS | `-g` for symbols | post-mortem state |

---

## 6. C vs C++

| | C | C++ |
|---|---|---|
| typical memory bugs | manual free/leak, buffer overflows, string terminators | dangling references/iterators, rule-of-3 double frees, mismatched `delete[]`, `shared_ptr` cycles |
| crash location | usually near the bug (or in malloc) | often deep in STL templates: scroll to *your* first frame |
| gdb | `p *p`, `p arr[0]@n` | pretty printers, `catch throw`, `break Class::method`, `p *this` |
| sanitizer support | ASan/UBSan/LSan | + `alloc-dealloc-mismatch`, `new-delete-type-mismatch`, container-overflow annotations |
| prevention | discipline, static analysis | RAII, smart pointers, `std::vector::at()`, `-D_GLIBCXX_ASSERTIONS` (bounds-checked `operator[]`) |

---

## 7. Where it is used in the real world

* **Linux kernel**: KASAN (kernel ASan), UBSAN, KMEMLEAK, KCSAN (races),
  lockdep, `CONFIG_DEBUG_*`, `dump_stack()`, oops/panic backtraces decoded with
  `scripts/decode_stacktrace.sh`, kgdb over serial — the same ideas at kernel level.
* **Embedded boards**: `gdbserver` + `gdb-multiarch`, core dumps copied off the
  board, strace for device-node/firmware/ioctl problems, JTAG (TI XDS110 on
  the BeagleBone AI-64) for bootloader and R5F firmware debugging.
* **GStreamer**: `GST_DEBUG=*:3`, `GST_DEBUG_DUMP_DOT_DIR` pipeline graphs,
  `G_DEBUG=fatal-warnings` + gdb to stop on the first GLib critical.
* **CI pipelines**: every serious C/C++ project (Chromium, Firefox, LLVM,
  systemd) runs its test suite under ASan/UBSan/TSan; OSS-Fuzz fuzzes with ASan.

---

## 8. Common mistakes and pitfalls

1. Debugging an `-O2` binary and trusting "optimized out" values and line jumps.
2. Fixing the crash site instead of the corruption site (heap corruption crashes in `malloc`).
3. Reading only the last line of an ASan report — the first frames and the
   "freed by"/"allocated by" stacks are the answer.
4. Running valgrind on an ASan build (they conflict) — build with `SAN=0`.
5. Expecting a core file with `ulimit -c 0`, or looking in the cwd when apport/systemd took it.
6. `printf` debugging to stdout: the last lines are lost in the buffer when it crashes.
7. Side effects inside `assert`.
8. Ignoring warnings ("it compiles").
9. Changing several things at once — you no longer know which one fixed it.
10. "It works now" without understanding why — the bug is just hiding (Heisenbug).
11. Forgetting `ptrace_scope` when `gdb -p` fails.

---

## 9. Files in this lesson

| File | What it is | Run / finds with |
|---|---|---|
| [c/01_assert_and_logging.c](c/01_assert_and_logging.c) | log levels, location, timestamps, errno, hexdump, assert vs CHECK | `make run P=dsa/03-debugging/c/01_assert_and_logging.c` |
| [c/02_leak_bug.c](c/02_leak_bug.c) | **bug**: lost pointer + missing free on error path | `make run P=dsa/03-debugging/c/02_leak_bug.c` → LSan |
| [c/03_use_after_free_bug.c](c/03_use_after_free_bug.c) | **bug**: stale global pointer after free | `make run P=dsa/03-debugging/c/03_use_after_free_bug.c` → ASan |
| [c/04_stack_overflow_bug.c](c/04_stack_overflow_bug.c) | **bug**: recursion with a broken base case | `make run P=dsa/03-debugging/c/04_stack_overflow_bug.c` → ASan / gdb `bt -3` |
| [c/05_heap_overflow_bug.c](c/05_heap_overflow_bug.c) | **bug**: strdup without +1 | `make run P=dsa/03-debugging/c/05_heap_overflow_bug.c` → ASan |
| [c/06_uninit_bug.c](c/06_uninit_bug.c) | **bug**: malloc'd struct field never set | `make run P=dsa/03-debugging/c/06_uninit_bug.c` → garbage; valgrind |
| [c/07_null_deref_bug.c](c/07_null_deref_bug.c) | **bug**: unchecked NULL return | `make run P=dsa/03-debugging/c/07_null_deref_bug.c` → UBSan/ASan; gdb; core |
| [c/08_gdb_practice.c](c/08_gdb_practice.c) | correct program for breakpoints, watchpoints, x, attach (`--wait`) | `make run P=dsa/03-debugging/c/08_gdb_practice.c` |
| [c/09_syscalls_for_strace.c](c/09_syscalls_for_strace.c) | correct program with interesting syscalls | `make run P=dsa/03-debugging/c/09_syscalls_for_strace.c` |
| [cpp/01_double_free_bug.cpp](cpp/01_double_free_bug.cpp) | **bug**: shallow copy → double `delete[]` | `make run P=dsa/03-debugging/cpp/01_double_free_bug.cpp` → ASan |
| [cpp/02_mismatched_delete_bug.cpp](cpp/02_mismatched_delete_bug.cpp) | **bug**: `new[]` released with `delete` | `make run P=dsa/03-debugging/cpp/02_mismatched_delete_bug.cpp` → ASan |
| [cpp/03_dangling_reference_bug.cpp](cpp/03_dangling_reference_bug.cpp) | **bug**: reference into a vector across `push_back` | `make run P=dsa/03-debugging/cpp/03_dangling_reference_bug.cpp` → ASan |
| [cpp/04_ubsan_bug.cpp](cpp/04_ubsan_bug.cpp) | **bugs**: overflow, shift, misaligned load, OOB | `make run P=dsa/03-debugging/cpp/04_ubsan_bug.cpp` → UBSan (first one; see Bug 10 for all) |
| [cpp/05_catch_throw.cpp](cpp/05_catch_throw.cpp) | correct program for `catch throw`, STL pretty printing | `make run P=dsa/03-debugging/cpp/05_catch_throw.cpp` |

`make test` skips the `*_bug` programs (they are supposed to fail).

---

## 10. gdb cheat sheet

```
START / STOP                          BREAKPOINTS
gdb -q ./prog | --args ./prog a b     b func | b file.c:42 | b *0x4005d0
gdb -q -p PID    (attach)             b 42 if i == 7          (conditional)
gdb -q ./prog core                    tbreak func             (one-shot)
run [args] / r < input                rbreak ^parse_          (regex)
start  (break at main + run)          info b | disable 2 | enable 2 | delete [n]
kill | detach | quit                  condition 2 x > 5 | ignore 2 100
                                      commands 2 ... end      (auto-run commands)
STEPPING                              catch throw | catch syscall openat | catch signal
next (n) - over calls
step (s) - into calls                 WATCHPOINTS (who changed it?)
finish   - until return (shows value) watch var | watch -l ptr->field
until 50 - run to line 50             rwatch (read) | awatch (any)
continue (c) [N]                      
stepi / nexti - one instruction       STACK
advance func                          bt | bt full | bt -5
                                      frame N | up | down | info frame
DATA                                  info args | info locals
p expr | p/x n | p/t n | p/c n        thread apply all bt     (hangs, deadlocks)
p *ptr | p ptr->field | p arr         info threads | thread 3
p *arr@len    (array from a pointer)
p $rip | p $sp | info registers       MEMORY
ptype var | whatis var                x/16xb addr | x/4gx $sp | x/s str | x/10i $pc
display expr  (print at every stop)   info proc mappings      (= /proc/PID/maps)
set var x = 5                         info symbol 0x...       (address → symbol)
set print pretty on                   info line *0x...        (address → line)
call func(args)
                                      TUI: gdb -tui  | Ctrl-x a toggle | Ctrl-x 2 asm+src
C++: catch throw | b Class::method | p *this | info vtbl obj | set print object on
REMOTE: target remote host:2345 | set sysroot /path | (board) gdbserver :2345 ./prog
.gdbinit: set history save on | set pagination off | set print pretty on
```

---

## 11. Exercises

1. **(easy)** Fix each `*_bug` program, then re-run it with `make run P=...`
   until it is clean and exits 0. Do not look at the hints in the headers first —
   read the tool output.
2. **(easy)** In `08_gdb_practice`, use a conditional breakpoint to stop when
   `sum` first exceeds 1 000 000. Print `x`, `y`.
3. **(easy)** Run `09_syscalls_for_strace` under `strace -c` and under
   `strace -e trace=%file`. Which syscall fails, and with which errno?
4. **(medium)** Attach to `08_gdb_practice --wait`, set `g_keep_waiting = 0`,
   put a watchpoint on `frames[2].checksum`, continue and catch the write.
5. **(medium)** Build `05_heap_overflow_bug.c` with `SAN=0`. Does it crash?
   Change the string to 24 characters, then 40. When does glibc notice, and with
   which message? Explain using the chunk layout from 02-memory-management.
6. **(medium)** Produce a core dump of `04_stack_overflow_bug` (SAN=0) and
   find the recursion depth from the core alone.
7. **(hard)** Write a program with a data race (two threads incrementing a
   counter). Find it with `-fsanitize=thread` (needs a separate build without ASan).
8. **(hard)** Use `git bisect run` on a small repo where you deliberately
   introduce a bug three commits back.
9. **(hard)** Cross-compile `08_gdb_practice.c` for aarch64, run it under
   `gdbserver` on the BeagleBone AI-64, and debug it from the PC with `gdb-multiarch`.

---

## 12. Remember forever

```
Method: reproduce → observe (full message, ASan, bt) → hypothesise → test → fix cause → verify.
Build debug: -O0 -g + -fsanitize=address,undefined. Also try -O2 -Wall for more warnings.
ASan report: bug type → YOUR first frame → "N bytes after/inside region" → freed-by → allocated-by.
Fault address < 4096 = NULL + field offset.  Crash inside malloc/free = earlier heap corruption.
gdb: b / r / bt / frame / p / x/NFU / watch / finish / catch throw / thread apply all bt / -p PID.
Core: ulimit -c unlimited; check /proc/sys/kernel/core_pattern; gdb prog core; bt.
valgrind (SAN=0 only): uninitialised reads + leaks without recompiling. strace: what the kernel said.
assert = invariants only, vanishes with NDEBUG. Logs → stderr with time, level, file:line.
```

---

## 13. Self-test questions

1. What are the six steps of the debugging method? Why "reproduce" first?
2. Why can a heap overflow crash much later inside `free()`?
3. What are shadow memory, redzones and quarantine in ASan?
4. Read this aloud: "0 bytes after 11-byte region". What was the bug?
5. What does a SIGSEGV at address `0x10` tell you?
6. Which bugs does valgrind find that ASan does not, and vice versa?
7. Why does uninitialised memory read 0 without ASan and `0xbe...` with it?
8. How do you find *who* writes a corrupted struct field? (Exact gdb commands.)
9. Where does a core dump go on Ubuntu, and why is there none by default?
10. `gdb -p` says "Operation not permitted". Why, and what are your options?
11. Why is `assert(fd = open(...) >= 0)` a bug in release builds?
12. Which tool for: a hang? a "file not found" on the board? a slow memory growth? a race?

Review schedule: see [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).
