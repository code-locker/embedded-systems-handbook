# Roadmap and progress tracker

Tick a box when you finish a lesson (README read, programs run and modified,
core rewritten from memory). Fill in the review dates using the schedule in
[LEARNING_METHOD.md](LEARNING_METHOD.md): R1 = +1 day, R2 = +3 days,
R3 = +1 week, R4 = +3 weeks, R5 = +2 months.

The two tracks can run in parallel. A good rhythm is 4 days of DSA and
2 days of GStreamer per week. GStreamer needs pointers, memory and debugging
(DSA phases 1–2) first.

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

## Milestone projects

Each one combines several lessons. Build it from scratch in a `projects/` folder.

1. **Memory-checked container library** (after phase 2): vector, list,
   hash map and ring buffer in C, with a test suite run under ASan.
2. **Route planner** (after graphs): load a small road map from a file and
   answer shortest-path queries with Dijkstra and A*.
3. **Frame grabber** (after GStreamer 06): capture from `v4l2src` or
   `videotestsrc`, pull frames with appsink, compute a per-frame histogram in
   C++, and print it.
4. **Streaming camera on the BeagleBone AI-64** (after GStreamer 14): camera →
   hardware encoder → RTP/UDP → laptop, with latency measured using tracers.
