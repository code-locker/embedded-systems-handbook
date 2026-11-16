# Conventions — how every lesson in this repo is written

Following one fixed shape makes the material easy to revisit: once you have
read one lesson, you know where everything is in all the others.

## Folder shape

```
dsa/NN-topic/
├── README.md          the lesson (theory + pictures + real-world use + recall)
├── c/                 C implementations     (C11)
│   ├── 01_<name>.c
│   └── 02_<name>.c
└── cpp/               C++ implementations   (C++17)
    ├── 01_<name>.cpp
    └── ...

linux-internals/NN-topic/   same shape as dsa/: README.md + c/ + cpp/

linux-device-drivers/NN-topic/
├── README.md
├── kernel/            out-of-tree module(s) + Kbuild Makefile (KDIR=..., `make drivers`)
└── user/              user-space programs that talk to the module (need it loaded)

gstreamer/NN-topic/
├── README.md
├── *.c / *.cpp        programs
└── *.sh               gst-launch-1.0 pipelines to try
```

## Every program

1. **Standalone.** One file is one program with its own `main()`. No shared
   headers between lessons, so you can copy a single file anywhere and build it.
   Built by the top-level `Makefile` into `build/<same path>`.
2. **Self-checking.** It prints what it does *and* checks its results with
   `assert()` (or a small `CHECK` macro), so `make test` proves it still works.
   It runs without user input and exits 0 on success.
3. **Clean under sanitizers.** Builds with `-Wall -Wextra -Wpedantic -Wshadow`
   and runs clean under AddressSanitizer + UndefinedBehaviorSanitizer, with no
   leaks. Exception: files named `*_bug.c` / `*_bug.cpp` are *intentionally*
   broken for debugging lessons; their header says which tool finds the bug.
   `linux-internals/` programs must *also* be clean under ThreadSanitizer
   (`make run P=... SAN=tsan`, `make test-tsan`), finish in a few seconds
   even under TSan, never hang (timeouts / `alarm()` watchdogs), and use
   per-run unique IPC names (include `getpid()`) that they clean up.
   Kernel modules build with zero warnings (also with `W=1`) and test
   themselves at load time, printing PASS/FAIL to `dmesg`.
4. **Header comment** at the top of the file:
   ```c
   /*
    * 01_singly_linked_list.c — singly linked list: insert, delete, reverse, search
    *
    * Concept : what this program teaches, in 2–4 lines
    * Build   : make run P=dsa/06-linked-lists/c/01_singly_linked_list.c
    * Read    : ../README.md  (section "Singly linked list")
    */
   ```
5. **Comments explain *why*, not just *what*.** Every non-trivial step says
   why it is done this way, what would break otherwise, and the complexity of
   each operation (`/* O(n): must walk to the tail */`). Memory ownership is
   always stated: who allocates, who frees.
6. **ASCII pictures in comments** at the tricky moments (pointer rewiring,
   rotations, stack frames), because that is the step you forget.
7. **C++ versions are idiomatic modern C++**: RAII, `std::unique_ptr`,
   templates, `const`-correctness, the STL equivalent shown next to the
   hand-written one, so you learn both how it works and how you would really
   use it.

## Every README

Sections in this order:

1. **Why this exists**: the problem it solves, in plain words.
2. **Mental model**: one analogy plus an ASCII diagram (memory layout,
   pointers, tree shape...).
3. **How it works**: step by step, operation by operation, with diagrams.
4. **Complexity table**: time and space for every operation, and *why*.
5. **C vs C++**: how each language expresses it; the STL/standard equivalent.
6. **Where it is used in the real world**: the Linux kernel, databases,
   networking, GStreamer, compilers, games, embedded.
7. **Common mistakes and pitfalls**.
8. **Files in this lesson**: a table that links every program, with a one-line
   description and the exact `make run P=...` command.
9. **Exercises**: graded from easy to hard, with hints.
10. **Remember forever**: a 5–10 line recall card, the minimum you must be
    able to rebuild from memory. Review these cards using the schedule in
    [LEARNING_METHOD.md](LEARNING_METHOD.md).
11. **Self-test questions**: questions you answer out loud without looking.
