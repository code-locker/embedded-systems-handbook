---
name: add-lesson
description: Write a new lesson (or add/change a program in one) in any track of the embedded-systems-handbook — dsa/, linux-internals/, linux-device-drivers/, gstreamer/ — following docs/CONVENTIONS.md, verify every program with the sanitizers, and link it from the track README and docs/ROADMAP.md. Use when asked to add a topic, check whether a topic is covered, extend a lesson, or fix a lesson program.
---

# Add a lesson

## 1. Is it already covered?

```bash
grep -rliE '<keyword>|<api_name>' --include=*.md --include=*.c --include=*.cpp dsa linux-internals linux-device-drivers gstreamer
```

If it is partly covered, extend that lesson or cross-link it instead of
duplicating it. For example, the lock-free SPSC ring lives in
`dsa/07-stacks-and-queues` §3.8 and the mutex+condvar ring in
`dsa/19-system-design-structures/c/03`, and the concurrency lessons link to both.

## 2. Pick the track and folder

| Topic | Goes in | Built/tested by |
|---|---|---|
| data structure / algorithm | `dsa/NN-topic/{c,cpp}/` | `make test` (ASan+UBSan) |
| user-space OS: threads, locks, IPC, memory, scheduling | `linux-internals/NN-topic/{c,cpp}/` | `make test` + `make test-tsan` |
| kernel code | `linux-device-drivers/NN-topic/{kernel,user}/` | `make drivers` (build only), see `kernel-module-lesson` |
| GStreamer | `gstreamer/NN-topic/` | `make test-gst` |
| aarch64 asm / ARM specifics | `arm-architecture/` | `make arm` (cross, run on board) |
| UART/I2C/SPI/CAN/TCP | `protocols/` | `make test` |

The Makefile picks up any `.c`/`.cpp` under those directories automatically.
You never add a program to the Makefile by hand.

## 3. Write it (docs/CONVENTIONS.md is the contract)

- Each program is standalone with its own `main()`, self-checks with a `CHECK` macro or `assert`, and exits 0.
  It starts with the header comment (`Concept / Build / Read`) and ends by printing `"<name>: all checks passed"`.
- Use `#define _GNU_SOURCE` before the includes for POSIX/Linux APIs (the build is `-std=c11` / `-std=c++17`).
- Comments explain *why*, include ASCII diagrams at the tricky steps, and state who owns each piece of memory.
- The README has the 11 sections in order, with a "Files in this lesson" table giving exact `make run P=...` commands.
- Name intentionally broken programs `*_bug.c` / `*_bug.cpp`. Their header says which tool catches the bug.
- Model lesson for tone and depth: `dsa/07-stacks-and-queues/`.

## 4. Verify every program

```bash
make run P=<path>                                  # zero warnings, exit 0, no ASan/UBSan/LSan report
make run P=<path> SAN=tsan BUILD=build/tsan        # linux-internals: also clean under TSan
make test && make test-tsan                        # whole tree before committing
```

For `*_bug` programs, confirm the named tool really reports the bug
(e.g. `make run P=..._bug.c SAN=tsan` shows `WARNING: ThreadSanitizer`).

## 5. Link it

- Add a row in the track README (`dsa/README.md`, `linux-internals/README.md`, …).
- Add a checkbox row in `docs/ROADMAP.md`.
- If the track itself is new, add it to the root `README.md` table and layout, and to `docs/CONVENTIONS.md`.

## Symptom → cause

| Symptom | Cause / fix |
|---|---|
| `implicit declaration of function 'pthread_...'/'mkstemp'/'sigaction'` | missing `#define _GNU_SOURCE` before the first include |
| program passes alone but fails in `make test` | depends on cwd, a fixed IPC name, or leftover `/dev/shm` objects. Use `getpid()` in names and clean up |
| need the full output of a failing program | `run-all.sh` shows its last 15 lines; for everything, run the binary directly: `./build/<path>` |
| timeout (60 s) in `run-all.sh` | too many iterations under sanitizers, or a real hang. Shrink the loops or add `alarm()` |
| passes locally, fails on CI | CI is Ubuntu 24.04 + GCC 13 (this PC: 22.04 + GCC 11). Reproduce with `docker run --rm -v "$PWD":/src:ro ubuntu:24.04 bash -c 'apt-get update -qq && apt-get install -y -qq build-essential make >/dev/null; cp -r /src /w && cd /w && rm -rf build && make -s -j16 test'`. Add `--security-opt seccomp=unconfined` for `test-tsan` (setarch needs `personality()`). `run-all.sh` prints each failure's last lines in the CI log |
| `&local` not inside `[stack]` only with GCC 13+ ASan | detect_stack_use_after_return is on by default and moves address-taken locals to a heap "fake stack". In programs that inspect stack addresses, define `__asan_default_options()` returning `"detect_stack_use_after_return=0"` under `__SANITIZE_ADDRESS__` |
| program fails only in a container | root there lacks `CAP_SYS_ADMIN`, and seccomp blocks `personality()`. Decide by trying the operation, not by `geteuid()==0`, and skip with a message |
