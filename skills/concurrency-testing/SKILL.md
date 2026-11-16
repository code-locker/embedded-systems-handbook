---
name: concurrency-testing
description: Test multi-threaded and multi-process C/C++ programs in this handbook for data races, deadlocks and hangs — ThreadSanitizer via `make SAN=tsan` / `make test-tsan` (and why it needs `setarch -R`), helgrind/DRD with SAN=0, writing deterministic `*_bug` race/deadlock/lost-wakeup demos, and fork/IPC rules that keep tests hang-free. Use when writing or debugging anything in linux-internals/, when TSan aborts with "unexpected memory mapping", or when a threaded test is flaky or hangs.
---

# Concurrency testing

## Commands (tested on Ubuntu 22.04, gcc 11, kernel 6.8)

```bash
make run P=linux-internals/<lesson>/c/<prog>.c SAN=tsan BUILD=build/tsan   # one program
make test-tsan                                                              # all of linux-internals/ under TSan
make SAN=0 build/linux-internals/<lesson>/c/<prog>                          # no sanitizer, for valgrind
valgrind --tool=helgrind build/linux-internals/<lesson>/c/<prog>            # second opinion on lock misuse
valgrind --tool=drd      build/linux-internals/<lesson>/c/<prog>
```

- `SAN=tsan` replaces ASan/UBSan (they cannot be combined). Use a separate
  `BUILD=` dir, or the flag stamp forces a full rebuild every time you switch modes.
- `make test-tsan` builds into `build/tsan/` and runs with
  `TSAN_OPTIONS=halt_on_error=1 second_deadlock_stack=1`.

## Why `setarch -R`

On recent Ubuntu kernels `vm.mmap_rnd_bits` is 32, and TSan (and sometimes ASan) dies with
`FATAL: ThreadSanitizer: unexpected memory mapping`. Fix it with either:
- `setarch $(uname -m) -R <prog>`, which turns ASLR off for that process only. The Makefile does
  this for `make run SAN=tsan` and `make test-tsan`. It needs no root.
- `sudo sysctl -w vm.mmap_rnd_bits=28`, which is what CI does (`.github/workflows/ci.yml`).

## Rules for programs that stay green

- **Bounded runtime.** TSan is 5–15× slower. Keep loops to ~10⁴–10⁵ operations, under 10 s
  total. `run-all.sh` kills programs at 60 s.
- **Never hang.** Use timed waits (`pthread_cond_timedwait`, `sem_timedwait`, `mq_timedreceive`)
  wherever a bug could block forever. Forked children set `alarm(5)` and exit with `_exit()`.
- **Deterministic checks.** Assert invariants (sums, counts, ordering, max-concurrency ≤ N).
  Print timing-dependent numbers (contention counts, benchmark times) but never assert them.
- **fork + TSan:** TSan does not support threads created after a fork in a multi-threaded process.
  Fork while the process is still single-threaded, then create threads. TSan cannot see
  races *between* processes. For shared-memory lessons the program's own counters are the test.
- **IPC names** (`/dev/shm`, `/dev/mqueue`, FIFOs, socket paths) include `getpid()` and are
  unlinked on every exit path. Leftovers make the next run fail.
- **Hand-made locks** must use C11/C++ atomics with acquire (lock) and release (unlock) ordering,
  so TSan can see the happens-before. A raw `futex` syscall alone is invisible to TSan.

## Writing a `*_bug` program

| Bug | How to make it observable | Tool named in header |
|---|---|---|
| data race on a counter | two threads `counter++`, print the lost updates | `SAN=tsan`: "data race" |
| ABBA deadlock | two threads lock A→B and B→A | `SAN=tsan`: "lock-order-inversion" (it also hangs: run with `timeout 5`) |
| lost wakeup | signal before state change / check without lock | hangs under `timeout 5`; helgrind |
| forgotten pipe write end | reader waits for EOF forever | `timeout 5` exit 124 |
| non-async-signal-safe handler | printf/malloc in a handler | `SAN=tsan`: "signal-unsafe call inside of a signal" |

`make test` skips `*_bug` binaries. Still build them, and check by hand that the named tool fires.

## Symptom → cause

| Symptom | Cause / fix |
|---|---|
| `FATAL: ThreadSanitizer: unexpected memory mapping` | ASLR entropy. Run via the Makefile (`setarch -R`) or set `mmap_rnd_bits=28` |
| TSan "data race" in code that uses a futex/hand-made lock | lock word not accessed with acquire/release atomics |
| TSan warning "starting new threads after multi-threaded fork" | fork earlier, before any `pthread_create` |
| test passes alone, fails/hangs in `make test` | fixed IPC name collides, or an earlier run left objects in `/dev/shm` or `/dev/mqueue` |
| helgrind reports on `std::atomic`/C11 atomics | helgrind doesn't model atomics well. Trust TSan for those |
| memory-order litmus test never shows the weak-ordering outcome | GCC turns a `memory_order` that is passed as a function *parameter* into seq_cst. Make orders compile-time constants (macros/templates), and put the variables on separate cache lines |
| TSan "double lock"/"unlock of unlocked mutex" with `pthread_cancel` | TSan misreports cleanup-handler unlocks when the thread is cancelled inside an intercepted blocking call (e.g. `nanosleep`). Cancel only at `pthread_testcancel()` in demos |
| TSan "double lock" / "unlock of an unlocked mutex" / race in C++ `wait_for`, `wait_until(steady_clock)`, `timed_mutex::try_lock_until` | GCC 11's TSan (Ubuntu 22.04) does not intercept `pthread_cond_clockwait` / `pthread_mutex_clocklock`, which libstdc++ uses for steady_clock. Use a `WaitClock` alias = `system_clock` under `__SANITIZE_THREAD__` (else `steady_clock`), and in C call `*clockwait` only on timeout paths |
| TSan "use of an invalid mutex" on a `PTHREAD_PRIO_PROTECT` mutex | locking it as a normal user returns EINVAL. Skip that lock attempt under `__SANITIZE_THREAD__` |
| TSan reports races around `mq_notify`/timer `SIGEV_THREAD` callbacks | glibc creates that thread internally, where TSan can't see it. Use `SIGEV_SIGNAL` + `sigtimedwait` in tested demos and explain `SIGEV_THREAD` in prose |
| signal demo hangs only under TSan | TSan defers signal handlers to its next safe point, so a handler expected to run *during* a blocking `read()` may never run. Drive the blocking call from another process, and assert `>= 1` not exact tick counts |
| queued RT signals arrive merged under TSan | TSan delivers them on unblock in a batch. Drain with a `sigsuspend` loop guarded by `sigpending`, one signal per call |
| ASan "CHECK failed … kCurrentStackFrameMagic" in a cancellation demo | glibc cancels C threads by unwinding in a way ASan doesn't track. Mark the cancelled functions `__attribute__((no_sanitize_address))` under `#ifdef __SANITIZE_ADDRESS__`; TSan still covers them |
| `mlock`/`mlockall` "succeed" but VmLck stays 0 | ASan/TSan intercept them as no-ops. Use `syscall(SYS_mlock, …)` to really lock; skip `mlockall` under `__SANITIZE_ADDRESS__`/`__SANITIZE_THREAD__` |
| LeakSanitizer fatal error at exit, stdout lost | the fd limit is used up, so LSan can't open `/proc`. Restore `RLIMIT_NOFILE` before printing or exiting. LSan also refuses ptrace, so use `SAN=0` for strace |
| page-fault / RSS / COW counts ~3× too high under TSan | TSan writes shadow memory on reads too. Assert `>=` or relative values; also `madvise(MADV_NOHUGEPAGE)` the region so THP doesn't map 512 pages per fault |
| TSan binary re-exec'd with ASLR on aborts | keep `ADDR_NO_RANDOMIZE` across the self-exec (it survives exec) |
| "which mapping holds libc" demo shows libasan/libtsan | intercepted symbols (printf, malloc) resolve to the sanitizer runtime. Use a non-intercepted one like `getpid` |
| thread with a small explicit stack segfaults under TSan before running | static TLS + TSan per-thread state eat the stack. Use ≥ 2 MiB stacks in tested programs |
| `mlockall` ENOMEM under ASan/TSan | terabytes of shadow mappings. Skip `mlockall` under sanitizers |
| latency floor of ~50 µs in a timing demo | default timer slack for SCHED_OTHER. `prctl(PR_SET_TIMERSLACK, 1)` |
| scheduling demo can't restore SCHED_OTHER / lower nice | unprivileged SCHED_IDLE and nice increases are one-way (RLIMIT_NICE=0). Do them in a throwaway thread |
| copy benchmarks show user-space copies much slower than sendfile/splice | sanitizers instrument user copies. Benchmark with `SAN=0 OPT=1` |
| fork/orphan demo flaky only under TSan | the parent races the child's read of `getppid()`. Add a pipe handshake so ordering is explicit, never `sleep` |
