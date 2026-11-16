# Roadmap and progress tracker

Tick a box when you finish a lesson (README read, programs run and modified,
core rewritten from memory). Fill in the review dates using the schedule in
[LEARNING_METHOD.md](LEARNING_METHOD.md): R1 = +1 day, R2 = +3 days,
R3 = +1 week, R4 = +3 weeks, R5 = +2 months.

The tracks can run in parallel. A good rhythm is 4 days of DSA and
2 days of GStreamer or Linux internals per week. GStreamer and Linux internals
need pointers, memory and debugging (DSA phases 1–2) first. GStreamer 08
(queues and threads) reads much better after Linux internals part 1.

## Track A — DSA

### Phase 1: how the machine works (weeks 1–3)
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [00 Foundations](../dsa/00-foundations/README.md) | | | | | | |
| [ ] | [01 Pointers](../dsa/01-pointers/README.md) | | | | | | |
| [ ] | [02 Memory management](../dsa/02-memory-management/README.md) | | | | | | |
| [ ] | [03 Debugging](../dsa/03-debugging/README.md) | | | | | | |
| [ ] | [04 Complexity analysis](../dsa/04-complexity-analysis/README.md) | | | | | | |

### Phase 2: linear structures (weeks 4–6)
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [05 Arrays and strings](../dsa/05-arrays-and-strings/README.md) | | | | | | |
| [ ] | [06 Linked lists](../dsa/06-linked-lists/README.md) | | | | | | |
| [ ] | [07 Stacks and queues](../dsa/07-stacks-and-queues/README.md) | | | | | | |
| [ ] | [08 Hashing](../dsa/08-hashing/README.md) | | | | | | |

### Phase 3: trees (weeks 7–9)
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [09 Trees](../dsa/09-trees/README.md) | | | | | | |
| [ ] | [10 Heaps and priority queues](../dsa/10-heaps-priority-queues/README.md) | | | | | | |
| [ ] | [11 Tries](../dsa/11-tries/README.md) | | | | | | |
| [ ] | [12 Advanced trees](../dsa/12-advanced-trees/README.md) | | | | | | |

### Phase 4: algorithms (weeks 10–15)
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [14 Sorting and searching](../dsa/14-sorting-and-searching/README.md) | | | | | | |
| [ ] | [15 Recursion and backtracking](../dsa/15-recursion-and-backtracking/README.md) | | | | | | |
| [ ] | [13 Graphs](../dsa/13-graphs/README.md) | | | | | | |
| [ ] | [16 Greedy](../dsa/16-greedy/README.md) | | | | | | |
| [ ] | [17 Dynamic programming](../dsa/17-dynamic-programming/README.md) | | | | | | |
| [ ] | [18 Bit manipulation](../dsa/18-bit-manipulation/README.md) | | | | | | |

Graphs come after recursion on purpose: DFS *is* recursion.

### Phase 5: putting it together (week 16+)
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [19 System-design structures](../dsa/19-system-design-structures/README.md) | | | | | | |

## Track B — GStreamer (start after DSA phase 2)

| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [00 Concepts](../gstreamer/00-concepts/README.md) | | | | | | |
| [ ] | [01 gst-launch](../gstreamer/01-gst-launch/README.md) | | | | | | |
| [ ] | [02 First pipeline in C](../gstreamer/02-first-pipeline/README.md) | | | | | | |
| [ ] | [03 Bus and messages](../gstreamer/03-bus-and-messages/README.md) | | | | | | |
| [ ] | [04 Dynamic pads](../gstreamer/04-dynamic-pads/README.md) | | | | | | |
| [ ] | [05 Caps and negotiation](../gstreamer/05-caps-and-negotiation/README.md) | | | | | | |
| [ ] | [06 appsrc / appsink](../gstreamer/06-appsrc-appsink/README.md) | | | | | | |
| [ ] | [07 Pad probes](../gstreamer/07-pad-probes/README.md) | | | | | | |
| [ ] | [08 Queues, threads, tee](../gstreamer/08-queues-threads-tee/README.md) | | | | | | |
| [ ] | [09 C++ RAII wrapper](../gstreamer/09-cpp-raii-wrapper/README.md) | | | | | | |
| [ ] | [10 Custom element / plugin](../gstreamer/10-custom-element-plugin/README.md) | | | | | | |
| [ ] | [11 Network streaming](../gstreamer/11-network-streaming/README.md) | | | | | | |
| [ ] | [12 Debugging and tracing](../gstreamer/12-debugging-and-tracing/README.md) | | | | | | |
| [ ] | [13 Performance and zero-copy](../gstreamer/13-performance-zero-copy/README.md) | | | | | | |
| [ ] | [14 Embedded: TDA4VM / BeagleBone AI-64](../gstreamer/14-embedded-tda4vm/README.md) | | | | | | |

## Track C — Linux internals: concurrency and IPC (start after DSA phase 2)

Run every program twice: normally and with `SAN=tsan`. Then break the locking
on purpose and watch ThreadSanitizer catch it.

### Part 1: threads and synchronisation
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [01 Processes and threads](../linux-internals/01-processes-and-threads/README.md) | | | | | | |
| [ ] | [02 Race conditions and atomics](../linux-internals/02-race-conditions-and-atomics/README.md) | | | | | | |
| [ ] | [03 Mutexes and deadlock](../linux-internals/03-mutexes-and-deadlock/README.md) | | | | | | |
| [ ] | [04 Condition variables](../linux-internals/04-condition-variables/README.md) | | | | | | |
| [ ] | [05 Semaphores](../linux-internals/05-semaphores/README.md) | | | | | | |
| [ ] | [06 Spinlocks and futex](../linux-internals/06-spinlocks-and-futex/README.md) | | | | | | |
| [ ] | [07 Read-write locks, seqlocks and RCU](../linux-internals/07-rwlocks-seqlocks-and-rcu/README.md) | | | | | | |
| [ ] | [08 Barriers, once and thread pools](../linux-internals/08-barriers-once-and-thread-pools/README.md) | | | | | | |

### Part 2: inter-process communication
| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [09 Pipes and FIFOs (+ IPC overview)](../linux-internals/09-ipc-pipes-and-fifos/README.md) | | | | | | |
| [ ] | [10 Message queues](../linux-internals/10-ipc-message-queues/README.md) | | | | | | |
| [ ] | [11 Shared memory](../linux-internals/11-ipc-shared-memory/README.md) | | | | | | |
| [ ] | [12 Unix domain sockets](../linux-internals/12-ipc-unix-sockets/README.md) | | | | | | |
| [ ] | [13 Signals, eventfd and epoll](../linux-internals/13-signals-eventfd-and-epoll/README.md) | | | | | | |

## Track D — Linux device drivers (after Track C part 1)

| ✓ | Lesson | Done on | R1 | R2 | R3 | R4 | R5 |
|---|---|---|---|---|---|---|---|
| [ ] | [01 Kernel synchronisation](../linux-device-drivers/01-kernel-synchronization/README.md) | | | | | | |

Kernel modules are build-tested on the PC. Load them on the board or in a VM,
not on your desktop kernel.

## Milestone projects

Each one combines several lessons. Build it from scratch in a `projects/` folder.

1. **Memory-checked container library** (after phase 2): vector, list,
   hash map and ring buffer in C, with a test suite run under ASan.
2. **Route planner** (after graphs): load a small road map from a file and
   answer shortest-path queries with Dijkstra and A*.
3. **Frame grabber** (after GStreamer 06): capture from `v4l2src` or
   `videotestsrc`, pull frames with appsink, compute a per-frame histogram in
   C++, and print it.
4. **Multi-process sensor logger** (after Linux internals): a producer
   process writes samples into a shared-memory ring buffer, a logger process
   wakes on an eventfd passed over a Unix socket, and a control process
   changes settings through a message queue. It shuts down cleanly on SIGTERM
   and is clean under TSan within each process.
5. **Streaming camera on the BeagleBone AI-64** (after GStreamer 14): camera →
   hardware encoder → RTP/UDP → laptop, with latency measured using tracers.
