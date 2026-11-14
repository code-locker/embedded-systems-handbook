# 5. Ordering: topological sort

[Back to the chapter](../README.md) · Programs:
[c/06_topological_sort.c](../c/06_topological_sort.c),
[cpp/06_topological_sort.cpp](../cpp/06_topological_sort.cpp)

## The problem

You have tasks and "must happen before" rules. Find an order that breaks no
rule. As a graph: draw `u -> v` for "u must come before v". A **topological
order** lists every vertex after all of its predecessors. It exists **iff the
graph is a DAG** (a cycle means "A before B before ... before A").

```
   Build DAG (edge = "is needed by")             ids
     config.h --+--> main.o --+                  0 app      3 config.h
                |      ^      |                  1 main.o   4 main.c
     main.c ----|------+      +--> app           2 util.o   5 util.c
                v             |
     util.c --> util.o -------+

   valid orders include:
     config.h main.c util.c main.o util.o app      (Kahn, FIFO)
     util.c main.c config.h util.o main.o app      (DFS reverse postorder)
   Both are right: a DAG usually has MANY topological orders.
```

---

## Kahn's algorithm (BFS on in-degrees)

### Intuition

Something with no unmet prerequisites can be done **now**. Do it, cross it
off, and see what became ready.

```
   indeg[v] = number of arrows into v
   ready = all v with indeg 0
   while ready not empty:
       u = take one from ready; output u
       for each u -> v:  indeg[v]--, if it hit 0: v is ready
   if output has fewer than n vertices: the rest are stuck on a CYCLE
```

### Trace (exact output of `c/06_topological_sort.c`)

| pop | in-degree after (app main.o util.o config.h main.c util.c) | ready queue after |
|---|---|---|
| (start) | 2 2 2 0 0 0 | 3 4 5 |
| 3 config.h | 2 1 1 0 0 0 | 4 5 |
| 4 main.c | 2 **0** 1 0 0 0 | 5 1 |
| 5 util.c | 2 0 **0** 0 0 0 | 1 2 |
| 1 main.o | 1 0 0 0 0 0 | 2 |
| 2 util.o | **0** 0 0 0 0 0 | 0 |
| 0 app | 0 0 0 0 0 0 | (empty) |

Order: config.h -> main.c -> util.c -> main.o -> util.o -> app.

**Cycle detection for free:** add the nonsense edge app -> config.h. Now
config.h has in-degree 1 and is never ready; Kahn outputs only main.c and
util.c, then the queue is empty with 4 vertices left. That is the moment GNU
make prints `Circular dependency dropped`.

**Choice of ready-set container:**

* FIFO queue: O(n + m), "breadth-first" order.
* Min-heap (`std::priority_queue` with `std::greater`): the
  **lexicographically smallest** order, O(n log n + m). Useful when you need
  a reproducible, deterministic build/install order regardless of input
  order. (`cpp/06`: 3 4 1 5 2 0.)
* The ready set is also the **parallelism**: everything in it can run at the
  same time. `make -j8` keeps up to 8 ready targets running.

---

## DFS-based topological sort

### Intuition

In DFS, a vertex **finishes** only after everything it points to has
finished. So if u -> v, then f[u] > f[v]. List vertices by **decreasing
finish time** (= reverse postorder) and every arrow points forward.

```
   visit(u):
       mark u GRAY
       for each u -> v:
           if v is GRAY: cycle! (no order exists)
           if v is WHITE: visit(v)
       mark u BLACK; append u to post[]
   answer = reverse(post)
```

Trace: DFS from 0 (app has no out-edges) finishes 0; then 1 (its only
successor 0 is done), 2, 3, 4, 5. Postorder 0 1 2 3 4 5, reversed:
**5 4 3 2 1 0** = util.c main.c config.h util.o main.o app.

| | Kahn | DFS reverse-postorder |
|---|---|---|
| idea | repeatedly remove a source | reverse of finish order |
| detects cycles | output < n | gray vertex hit |
| gives parallel "waves" | yes (the ready set) | no |
| recursion depth | none | up to n (use explicit stack) |
| lexicographic order | yes with a min-heap | no |
| time | O(n + m) | O(n + m) |

---

## Where topological order runs your life

* **make / ninja / Bazel / CMake**: targets and prerequisites form a DAG; the
  build order is a topological order; `-j` runs the ready set in parallel.
  Yocto/BitBake does the same with recipe tasks (`do_fetch` -> `do_unpack` ->
  `do_configure` -> `do_compile` -> `do_install` ...).
* **Package managers** (apt/dpkg, pip, cargo, npm): install dependencies
  before dependents. `cpp/06` resolves an install order *by name*:

  ```
  libc6 -> libv4l-0 -> libglib2.0-0 -> libgstreamer1.0-0
        -> gstreamer1.0-plugins-base -> gstreamer1.0-plugins-good
  ```

* **Task schedulers / init systems**: systemd orders units by `After=` /
  `Before=` (and pulls them in by `Requires=`/`Wants=`); Airflow and CI
  pipelines are explicitly DAGs of jobs; the Linux kernel runs `initcall`
  levels in order and resolves device probe ordering with deferred probing
  (`-EPROBE_DEFER`) and fw_devlink supplier/consumer links.
* **Spreadsheets**: recompute cells in topological order of their formula
  references (and report a circular reference on a cycle).
* **Compilers**: instruction scheduling inside a basic block respects the
  data-dependency DAG; static initialisation order; module import order.
* **Deep-learning frameworks**: a model is a DAG of operators; execution
  order (and memory reuse) comes from a topological sort. TI's TIDL on the
  TDA4VM imports a network into exactly such a graph before running it on
  the C7x/MMA.

### GStreamer pipelines as a DAG

A GStreamer pipeline is a DAG of elements joined by pad links (src pad ->
sink pad). Data (buffers) flows in topological order:

```
  filesrc -> qtdemux -+-> h264parse -> avdec_h264 -> videoconvert -> autovideosink
                      +-> aacparse  -> avdec_aac  -> audioconvert -> autoaudiosink
```

A topological order (`c/06`, Kahn):

```
filesrc qtdemux h264parse aacparse avdec_h264 avdec_aac videoconvert audioconvert autovideosink autoaudiosink
```

**State changes go the other way.** When a bin goes NULL -> READY -> PAUSED ->
PLAYING, it changes its children's states **from sinks towards sources**
(GstBin iterates its elements "sorted", sinks first) — reverse topological
order. Reason: a downstream element must be ready to accept buffers before
the upstream element starts pushing them. When you build pipelines in code,
`gst_element_link_many()` adds edges to this DAG; a `tee` makes a fan-out
vertex, a muxer/compositor makes a fan-in vertex. Feedback loops are not
allowed in the data flow (that would be a cycle).

---

## Pitfalls

* Edge direction: decide once whether `u -> v` means "u before v" or "u
  depends on v". Reversing it silently gives you the reverse order.
* Forgetting the cycle check (Kahn: `count < n`; DFS: gray vertex). A
  "topological order" of a cyclic graph is garbage.
* Expecting a unique answer. Unless the DAG has a Hamiltonian path, many
  orders are valid — tests must check *validity*, not equality
  (`valid_order()` in the programs).
* DFS topo sort on huge graphs: recursion depth. Use Kahn or an explicit
  stack.

[Previous: connectivity](04-connectivity.md) · [Next: shortest paths](06-shortest-paths.md)
