# 09: Kernel Debugging and Tracing

> In user space you start `gdb`, set a breakpoint and step. In the kernel
> the program you would stop is the one running the debugger, the bug may
> only show at 2 a.m. on a board in a box, and the evidence is usually one
> crash dump on a serial console. This lesson covers the kernel debugging
> toolbox, roughly in the order you reach for it: `printk` and dynamic
> debug, reading and decoding an **oops**, `WARN` vs `BUG`, saving crash
> logs on an embedded board (pstore/ramoops, kdump), **ftrace** and
> tracepoints (including your own `TRACE_EVENT`), **kprobes** and
> eBPF/bpftrace, the sanitizers and debug Kconfig options, and `kgdb`
> over the BeagleBone's serial console, JTAG and `perf`. Four modules give
> you something real to practise on.

---

## 1. Why this exists

A driver bug on the BeagleBone AI-64 looks like one of these:

- an **oops** on the serial console, then a dead or half-working board;
- a **`WARNING:`** with a stack trace that scrolls past at boot;
- a **hang**: no output, the heartbeat LED stops;
- **wrong behaviour** with no message: frames dropped, an I2C read
  returning stale data, latency spikes on the PREEMPT_RT kernel;
- **slow corruption** that crashes somewhere unrelated hours later.

Each needs a different tool. A stray `printk` finds the obvious bugs.
Latency spikes need ftrace. Corruption needs KASAN or KFENCE. A hang
needs lockdep, the soft/hard lockup detectors or `sysrq`. A crash that
happens once a week needs pstore, so the log survives the reboot. Knowing
which tool fits which symptom, and that the tool is in your kernel config
before you need it, is most of the skill.

---

## 2. Mental model

**Detective work at a crime scene you can't freeze.** `printk` is
interviewing witnesses: cheap, but witnesses change the story (timing) and
you must decide in advance whom to ask. **Tracing** is CCTV: it records
everything at low cost in a ring buffer and you rewind afterwards. An
**oops** is the body: one snapshot with the registers and the call stack,
and you work backwards to the cause. **Sanitizers** are tripwires installed
before the crime, which catch the culprit in the act. **kgdb/JTAG**
freezes the scene, but freezing the kernel also freezes timers, watchdogs
and the hardware it talks to.

```
      cost / intrusiveness  ------------------------------------------------>
 printk  pr_debug+dyndbg  tracepoints  ftrace  kprobes/eBPF  KASAN/lockdep  kgdb/JTAG
  always   off until        ~NOP when    func    any func,     rebuild,       stops
 compiled  enabled at       off, ring    entry/  no rebuild    2-3x slower,   the world
  in       run time         buffer       exit                  catch at once
      ^                                                                  ^
  "what happened?"                                           "why exactly here?"

  After the fact:  oops/WARN text -> decode_stacktrace.sh / faddr2line / gdb
                   pstore (ramoops) keeps the log across a reboot; kdump keeps memory
```

The sentence to remember: **make the kernel tell you (printk, WARN,
tracepoints), make it record (ftrace, pstore), make it check
(sanitizers, lockdep), and only stop it (kgdb, JTAG) when nothing else
works.**

---

## 3. How it works

### 3.1 printk in depth  (`dbg_demo.c`)

**Log levels.** Every message has a level, 0 (`KERN_EMERG`) to 7
(`KERN_DEBUG`). Use the `pr_*` wrappers, or `dev_*` in drivers, which
prefix the device name (`dev_err(&pdev->dev, …)` →
`ti-i2c 2000000.i2c: …`):

| Level | Macro | Meaning |
|---|---|---|
| 0 | `pr_emerg` | system is unusable |
| 1 | `pr_alert` | act immediately |
| 2 | `pr_crit` | critical condition |
| 3 | `pr_err` | error |
| 4 | `pr_warn` | warning |
| 5 | `pr_notice` | normal but significant |
| 6 | `pr_info` | informational |
| 7 | `pr_debug` / `dev_dbg` | debug (compiled out or dynamic) |

Everything goes into the kernel log buffer (`dmesg`, `/dev/kmsg`,
journald). Only messages with level **lower than `console_loglevel`**
are also written to the console, which on the board is the serial port
(slow: 115200 baud ≈ 11 KB/s, and on non-RT kernels it can stall the CPU
that prints).

```sh
cat /proc/sys/kernel/printk           # 4  4  1  7
                                      # console_loglevel, default msg level, min, boot default
echo 8 | sudo tee /proc/sys/kernel/printk   # everything to the console
sudo dmesg -n 1                              # only panics to the console
dmesg -l err,warn       # filter by level;  dmesg -w  follow;  dmesg -T  human time
# boot args: loglevel=8   quiet (=4)   ignore_loglevel   printk.time=1
```

`pr_fmt`: define `#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt` before the
includes and every `pr_*` in the file gets the module-name prefix (all
modules in this repo do).

**`pr_debug` and dynamic debug.** With `CONFIG_DYNAMIC_DEBUG=y` (the board
and Ubuntu have it) every `pr_debug`/`dev_dbg`/`print_hex_dump_debug` call
site is compiled in but **disabled**, at the cost of one static-key NOP. You
switch sites on at run time, selecting by module, file, function, line or
format:

```sh
sudo cat /sys/kernel/debug/dynamic_debug/control | grep dbg_demo
# .../dbg_demo.c:99 [dbg_demo]dbg_demo_target =_ "target(%u) -> %08x, call %llu\n"
echo 'module dbg_demo +p'                 | sudo tee /sys/kernel/debug/dynamic_debug/control
echo 'func dbg_demo_target +pfl'          | sudo tee /sys/kernel/debug/dynamic_debug/control
                                          # p=print f=function name l=line m=module t=thread id
echo 'file drivers/i2c/busses/i2c-omap.c +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
echo 'module dbg_demo -p'                 | sudo tee /sys/kernel/debug/dynamic_debug/control
sudo insmod dbg_demo.ko dyndbg=+p         # per-module, at load time
# boot args (built-in code, early boot):  dyndbg="file drivers/base/dd.c +p"
#                                         i2c_omap.dyndbg=+p     (module name.dyndbg)
```

Without `CONFIG_DYNAMIC_DEBUG`, `pr_debug` only prints if the file is
compiled with `-DDEBUG` (`ccflags-y += -DDEBUG` in the Kbuild file).

**Rate limiting.** A message in an interrupt handler that fires 10 000
times a second will flood the serial console and stall the board. Use
`pr_err_ratelimited()` / `dev_warn_ratelimited()` (default: 10 messages per
5 s per call site) or your own state:

```c
static DEFINE_RATELIMIT_STATE(rs, HZ, 3);      /* 3 per second */
if (__ratelimit(&rs))
        pr_info("...");                        /* later: "N callbacks suppressed" */
```

`printk_once()` / `pr_warn_once()` print once per boot. `dbg_demo` checks
that 100 attempts through a burst-3 limiter print 3 (up to 6 if the loop
straddles a window boundary).

**Dumping bytes.** `print_hex_dump(KERN_INFO, "prefix: ", DUMP_PREFIX_OFFSET,
16, 1, buf, len, true)` prints 16-byte rows with ASCII;
`print_hex_dump_debug()` is a dyndbg site; `%*ph` prints up to 64 bytes
inline (`pr_info("%*ph\n", 4, buf)` → `88 b5 00 14`).

**Pointer formats** (`Documentation/core-api/printk-formats.rst`):

| Specifier | Prints | Note |
|---|---|---|
| `%p` | hashed value, e.g. `000000004c8d2a1f` | the default since 4.15: no address leak to logs; same pointer, same hash |
| `%px` | raw address | only when you really need it; checkpatch warns; never in production paths |
| `%pK` | raw, or zeros, per `kptr_restrict` | for files root may read |
| `%pS` / `%ps` | `dbg_demo_target+0x0/0x50 [dbg_demo]` / `dbg_demo_target` | code addresses; `%pS` of `__builtin_return_address(0)` = your caller |
| `%pB` | like `%pS` for return addresses in backtraces | |
| `%pe` | `-ENOMEM` for an `ERR_PTR` | `pr_err("...: %pe\n", ERR_PTR(err))` |
| `%pOF` | `/bus@100000/i2c@2000000` | device-tree node full name |
| `%pa` / `%pad` | `phys_addr_t` / `dma_addr_t` (pass a pointer to it) | |
| `%pI4` / `%pM` | IPv4 address / MAC address | |
| `%*pbl` | `0-1,3,7` | bitmaps and cpumasks |
| `%pV` | a `struct va_format` | wrappers such as `check()` in every module here |

Hashing explains the "why are all my pointers different every boot and
never match `/proc/kallsyms`?" moment: debug with `%pS`, or `%px`
temporarily. Boot with `no_hash_pointers` on a dev board to print raw `%p`
everywhere.

### 3.2 Reading an oops  (`oops_demo_bug.c`, `BUGS=1`)

**BUG, WARN, Oops, panic:**

| Event | What triggers it | What happens next |
|---|---|---|
| `WARN()` / `WARN_ON(cond)` / `WARN_ON_ONCE` | code says "this shouldn't happen, but I can go on" | `WARNING: CPU: 1 PID: … at file:line func`, registers + call trace, taint `W`, **execution continues** |
| **Oops** | the CPU faulted in kernel mode: NULL/bad pointer, undefined instruction, alignment | registers, call trace, taint `D`; the *current task is killed*, locks it held stay held, the kernel limps on |
| `BUG()` / `BUG_ON(cond)` | code says "impossible, can't continue" | `kernel BUG at file:line!` then an Oops |
| `panic()` | Oops in interrupt context, Oops with `panic_on_oops=1`, fatal errors | `Kernel panic - not syncing: …`, stop or reboot after `panic=N` seconds |

**Don't use `BUG_ON()` in drivers.** Killing the machine because one
device misbehaves is almost never right (checkpatch says so). Use
`WARN_ON_ONCE()` and return an error. Embedded products often set
**`panic_on_oops=1`** and `panic=5` (reboot after 5 s) with a hardware
watchdog, because a half-dead kernel after an oops is worse than a reboot.
Some also set `panic_on_warn=1` in testing to catch every WARN.

**Anatomy of an arm64 oops.** This is the NULL dereference from
`oops_demo_bug` (`echo 1 > …/parameters/trigger`), shown on the board's
6.18 kernel. **It is an illustrative example**: the function offsets
and line numbers were taken from the cross-built module, but the
addresses, registers and timestamps are made up.

```
 1  Unable to handle kernel NULL pointer dereference at virtual address 0000000000000010
 2  Mem abort info:
 3    ESR = 0x0000000096000004
 4    EC = 0x25: DABT (current EL), IL = 32 bits
 5    SET = 0, FnV = 0
 6    EA = 0, S1PTW = 0
 7    FSC = 0x04: level 0 translation fault
 8  Data abort info:
 9    ISV = 0, ISS = 0x00000004, ISS2 = 0x00000000
10    CM = 0, WnR = 0, TnD = 0, TagAccess = 0
11  user pgtable: 4k pages, 48-bit VAs, pgdp=0000000883a1c000
12  [0000000000000010] pgd=0000000000000000, p4d=0000000000000000
13  Internal error: Oops: 0000000096000004 [#1]  SMP
14  Modules linked in: oops_demo_bug(O) ... ti_am335x_adc
15  CPU: 1 UID: 0 PID: 1874 Comm: tee Tainted: G           O        6.18.38-arm64-k3-r45 #1 PREEMPT_RT
16  Tainted: [O]=OOT_MODULE
17  Hardware name: BeagleBoard.org BeagleBone AI-64 (DT)
18  pstate: 60400005 (nZCv daif +PAN -UAO -TCO -DIT -SSBS BTYPE=--)
19  pc : widget_read_value+0x8/0x18 [oops_demo_bug]
20  lr : widget_process+0x38/0x74 [oops_demo_bug]
21  sp : ffff8000837b3c90
22  x29: ffff8000837b3c90 x28: ffff000802a45a00 x27: 0000000000000000
23  ...
24  x2 : 0000000000000000 x1 : 0000000000000001 x0 : 0000000000000000
25  Call trace:
26   widget_read_value+0x8/0x18 [oops_demo_bug] (P)
27   widget_process+0x38/0x74 [oops_demo_bug]
28   trigger_set+0xb0/0xc8 [oops_demo_bug]
29   param_attr_store+0xa4/0x130
30   module_attr_store+0x24/0x40
31   sysfs_kf_write+0x4c/0x68
32   kernfs_fop_write_iter+0x128/0x1c8
33   vfs_write+0x2b4/0x3a0
34   ksys_write+0x70/0x108
35   __arm64_sys_write+0x20/0x30
36   invoke_syscall+0x50/0x120
37   el0_svc_common.constprop.0+0x48/0xf0
38   do_el0_svc+0x24/0x38
39   el0_svc+0x34/0xf0
40   el0t_64_sync_handler+0x10c/0x138
41   el0t_64_sync+0x1b0/0x1b8
42  Code: d503201f d503201f d503201f b9401000 (d65f03c0)   <- sample; the faulting word is in ()
43  ---[ end trace 0000000000000000 ]---
```

Reading it line by line:

- **1**: the kind of fault and the **faulting address**: `0x10`. A small
  address means "NULL pointer plus a field offset". In `struct esh_widget`,
  `value` is at offset `0x10` (two `u64`s before it), so the code read
  `w->value` with `w == NULL`. Addresses like `dead000000000100` /
  `dead000000000122` mean a deleted `list_head` (`LIST_POISON1/2`), and
  `6b6b6b6b…` is freed slab memory (`POISON_FREE`): a use-after-free.
- **2–10**: arm64 exception syndrome. `EC = 0x25` = data abort from the
  kernel itself (current EL); `WnR = 0` = it was a **read** (1 = write);
  `FSC = 0x04` = translation fault (nothing mapped there). x86 prints
  `#PF: supervisor read access in kernel mode` instead.
- **11–12**: the page-table walk for that address: empty.
- **13**: `Oops: <ESR> [#1]`: the first oops since boot. Later ones (`#2`)
  may be follow-on damage: **always debug the first one**.
- **14**: modules loaded. Flags after a name: `(O)` out-of-tree, `(E)`
  unsigned, `(P)` proprietary, `(+)`/`(-)` being loaded/unloaded.
- **15–16**: which CPU, PID and command (`tee`, the process writing the
  parameter: it gets killed), the **taint flags**, the exact kernel
  version. Taints: `G` all GPL, `P` proprietary module, `O` out-of-tree
  module, `E` unsigned, `W` a WARN happened earlier, `D` an oops happened
  earlier, `C` staging driver, `T` built with a different struct layout.
  A `W` or `D` means **look further up the log first**. Decode it with
  `cat /proc/sys/kernel/tainted` and `tools/debugging/kernel-chktaint`.
- **17**: board name from the device tree.
- **18**: processor state (condition flags, `daif` = interrupt masks).
- **19–20**: **`pc`** = the faulting instruction, **`lr`** = link register
  = the return address into the caller. Format: `function+offset/size
  [module]`. `widget_read_value+0x8/0x18` means 8 bytes into a function
  that is 0x18 bytes long.
- **21–24**: registers. On arm64, arguments come in `x0..x7` and the return
  value goes out in `x0`; `x29` = frame pointer, `x30` = lr. Here `x0 = 0`:
  the first argument, `w`, was NULL. Cross-check it with the disassembly:
  `ldr w0, [x0, #16]` (`b9401000`) loads from `x0 + 0x10`.
- **25–41**: the **call trace**, innermost first. Read it bottom-up to see
  the story: a user-space `write()` syscall (`el0_svc` … `vfs_write`) to a
  sysfs file (`kernfs_fop_write_iter` → `sysfs_kf_write`) that is a module
  parameter (`param_attr_store`) called our `trigger_set` → `widget_process`
  → `widget_read_value`. Entries prefixed with `?` (x86) are unreliable
  guesses found on the stack. `(P)` marks the frame the PC was in.
- **42**: `Code:` the instruction bytes around `pc`, the faulting one in
  `( )` (`<>` on x86). `scripts/decodecode` turns them into assembly.

On **x86** the same information reads `BUG: kernel NULL pointer
dereference, address: 0000000000000010`, `RIP: 0010:widget_read_value+0x4/0x10
[oops_demo_bug]`, registers `RAX…R15`, and the call trace looks the same.

### 3.3 Decoding the oops to source lines

`function+offset` → file:line needs debug info. The board's kernel is built
**without** `CONFIG_DEBUG_INFO`, so this lesson's Makefile adds `-g` for
`oops_demo_bug.o` (look for `CFLAGS_oops_demo_bug.o`). For your own modules,
add `ccflags-y += -g` in the same way, or build the kernel with
`CONFIG_DEBUG_INFO_DWARF5=y` to get `vmlinux` debug info for the core-kernel
frames too.

**`scripts/faddr2line`**: the most reliable for modules (verified on this
module, cross-built for the board):

```sh
BB=<path to beaglebone-ai64>/sources/arm64-multiplatform
export CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
$BB/KERNEL/scripts/faddr2line kernel/oops_demo_bug.ko \
        widget_read_value+0x8/0x18 widget_process+0x38/0x74 trigger_set+0xb0/0xc8
# widget_read_value+0x8/0x18:
# widget_read_value at .../oops_demo_bug.c:44      <- return READ_ONCE(w->value);
# widget_process+0x38/0x74:
# widget_process at .../oops_demo_bug.c:54         <- v = widget_read_value(READ_ONCE(widget));
# trigger_set+0xb0/0xc8:
# trigger_set at .../oops_demo_bug.c:77            <- pr_info("value %u\n", widget_process(how));
```

It checks the `/size` against the symbol table and refuses a mismatch
("size mismatch"), so a `.ko` that doesn't match the running one can't
mislead you. For a symbol in the core kernel: `faddr2line vmlinux
vfs_write+0x2b4/0x3a0`.

**`scripts/decode_stacktrace.sh`**: paste the whole oops and get
`file:line` (and inlined frames) added to every line:

```sh
sudo dmesg > oops.txt          # or the pstore file, or the serial console log
$BB/KERNEL/scripts/decode_stacktrace.sh $BB/KERNEL/vmlinux $BB/KERNEL \
        $PWD/kernel < oops.txt  # args: vmlinux, kernel source dir, where the .ko files are
#  widget_read_value (.../oops_demo_bug.c:44) oops_demo_bug
```

**gdb on the `.ko`** (`gdb-multiarch` for an arm64 module on an x86 PC):

```sh
gdb-multiarch kernel/oops_demo_bug.ko
(gdb) list *(widget_read_value+0x8)       # source around the faulting line
(gdb) disassemble /s widget_read_value    # interleaved source + asm
(gdb) info line *(widget_process+0x38)
```

Careful: in an unlinked `.ko`, `.text`, `.init.text` and `.exit.text` all
start at address 0, so `list *(func+off)` can report a line in the
*wrong function* (on the host build it put `widget_read_value+0x4` into
`oops_demo_exit`). Check the function name gdb prints, or use
`faddr2line`, which handles sections. To get the real load addresses
into gdb: `sudo cat /sys/module/oops_demo_bug/sections/.text` on the target,
then `add-symbol-file oops_demo_bug.ko <that address>`.

**`addr2line -fip -e x.ko 0xOFFSET`** works for simple cases (the offset
is the section-relative address from `nm` plus `+off`).

### 3.4 Keeping crash logs: pstore/ramoops, kdump

On a board without a permanently attached serial console, the oops
disappears with the reboot. Options:

- **pstore + ramoops** (the board kernel has `CONFIG_PSTORE_RAM=m`):
  reserve some RAM that survives a warm reset; on oops/panic the kernel log
  is written there, and after reboot it appears as files:
  ```
  /* device tree: reserved-memory node */
  ramoops@9ff00000 {
          compatible = "ramoops";
          reg = <0x0 0x9ff00000 0x0 0x100000>;   /* 1 MiB, pick a free region */
          record-size = <0x20000>; console-size = <0x20000>; pmsg-size = <0x20000>;
  };
  ```
  or without a DT change (module parameters, 1 MiB at a free address that
  `/proc/iomem` shows as RAM you have reserved with `memmap=`/`mem=`):
  `modprobe ramoops mem_address=0x9ff00000 mem_size=0x100000 record_size=0x20000`.
  After the crash and a warm reboot: `ls /sys/fs/pstore/` →
  `dmesg-ramoops-0` (the oops), `console-ramoops-0` (the previous boot's
  console). It survives a reset but not a power cut (DRAM loses its
  contents), so test with `echo c > /proc/sysrq-trigger` followed by a
  watchdog/warm reset. Pick the address from the board's memory map so
  that U-Boot doesn't overwrite it.
- **kdump**: a second "crash kernel" is preloaded (`crashkernel=256M`,
  `kexec -p`). On panic it boots and saves the whole memory of the dead
  kernel as `/proc/vmcore`, which you analyse with the `crash` tool. That
  is heavy for a 4 GiB board but standard on servers.
- **netconsole**: send printk over UDP to another machine
  (`netconsole=@/eth0,6666@192.168.1.10/`), useful when the serial port is
  in use.
- **SysRq**: `echo t > /proc/sysrq-trigger` (all task stacks), `w` (blocked
  tasks), `l` (backtrace on all CPUs), `c` (crash now, to test pstore).
  Over the serial console: send BREAK, then the letter.

### 3.5 ftrace  (`dbg_demo` as the target)

ftrace is the kernel's built-in tracer, controlled through **tracefs** at
`/sys/kernel/tracing` (also `/sys/kernel/debug/tracing` for compatibility).
Compiled with `-pg`/`-mfentry` (patchable-function-entry on arm64),
every traceable function starts with NOPs. Enabling a tracer patches them
into calls at run time, so there is zero cost while it's off.

```sh
cd /sys/kernel/tracing                         # as root
cat available_tracers                          # function function_graph wakeup_rt irqsoff ... nop
grep dbg_demo available_filter_functions       # dbg_demo_target [dbg_demo], dbg_demo_helper [dbg_demo]

echo dbg_demo_target > set_ftrace_filter       # only this function (wildcards ok: 'dbg_demo_*')
echo function > current_tracer
echo 3 > /sys/kernel/debug/dbg_demo/poke       # or load with period_ms=500
cat trace
#   tracer: function
#           TASK-PID     CPU#  |||||  TIMESTAMP  FUNCTION
#              | |         |   |||||     |         |
#             tee-1912    [000] .....  2481.330141: dbg_demo_target <-poke_write
#             tee-1912    [000] .....  2481.330152: dbg_demo_target <-poke_write

echo function_graph > current_tracer
echo dbg_demo_target > set_graph_function      # graph starting at this function
echo 1 > /sys/kernel/debug/dbg_demo/poke
cat trace
#  0)               |  dbg_demo_target [dbg_demo]() {
#  0)               |    dbg_demo_helper [dbg_demo]() {
#  0)   0.980 us    |      __kmalloc_cache_noprof();
#  0)   0.410 us    |      kfree();
#  0)   2.870 us    |    }
#  0)   3.940 us    |  }

echo nop > current_tracer; echo > set_ftrace_filter; echo > set_graph_function   # reset
```

- `trace` is a snapshot you read; `trace_pipe` is a consuming, blocking
  stream. `tracing_on` (0/1) pauses recording without losing the buffer.
  `buffer_size_kb` sets the per-CPU ring size. `set_ftrace_pid` limits it to
  one process. `options/func_stack_trace` adds a stack trace per hit.
- `irqsoff`, `preemptoff` and `wakeup_rt` tracers measure the longest
  interrupts-off or preempt-off section and the worst RT wake-up latency.
  That is how you hunt latency on the PREEMPT_RT board (with `cyclictest`
  from [linux-internals/15](../../linux-internals/15-scheduling-and-real-time/)).
- **`trace_printk()`** is a `printk` into the ftrace buffer: microseconds
  cheaper, no console, and safe in hot paths. Use it for temporary
  debugging only: a module that contains it makes the kernel print a big
  "trace_printk() being used" warning banner at load, which is why none of
  this lesson's modules use it.
- `ftrace_dump_on_oops` (boot arg or `/proc/sys/kernel/ftrace_dump_on_oops`)
  dumps the trace buffer to the console when the kernel oopses: the last
  milliseconds before the crash.

**trace-cmd and KernelShark** are front-ends to the same files:

```sh
sudo trace-cmd record -p function_graph -g dbg_demo_target sleep 2
sudo trace-cmd record -e sched_switch -e irq -e esh_trace sleep 5   # events
trace-cmd report | head
kernelshark trace.dat           # GUI timeline per CPU (copy trace.dat to the PC)
```

### 3.6 Tracepoints and your own `TRACE_EVENT`  (`trace_demo.c`, `trace_demo_trace.h`)

**Tracepoints** are static hooks placed by kernel developers at
interesting spots (`sched:sched_switch`, `irq:irq_handler_entry`,
`i2c:i2c_write`, `block:block_rq_issue`, `kmem:kmalloc`), with typed
fields. There are thousands:

```sh
ls /sys/kernel/tracing/events/                 # subsystems
cat /sys/kernel/tracing/events/i2c/i2c_result/format    # field layout
echo 1 > /sys/kernel/tracing/events/i2c/enable           # all i2c events
echo 'adapter_nr == 2' > /sys/kernel/tracing/events/i2c/i2c_write/filter
cat /sys/kernel/tracing/trace_pipe
# boot arg for early boot: trace_event=i2c:*,sched:sched_switch
```

**Your own.** A trace header declares the events with `TRACE_EVENT()`:
prototype, ring-buffer record layout, assignment and print format:

```c
TRACE_EVENT(esh_tick,
        TP_PROTO(u64 seq, unsigned int period_ms, s64 late_us),
        TP_ARGS(seq, period_ms, late_us),
        TP_STRUCT__entry(__field(u64, seq) __field(unsigned int, period_ms)
                         __field(s64, late_us)),
        TP_fast_assign(__entry->seq = seq; __entry->period_ms = period_ms;
                       __entry->late_us = late_us;),
        TP_printk("seq=%llu period_ms=%u late_us=%lld",
                  __entry->seq, __entry->period_ms, __entry->late_us));
```

The mechanics that trip everybody up:

1. **`#define TRACE_SYSTEM esh_trace`** names the directory under
   `events/`.
2. The include guard is `#if !defined(_X_H) || defined(TRACE_HEADER_MULTI_READ)`,
   because the header is read several times with different macro
   definitions.
3. **Exactly one `.c` file** does `#define CREATE_TRACE_POINTS` before
   `#include "trace_demo_trace.h"`. That instance generates the code.
4. After the guard, `TRACE_INCLUDE_PATH .` + `TRACE_INCLUDE_FILE trace_demo_trace`
   + `#include <trace/define_trace.h>`. define_trace.h includes the
   header again *by path relative to the kernel's `include/trace/`
   directory*, so for an out-of-tree module **the Makefile must add
   `CFLAGS_trace_demo.o := -I$(src)`** (or `ccflags-y += -I$(src)`).
   Without it: `fatal error: ./trace_demo_trace.h: No such file or directory`.
5. Prefer fixed-size fields. `__string()`/`__assign_str()` are the way to
   store strings, but `__assign_str(dst, src)` became `__assign_str(dst)` in
   6.10, so this lesson uses a `__array(char, who, 16)` to build on both
   6.8 and 6.18.

Calling `trace_esh_tick(seq, p, late_us)` costs a NOP while the event is
off. `trace_esh_tick_enabled()` lets you skip expensive argument
preparation. `TRACE_EVENT_CONDITION` (`esh_late`, `late_us > 1000`)
records only when the condition holds. `DECLARE_EVENT_CLASS` +
`DEFINE_EVENT` share one layout among many events (that's how the
`sched_*` events are written). See `samples/trace_events/` in the kernel
tree for every feature.

```sh
sudo insmod trace_demo.ko period_ms=100
echo 1 | sudo tee /sys/kernel/tracing/events/esh_trace/enable
sudo cat /sys/kernel/tracing/trace_pipe
#     esh_trace-2051    [001] .....  3120.512330: esh_tick: seq=41 period_ms=100 late_us=72
#     esh_trace-2051    [001] .....  3120.616411: esh_tick: seq=42 period_ms=100 late_us=81
#     esh_trace-2051    [000] .....  3120.720398: esh_tick: seq=43 period_ms=100 late_us=67
echo 5 | sudo tee /sys/module/trace_demo/parameters/period_ms    # faster; 0 or 20000 -> EINVAL
echo 'late_us > 500' | sudo tee /sys/kernel/tracing/events/esh_trace/esh_tick/filter
sudo cat /sys/kernel/tracing/events/esh_trace/esh_tick/format
```

`late_us` is how much later than requested the kthread woke up. The sleep
is jiffy-based (`CONFIG_HZ`, 4 ms on a 250 Hz kernel), so expect values up
to one jiffy, plus scheduling latency; on the board, load the CPUs
(`stress-ng --cpu 2`) and look for `esh_late` events.

### 3.7 kprobes and eBPF  (`kprobe_demo.c`)

A **kprobe** puts a breakpoint at (almost) any kernel instruction at run
time; you get a callback with the registers. A **kretprobe** also catches
the return (value in `regs_return_value()`). `kprobe_demo` counts calls to
a function (`symbol=`, default `vfs_read`), times each call, and counts
negative returns:

```sh
sudo insmod kprobe_demo.ko                    # symbol=vfs_read
sudo cat /sys/kernel/debug/kprobe_demo/stats
# symbol:   vfs_read
# calls:    48211
# returns:  48207 (311 negative = errors)
# avg ns:   5210
# max ns:   18432077                <- a read() that slept (pipe/tty)
# missed:   kprobe 0, kretprobe 0
sudo rmmod kprobe_demo
sudo insmod kprobe_demo.ko symbol=no_such_fn   # -> insmod fails, dmesg explains
# kprobe_demo: register_kprobe(no_such_fn) failed: -ENOENT (symbol not found / inlined)
```

Rules: handlers run in atomic context (no sleeping, no mutex, minimal
work). Some functions can't be probed (kprobe internals, `notrace`, entry
code; see `/sys/kernel/debug/kprobes/blacklist`). A static function that
the compiler inlined doesn't exist as a symbol. `maxactive` limits
how many calls of a kretprobe'd function can be pending at once, and
`nmissed` counts the overflow. Unregistering waits until no CPU is in your
handlers. Without writing a module: `kprobe_events` in tracefs:

```sh
echo 'p:myprobe vfs_read count=%x2' | sudo tee /sys/kernel/tracing/kprobe_events   # arm64: x2 = 3rd arg
echo 'r:myret vfs_read ret=$retval'  | sudo tee -a /sys/kernel/tracing/kprobe_events
echo 1 | sudo tee /sys/kernel/tracing/events/kprobes/enable
sudo cat /sys/kernel/tracing/trace_pipe
echo 0 | sudo tee /sys/kernel/tracing/events/kprobes/enable; echo | sudo tee /sys/kernel/tracing/kprobe_events
```

**eBPF / bpftrace** do the same safely from user space: the program is
verified, JIT-compiled, attached to a kprobe/tracepoint, and aggregates
in kernel maps. That is the modern way to answer "how often / how long /
who" in production. It needs `CONFIG_BPF_SYSCALL`, `CONFIG_DEBUG_INFO_BTF`
for the nicest features, and `bpftrace` installed (on the board: `apt
install bpftrace`, if the kernel has BTF):

```sh
sudo bpftrace -e 'kprobe:vfs_read { @[comm] = count(); }'                 # reads per process
sudo bpftrace -e 'kretprobe:vfs_read /retval < 0/ { @err[comm, retval] = count(); }'
sudo bpftrace -e 'kprobe:vfs_read { @start[tid] = nsecs; }
                  kretprobe:vfs_read /@start[tid]/ { @us = hist((nsecs - @start[tid]) / 1000); delete(@start[tid]); }'
sudo bpftrace -e 'tracepoint:esh_trace:esh_tick { @late = hist(args->late_us); }'   # your own event
sudo bpftrace -e 'tracepoint:irq:irq_handler_entry { @[str(args->name)] = count(); }'
sudo bpftrace -l 'kprobe:i2c_*'                                           # list probe points
```

`perf probe`, `perf trace` and the BCC tools (`funccount`, `funclatency`,
`biolatency`, `opensnoop`) work the same way underneath.

### 3.8 debugfs as a debug interface

Exposing internal state in debugfs (register dumps, queue depths, error
counters, knobs that force error paths) costs a few lines and saves days.
`dbg_demo` has `/sys/kernel/debug/dbg_demo/{poke,warn,calls}`,
`kprobe_demo` has `stats`. How to write debugfs files properly
(`debugfs_create_*`, custom `seq_file` files, removal) is in
[05 sysfs, procfs, debugfs](../05-sysfs-procfs-debugfs/).

### 3.9 Debug kernels: sanitizers and debug options

Build a separate **debug kernel** (for a QEMU arm64 VM, or as a second
kernel on the board) with the checkers enabled. They slow the kernel down
and some exclude each other, so a production kernel has none of them:

| Option | Catches | Cost |
|---|---|---|
| `CONFIG_KASAN` (generic; `KASAN_SW_TAGS`/`HW_TAGS` on arm64) | out-of-bounds, use-after-free, double free, at the faulting access, with alloc/free stacks | ~2–3× slower, 1/8 of RAM for shadow (generic) |
| `CONFIG_KFENCE` | the same, by **sampling** (one object in a guarded page now and then) | ~0: fine in production |
| `CONFIG_UBSAN` (+ `UBSAN_BOUNDS`, `UBSAN_SHIFT`) | undefined behaviour: array index out of bounds, bad shifts, signed overflow (opt-in) | small |
| `CONFIG_KCSAN` | data races (concurrent unmarked accesses) | big; not with KASAN |
| `CONFIG_PROVE_LOCKING` (lockdep) | lock-order inversions (potential deadlocks), IRQ-unsafe locking, recursive locking, before they happen | big |
| `CONFIG_DEBUG_ATOMIC_SLEEP` | sleeping in atomic context (`mutex_lock`/`GFP_KERNEL` under a spinlock) | small |
| `CONFIG_DEBUG_KMEMLEAK` | leaks (see [02](../02-kernel-memory-and-data-structures/)) | memory + periodic scans |
| `CONFIG_DEBUG_OBJECTS` (`_TIMERS`, `_WORK`, `_RCU_HEAD`, `_FREE`) | freeing an active timer/work, double init, use after free of these objects | small–medium |
| `CONFIG_DEBUG_LIST`, `CONFIG_DEBUG_SG` | corrupted list pointers, bad scatterlists | small |
| `CONFIG_SLUB_DEBUG` + `slub_debug=FZPU` | red zones, poisoning, sanity checks, owner tracking (no rebuild: boot arg) | medium |
| `CONFIG_DEBUG_PAGEALLOC` | use of freed pages (unmaps them) | big |
| `CONFIG_SOFTLOCKUP_DETECTOR`, `HARDLOCKUP_DETECTOR`, `DETECT_HUNG_TASK` | CPUs stuck in kernel with/without interrupts, tasks in D state > 120 s | ~0 |
| `CONFIG_DEBUG_INFO_DWARF5` / `_BTF` | file:line decoding, gdb, BTF for bpftrace/CO-RE | build time, size |
| `CONFIG_FAULT_INJECTION` (`FAILSLAB`, `FAIL_PAGE_ALLOC`) | makes allocations fail on purpose to test error paths | ~0 when off |

Recipe for the VM: `make defconfig`, then `scripts/config -e KASAN -e
PROVE_LOCKING -e DEBUG_ATOMIC_SLEEP -e DEBUG_KMEMLEAK -e DEBUG_INFO_DWARF5
-e DEBUG_OBJECTS_TIMERS`, then `make olddefconfig`. The repo's other
lessons point here for their bug modules:
[03 ksync_bug.c](../03-kernel-synchronization/) (lockdep,
atomic-sleep) and [02 kmem_leak_bug.c](../02-kernel-memory-and-data-structures/)
(kmemleak). User-space equivalents (ASan, UBSan, TSan, valgrind, gdb) are
in [dsa/03 debugging](../../dsa/03-debugging/).

### 3.10 kgdb / kdb over the serial console, JTAG

**kgdb** lets `gdb` on your PC debug the running kernel over the serial
port: breakpoints, single-step, inspect variables. The whole board stops
while you're at the gdb prompt. Kernel config: `CONFIG_KGDB`,
`CONFIG_KGDB_SERIAL_CONSOLE`, `CONFIG_KGDB_KDB` (the on-target shell),
`CONFIG_DEBUG_INFO`, `CONFIG_FRAME_POINTER`, and ideally
`CONFIG_STRICT_KERNEL_RWX=n` (software breakpoints need writable text) or
hardware breakpoints.

```sh
# 1. On the board, share the debug UART between console and kgdb.
#    The AI-64's debug UART is usually ttyS2 — check your bootargs console=
#    (cat /proc/cmdline) and use that name.
echo ttyS2,115200 | sudo tee /sys/module/kgdboc/parameters/kgdboc
#    or at boot:  kgdboc=ttyS2,115200 kgdbwait   (wait for gdb before init runs)
# 2. Stop the kernel:
echo g | sudo tee /proc/sysrq-trigger           # the console now shows the kdb prompt
#    kdb> bt   ps   lsmod   dmesg   go          (kdb works without a PC)
# 3. Close your serial terminal (picocom/minicom) and attach gdb on the PC:
gdb-multiarch $BB/KERNEL/vmlinux
(gdb) set serial baud 115200
(gdb) target remote /dev/ttyUSB0
(gdb) lx-symbols kernel/                       # with scripts/gdb helpers: load module symbols
(gdb) break dbg_demo_target
(gdb) continue
```

The console and gdb can't share the line at the same time; `agent-proxy`
splits one serial port into a console and a gdb port. For modules, gdb
must know the section addresses (`lx-symbols` from
`CONFIG_GDB_SCRIPTS`, or `add-symbol-file` with `/sys/module/<m>/sections/.text`).

**JTAG.** The TDA4VM has a JTAG/debug port (the AI-64 has a TAG-Connect
footprint). With an XDS110/XDS560 probe and TI **Code Composer Studio**,
or **OpenOCD** + gdb, you can halt the A72 cores even when Linux is
completely hung or hasn't booted (early boot, the R5F/TF-A/U-Boot stages),
read memory and registers, and debug the R5F and C7x cores as well. It's
the tool of last resort for "no output at all".

### 3.11 perf for kernel profiling

`perf` samples where CPU time goes, kernel included:

```sh
sudo perf top -g                           # live, kernel + user symbols
sudo perf record -a -g -- sleep 10         # whole system, call graphs
sudo perf report --sort sym                # which kernel functions burned CPU
sudo perf stat -e context-switches,cpu-migrations,cache-misses -a sleep 5
sudo perf trace -s ./my_app                # syscall summary, like strace but cheap
sudo perf sched record -- sleep 5; sudo perf sched latency   # scheduler latencies
```

On the board install the `linux-perf` package matching the kernel. The
A72 PMU gives hardware counters (`perf list`); without them perf falls back
to timer sampling. Flame graphs: `perf script | stackcollapse-perf.pl |
flamegraph.pl > out.svg`.

### 3.12 Which tool for which symptom?

| Symptom | First tool | Then |
|---|---|---|
| oops / BUG / WARN in the log | read it (3.2), `faddr2line` (3.3) | KASAN if it smells like corruption |
| crash, board reboots, log lost | pstore/ramoops, serial console capture | kdump, `ftrace_dump_on_oops` |
| hang, no output | SysRq `l`/`t`/`w`, lockup detectors, lockdep | kgdb/kdb, JTAG |
| "is my code even called?" | `pr_debug` + dyndbg, ftrace function tracer | kprobe / bpftrace |
| wrong order of events, races | tracepoints / `trace_printk` with timestamps | KCSAN, lockdep |
| latency spikes on PREEMPT_RT | `cyclictest` + ftrace `wakeup_rt`/`irqsoff`, `trace-cmd` | `osnoise`/`timerlat` tracers |
| CPU usage too high | `perf top` / `perf record` | ftrace function_graph on the hot path |
| memory leak | kmemleak, `/proc/slabinfo`, `/proc/meminfo` | `kmem:*` tracepoints, bpftrace |
| intermittent corruption | KASAN / KFENCE / `slub_debug` | `DEBUG_PAGEALLOC`, `DEBUG_OBJECTS` |

---

## 4. Complexity table

| Tool | Cost when off | Cost when on | Why |
|---|---|---|---|
| `printk` | n/a (always on) | µs to format; ms per line on a 115200 serial console | formatting + console drivers |
| `pr_debug` (dyndbg) | one NOP (static key) | like `printk` | jump label patched at enable |
| tracepoint | one NOP | ~100 ns per event | binary record into per-CPU ring buffer, no formatting |
| ftrace function tracer | NOPs at function entry | ~100–200 ns per traced call | NOP patched to a call |
| function_graph | same | 2× function (entry + exit) | return-address hijack |
| kprobe | 0 (not installed) | ~0.1–1 µs per hit (breakpoint + single-step); less if ftrace-based | trap + emulate |
| kretprobe | 0 | kprobe + return trampoline, instance pool | return-address hijack |
| eBPF program | 0 | tens of ns + map ops | JIT-compiled, verified |
| KASAN | rebuild | 2–3× CPU, +12.5% RAM | shadow memory check on every access |
| lockdep | rebuild | large, per lock acquire | dependency graph updates |
| kgdb | 0 | the whole system stops | |
| perf sampling | 0 | ~1–5% at default frequency | PMU interrupt + unwind |

---

## 5. C vs C++, and kernel vs user space

| User space | Kernel |
|---|---|
| `printf` / `fprintf(stderr)` / `syslog` | `printk` / `pr_*` / `dev_*` with log levels |
| `-DDEBUG` + `#ifdef DEBUG` logging | `pr_debug` + dynamic debug, switchable at run time |
| `assert()` / `abort()` | `WARN_ON()` (continue) / `BUG_ON()` (avoid) |
| segfault + core dump | oops (task killed, kernel continues tainted) / panic + kdump vmcore |
| `gdb ./prog core` / `addr2line` | `faddr2line`, `decode_stacktrace.sh`, `gdb x.ko`, `crash vmlinux vmcore` |
| `gdb` live, breakpoints | kgdb / kdb over serial, JTAG |
| `strace` | `perf trace`, syscall tracepoints, ftrace |
| `ltrace` / uprobes | kprobes / kretprobes / fprobes |
| `perf record ./prog` | `perf record -a -g` (kernel symbols) |
| ASan / UBSan / TSan / Valgrind | KASAN / UBSAN / KCSAN / KFENCE / kmemleak / lockdep |
| LTTng-UST / USDT probes | tracepoints / `TRACE_EVENT` (LTTng also does kernel) |

C++ isn't used in the kernel. A C++ user-space program on the board is
debugged with the user-space tools ([dsa/03](../../dsa/03-debugging/)), and
the kernel tools see it only from the outside (its syscalls, page faults,
scheduling).

---

## 6. Where it is used in the real world

- **Every kernel bug report** on the mailing lists is an oops or WARN plus
  `decode_stacktrace.sh` output. syzbot (the kernel fuzzer) files
  thousands, found with KASAN/KCSAN/lockdep kernels.
- **Embedded products** ship with `panic_on_oops=1`, a watchdog, and
  pstore/ramoops (Android, Chromebooks and automotive ECUs collect
  `/sys/fs/pstore` after every crash and upload it).
- **Tracepoints** power `perf`, `trace-cmd`, LTTng, bpftrace, Android
  systrace/Perfetto, and the RT tooling (`rtla timerlat`, `osnoise`) used
  to qualify PREEMPT_RT systems like the board's kernel.
- **ftrace** function graph is how driver authors see what a path really
  calls (an I2C transfer, a V4L2 `QBUF`).
- **kprobes/eBPF**: production observability at every big operator
  (Netflix, Meta, Google), BCC tools, Cilium, security monitors (Falco,
  Tetragon).
- **dynamic debug** is how you get `dev_dbg` output from a TI driver on the
  AI-64 without rebuilding: `echo 'module ti_csi2rx +p' > …/control`.
- **JTAG/CCS** is TI's standard path for bringing up TDA4VM boards and the
  R5F firmware.

---

## 7. Common mistakes and pitfalls

1. **Debugging the second oops.** The first one (`[#1]`) is the bug.
   Everything after may be fallout. Check the taint flags for an earlier
   `W`/`D`.
2. **A `printk` flood from an IRQ handler** stalls the board, and the
   serial console hides the timing bug you were chasing. Use
   `_ratelimited`, `trace_printk` or tracepoints.
3. **Expecting `pr_debug` to print** without enabling dynamic debug (or
   `-DDEBUG`).
4. **Using `%px` in a committed patch**, or being confused by hashed `%p`
   values. Use `%pS` for code and `%pK` for root-only files.
5. **`BUG_ON()` for recoverable conditions.** It turns a driver bug into a
   dead board. `WARN_ON_ONCE()` + return an error.
6. **`WARN_ON()` that user space can trigger at will**: log spam, and with
   `panic_on_warn` a denial of service. Validate input with normal errors.
7. **Decoding with the wrong `.ko`/`vmlinux`.** Offsets only match the
   exact binary that crashed. Keep the build artifacts of every image you
   deploy (`faddr2line` refuses a size mismatch, which is a good sign).
8. **No debug info on the target build**, so file:line decoding is
   impossible. Add `-g` to your modules (`ccflags-y += -g`) or enable
   `CONFIG_DEBUG_INFO_*`.
9. **Trusting `gdb list *(f+off)` on a `.ko` blindly**: section-relative
   addresses can resolve into the wrong function (see 3.3).
10. **Out-of-tree `TRACE_EVENT` without `-I$(src)`**: "No such file"
    from `define_trace.h`. Also: `CREATE_TRACE_POINTS` in more than one
    `.c` file gives duplicate symbols.
11. **Sleeping or taking a mutex in a kprobe handler.** It's atomic
    context. Also, probing a function used by your own handler (`printk`,
    `ktime_get`) recurses (kprobes detect it and count it as missed).
12. **Leaving a tracer running** (`function` on everything) and wondering
    why the board is slow. `echo nop > current_tracer`.
13. **pstore at an address U-Boot or Linux also uses**: the log is
    overwritten. Reserve the region in the device tree.
14. **kgdb with the serial terminal still open**: gdb and picocom fight
    over the port.
15. **Running bug modules on your development PC.** Never: board or VM
    only.

---

## 8. Files in this lesson

| File | What it shows | Build / run |
|---|---|---|
| [kernel/Makefile](kernel/Makefile) | Kbuild makefile; `KDIR=`, `ARCH=`/`CROSS_COMPILE=`, `BUGS=1`, `W=1`, `clean`; `CFLAGS_trace_demo.o := -I$(src)` for the trace header; `kprobe_demo` only `ifdef CONFIG_KPROBES`; `-g` for `oops_demo_bug.o` | `make drivers` |
| [kernel/dbg_demo.c](kernel/dbg_demo.c) | log levels, `pr_debug` sites for dynamic debug, a ratelimit state checked to pass exactly its burst, `print_hex_dump`(`_debug`), `%*ph`, `%p`/`%pK`/`%pS`/`%ps`/`%pe`/`%pOF`, `WARN_ONCE` (param `warn=1` or debugfs `warn`), `noinline` ftrace targets `dbg_demo_target`→`dbg_demo_helper` called by a delayed work (`period_ms=`) or debugfs `poke` | `insmod dbg_demo.ko [period_ms=500] [warn=1]` |
| [kernel/trace_demo_trace.h](kernel/trace_demo_trace.h) | `TRACE_EVENT(esh_tick)` and `TRACE_EVENT_CONDITION(esh_late)`, the multi-read guard and the `define_trace.h` include path | (header) |
| [kernel/trace_demo.c](kernel/trace_demo.c) | `CREATE_TRACE_POINTS`, a kthread emitting the events every `period_ms` with its wake-up lateness; `period_ms` writable at run time via `module_param_cb` + `param_set_uint_minmax`; `trace_*_enabled()` | `insmod trace_demo.ko [period_ms=100]` |
| [kernel/kprobe_demo.c](kernel/kprobe_demo.c) | kprobe counting calls + kretprobe timing calls and counting negative returns for `symbol=` (default `vfs_read`); clear error on registration failure; stats in debugfs and at unload | `insmod kprobe_demo.ko [symbol=vfs_read]` |
| [kernel/oops_demo_bug.c](kernel/oops_demo_bug.c) | **intentionally broken** (only with `BUGS=1`): safe to load; writing 1/2/3 to its `trigger` parameter causes a NULL dereference (fault address 0x10), `BUG()`, or a `WARN()` three calls deep | `make -C …/kernel BUGS=1`, `insmod oops_demo_bug.ko` |

There are no user-space programs: every module is driven through tracefs,
debugfs, module parameters and `dmesg`.

**Build** (from the repo root):

```sh
make drivers                                                              # host
make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel W=1      # extra warnings
make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel BUGS=1   # + oops_demo_bug.ko
BB=<path to beaglebone-ai64>/sources/arm64-multiplatform                  # board, see ../README.md#building
make -C linux-device-drivers/09-kernel-debugging-and-tracing/kernel BUGS=1 \
     KDIR=$BB/KERNEL ARCH=arm64 \
     CROSS_COMPILE=$BB/dl/gcc-15.2.0-nolibc/aarch64-linux/bin/aarch64-linux-
make drivers-clean
```

All modules are **build-tested only**: on the host (Ubuntu 22.04, kernel
6.8, zero warnings with and without `W=1` and `BUGS=1`; checkpatch
`--strict` clean apart from the file-name-in-header note, the
`TRACE_EVENT` layout checks that every kernel trace header gets, and
the intentional `BUG_ON` in the bug module), and cross-built with `W=1`
against the board's `6.18.38-arm64-k3` PREEMPT_RT tree with its GCC 15.
The `faddr2line` output in 3.3 was produced from that cross build.
Loading them is **your** step, on the board or in a VM; see
[../README.md](../README.md#safety).

**Run** (board or VM):

```sh
sudo insmod dbg_demo.ko period_ms=500
sudo dmesg | grep dbg_demo
echo 'module dbg_demo +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
sleep 2; sudo dmesg | tail -3            # pr_debug lines from the ticking target
echo 1 | sudo tee /sys/kernel/debug/dbg_demo/warn          # WARNING: ... warn_write
sudo rmmod dbg_demo
```

Expected output (example; hashes, offsets and timings differ):

```
dbg_demo: level 3 KERN_ERR     (pr_err)
dbg_demo: level 4 KERN_WARNING (pr_warn)
dbg_demo: level 5 KERN_NOTICE  (pr_notice)
dbg_demo: level 6 KERN_INFO    (pr_info)
dbg_demo: ratelimited message 0
dbg_demo: ratelimited message 1
dbg_demo: ratelimited message 2
dbg_demo: PASS ratelimit  100 attempts, 3 printed, 97 suppressed (burst 3 per second)
dbg_demo: 00000000: 88 b5 00 14 41 42 43 44 45 46 47 48 49 4a 4b 4c  ....ABCDEFGHIJKL
dbg_demo: 00000010: 4d 4e 4f 50 51 52 53 54                          MNOPQRST
dbg_demo: header bytes: 88 b5 00 14
dbg_demo: %p   00000000a1b2c3d4
dbg_demo: %pK  0000000000000000            (zeros or the raw address, per kptr_restrict)
dbg_demo: %pS  dbg_demo_target+0x0/0x50 [dbg_demo]
dbg_demo: %ps  dbg_demo_target
dbg_demo: %pS of the caller of this function: dbg_demo_init+0x30/0xff8 [dbg_demo]
dbg_demo: %pe  -ENOMEM
dbg_demo: %pOF / (the device-tree root)
dbg_demo: PASS target     dbg_demo_target(1) hash ok
dbg_demo: ready: ftrace target dbg_demo_target, debugfs /sys/kernel/debug/dbg_demo/{poke,warn,calls}, ticking
dbg_demo: done: all checks passed
   ... after enabling dyndbg:
dbg_demo: target(12) -> 6a99b44c, call 13
```

`trace_demo` and `kprobe_demo`: see the outputs in 3.6 and 3.7.
`oops_demo_bug`:

```sh
sudo insmod oops_demo_bug.ko             # "armed: echo 1|2|3 > ..."
echo 3 | sudo tee /sys/module/oops_demo_bug/parameters/trigger   # WARNING, kernel continues
echo 1 | sudo tee /sys/module/oops_demo_bug/parameters/trigger   # Oops (see 3.2); tee is killed
cat /proc/sys/kernel/tainted             # non-zero: D (oops) + W + O
# now decode it (3.3), then reboot: the module can't be removed cleanly any more
```

---

## 9. Exercises

**Easy**
1. Set `console_loglevel` to 4 and load `dbg_demo`. Which lines reach the
   serial console and which only `dmesg`? Now boot with `loglevel=8`.
2. Enable dynamic debug for `dbg_demo` with the `+pflmt` flags and explain
   each extra field in the output. Then enable `dev_dbg` output of the
   board's I2C driver and run `i2cdetect -y 2`.
3. Trace `dbg_demo_target` with the function tracer while
   `period_ms=100`. Who calls it (`<-` column), and in which context
   (the `.....` flags column: irqs-off, need-resched, hardirq/softirq,
   preempt depth)?

**Medium**
4. Load `trace_demo period_ms=1`, enable both events, and run
   `stress-ng --cpu 2 --timeout 20` on the board. Build a histogram of
   `late_us` with `bpftrace` (3.7). Compare a non-RT VM.
5. Use `kprobe_demo symbol=i2c_transfer` on the board while reading a
   sensor. Then do the same with a tracefs `kprobe_events` probe printing
   the `num` argument, and with a bpftrace one-liner.
6. Trigger `oops_demo_bug` cases 1 and 2 (one per boot) and decode both
   with `faddr2line` and `decode_stacktrace.sh`. In case 2, which line does
   `kernel BUG at` name, and how does arm64 implement `BUG()` (look for
   `brk #0x800` in `objdump -d`)?
7. Set up ramoops on the board (3.4), crash with `echo c >
   /proc/sysrq-trigger` and read `/sys/fs/pstore/dmesg-ramoops-0` after a
   warm reboot.

**Hard**
8. Add a `DECLARE_EVENT_CLASS` with two `DEFINE_EVENT`s to
   `trace_demo_trace.h` (`esh_start`, `esh_stop`) and a `__string()` field
   that builds on both 6.8 and 6.18 (hint: `LINUX_VERSION_CODE` around
   `__assign_str`).
9. Build a QEMU arm64 kernel with kgdb, boot with `kgdboc=ttyAMA0
   kgdbwait`, attach `gdb-multiarch`, load `dbg_demo`, use `lx-symbols`,
   set a breakpoint on `dbg_demo_target` and print the argument.
10. Write a kprobe on `dbg_demo_target` itself from `kprobe_demo` (it's a
    module symbol: `symbol=dbg_demo_target`; what happens if you unload
    `dbg_demo` first? and while probed?) and read the first argument from
    `regs->regs[0]` on arm64 / `regs->di` on x86.

---

## 10. Remember forever

```
printk levels 0..7; console gets level < console_loglevel (/proc/sys/kernel/printk); dmesg gets all
pr_debug/dev_dbg = dynamic debug: echo 'module X +p' > /sys/kernel/debug/dynamic_debug/control
ratelimit floods (_ratelimited, DEFINE_RATELIMIT_STATE); %p hashed, %pS symbol, %pe errno, %pOF node
Oops: first one only; fault addr (0x10 = NULL+offset), pc/lr func+off/size [mod], taint, call trace
decode: faddr2line x.ko func+off/size (needs -g), decode_stacktrace.sh, gdb list *(f+off) (check section!)
WARN_ON_ONCE + recover, not BUG_ON; panic_on_oops + watchdog + pstore/ramoops on products
ftrace: /sys/kernel/tracing, set_ftrace_filter, function/function_graph, trace_pipe, nop to stop
TRACE_EVENT: TRACE_SYSTEM, multi-read guard, CREATE_TRACE_POINTS in ONE .c, -I$(src) out of tree
kprobe/kretprobe: atomic handlers, -ENOENT/-EINVAL on register; bpftrace for one-liners
debug kernel: KASAN KFENCE UBSAN KCSAN lockdep DEBUG_ATOMIC_SLEEP kmemleak DEBUG_OBJECTS
kgdboc=<console tty>,115200 + sysrq g; JTAG when nothing else talks; perf top/record for CPU
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Which printk messages reach the serial console, and how do you change
   that at run time and at boot?
2. How does dynamic debug make a disabled `pr_debug` nearly free? Write the
   control-file command to enable all debug output of one source file.
3. Why does `%p` print hashed values? When would you use `%pS`, `%pK`,
   `%pe` and `%px`?
4. What is the difference between WARN, BUG, Oops and panic? What
   happens to the current task and to locks it held after an oops?
5. An oops says `address 0000000000000018`. What is the most likely bug? And
   for `dead000000000122` or `6b6b6b6b6b6b6b6b`?
6. In `pc : foo+0x24/0x80 [bar]`, what are the four parts? What does `lr`
   tell you on arm64?
7. Name four taint flags. Why do `W` and `D` matter when reading an oops?
8. Give three ways to turn `func+off/size [module]` into a source line.
   What does each need, and what is the `.ko` section pitfall?
9. Why do embedded products set `panic_on_oops=1`, and how do you still
   get the log (two methods)?
10. How does ftrace trace a function with zero cost while disabled?
    Difference between `function` and `function_graph`?
11. Explain the four mechanics of an out-of-tree `TRACE_EVENT` header.
    What error do you get without `-I$(src)`?
12. Tracepoint vs `trace_printk` vs `printk`: cost, where the output goes,
    when to use each.
13. What can't you do in a kprobe handler? Why might `register_kprobe()`
    fail with `-ENOENT` for a function that exists in the source?
14. Which debug option catches each: a use-after-free, sleeping under a
    spinlock, an ABBA deadlock that hasn't happened yet, a data race, a
    leak, freeing an armed timer?
15. How do you attach gdb to the board's kernel over the serial console,
    and why must the terminal program be closed?
16. When is JTAG the only option?
