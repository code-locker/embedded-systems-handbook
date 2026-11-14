# DSA + GStreamer — learn once, remember forever

A hands-on course in **C and C++** that goes from first principles to advanced
topics in two tracks:

| Track | What you learn |
|---|---|
| [**DSA**](dsa/README.md) | How the machine really works (compilation, pointers, stack/heap memory, debugging), then every core data structure and algorithm, up to graphs, advanced trees, dynamic programming and system-design structures. |
| [**GStreamer**](gstreamer/README.md) | Multimedia pipelines from `gst-launch-1.0` one-liners to C/C++ applications, appsrc/appsink, pad probes, custom elements, network streaming, debugging, zero-copy, and embedded use on the BeagleBone AI-64 (TDA4VM). |

More tracks are on the way, each arriving as its own PR: **Linux internals**
(processes, virtual memory, threads, interrupts, zero-copy, DMA buffers),
**ARM architecture**, **Linux device drivers**, and **protocols**
(UART, I2C, SPI, CAN, USB, TCP/IP).

Every lesson is a folder with a **README that explains the concept** (with
analogies, ASCII diagrams, real-world uses, pitfalls and a recall card) and
**runnable programs** in C and C++. Each program checks its own results, so
`make test` proves all of them still work.

## Quick start

```bash
sudo apt install build-essential gdb valgrind \
     libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
     gstreamer1.0-tools gstreamer1.0-plugins-good gstreamer1.0-plugins-bad

make test                 # build + run every DSA program (ASan + UBSan on)
make test-gst             # build + run every GStreamer program (headless)

# one program at a time (this is how you study):
make run P=dsa/01-pointers/c/01_pointer_basics.c
```

More in [docs/SETUP.md](docs/SETUP.md).

## How to study (read this first)

1. **[docs/LEARNING_METHOD.md](docs/LEARNING_METHOD.md)** explains how to make it
   stick: active recall, rebuilding from a blank file, spaced review.
2. **[docs/ROADMAP.md](docs/ROADMAP.md)** is the order to follow, with checkboxes
   and the review schedule.
3. For each lesson: read the README → run the programs → change them and break
   them → close everything and rewrite the core from memory → answer the
   self-test questions out loud.

## Layout

```
.
├── docs/                    learning method, roadmap, setup, conventions
├── dsa/
│   ├── 00-foundations       compilation, types, UB, C vs C++
│   ├── 01-pointers          everything about pointers, with memory pictures
│   ├── 02-memory-management stack / heap / segments / allocators / RAII / smart ptrs
│   ├── 03-debugging         gdb, valgrind, sanitizers, core dumps (buggy programs to hunt)
│   ├── 04-complexity-analysis
│   ├── 05-arrays-and-strings
│   ├── 06-linked-lists      incl. the Linux kernel list_head
│   ├── 07-stacks-and-queues incl. ring buffers and a lock-free SPSC queue
│   ├── 08-hashing
│   ├── 09-trees             BST, AVL, red-black
│   ├── 10-heaps-priority-queues
│   ├── 11-tries
│   ├── 12-advanced-trees    segment tree, Fenwick, B-tree, skip list
│   ├── 13-graphs            types, BFS/DFS, shortest paths, MST, SCC, flow, where used
│   ├── 14-sorting-and-searching
│   ├── 15-recursion-and-backtracking
│   ├── 16-greedy
│   ├── 17-dynamic-programming
│   ├── 18-bit-manipulation  incl. embedded register manipulation
│   └── 19-system-design-structures  LRU, pools, timer wheels, rate limiters
├── gstreamer/
│   ├── 00-concepts … 14-embedded-tda4vm
├── scripts/run-all.sh
└── Makefile                 builds every .c/.cpp as its own program into build/
```

## Conventions

Every lesson has the same shape, so once you know one you can find your way
around all of them. See [docs/CONVENTIONS.md](docs/CONVENTIONS.md).
