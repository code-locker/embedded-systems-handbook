# 10 — Heaps and priority queues

> One sentence to keep forever: **a heap is a complete binary tree stored in an
> array, where every parent beats its children — so the winner is always at
> index 0, and fixing the tree after a change only walks one path: O(log n).**

Prerequisites: arrays ([05](../05-arrays-and-strings/)), binary-tree vocabulary
([09-trees](../09-trees/README.md): root, leaf, height, *complete* tree).

---

## 1. Why this exists

Many programs keep asking one question over and over:

* "Which timer expires **next**?" (every event loop, the kernel's hrtimers, GStreamer clock waits)
* "Which task is **most urgent**?" (schedulers, interrupt bottom halves, job queues)
* "What are the **k largest** values so far?" (top-k, monitoring, leaderboards)
* "Which of these k sorted streams has the **smallest** next element?" (merging logs, LSM compaction)

You could keep everything **sorted** — but then inserting costs O(n) (shift the
array) or you need a balanced tree (pointers, rotations, cache misses). You
could keep it **unsorted** — insert is O(1) but finding the minimum is O(n).

A heap sits in the sweet spot: you do **not** need the whole order, only the
**best** element. By keeping only a *partial* order it gets:

| operation          | sorted array | unsorted array | **binary heap** |
|--------------------|-------------|----------------|-----------------|
| insert             | O(n)        | O(1)           | **O(log n)**    |
| peek best          | O(1)        | O(n)           | **O(1)**        |
| remove best        | O(1)/O(n)   | O(n)           | **O(log n)**    |
| build from n items | O(n log n)  | O(1)           | **O(n)**        |

and it needs **no pointers at all**: just an array. That is why it is the
default "priority queue" in almost every standard library.

---

## 2. Mental model

**Analogy: a company org chart where every boss is faster than their direct
reports.** The fastest person in the company must be the CEO (the root). You do
**not** know whether someone in the marketing branch is faster than someone in
engineering — and you don't care. When the CEO leaves, the best of the two
VPs gets promoted, then the best of *their* reports fills the gap, and so on
down **one** chain of command. When a new hire arrives, they start at the
bottom and get promoted while they beat their boss.

**Min-heap property:** `parent <= child` for every edge.
(Max-heap: `parent >= child`.) Siblings are *not* ordered.

```
          tree view                         array view (0-based)
                1                      index:  0   1   2   3   4   5   6   7
             /     \                         +---+---+---+---+---+---+---+---+
           3         2                       | 1 | 3 | 2 | 7 | 4 | 5 | 9 | 8 |
          / \       / \                      +---+---+---+---+---+---+---+---+
         7   4     5   9                       ^   \___/   \___________/   \..
        /                                     root level 1    level 2     level 3
       8
```

**Complete** means every level is full except possibly the last, which is
filled **from the left**. That single rule is what lets the tree live in an
array with no gaps: reading the tree level by level, left to right, *is* the
array.

### Array representation (the index arithmetic)

```
0-based (C, C++, this repo)          1-based (many textbooks, CLRS)
  parent(i) = (i - 1) / 2              parent(i) = i / 2
  left(i)   = 2*i + 1                  left(i)   = 2*i
  right(i)  = 2*i + 2                  right(i)  = 2*i + 1
```

Why it works: level `d` holds 2^d nodes, starting at index 2^d − 1. Node `i`'s
children are the next free pair one level down — and since every level before
them is full, that pair is at `2i+1, 2i+2`.

Facts you get for free:

* **Leaves are the second half**: indices `n/2 … n-1`. (Build-heap starts at `n/2 - 1`.)
* **Height = floor(log2 n)**: a million elements is only 20 levels.
* **Memory**: one contiguous array — excellent cache behaviour, zero pointer
  overhead, trivially `realloc`-able. Compare a BST node: value + 2 pointers
  (+ colour/height) = 3–4x the memory and a cache miss per level.

---

## 3. How it works

All operations are built from **two repair moves**. Learn these two and you
know heaps.

### Sift-up (a.k.a. bubble-up, swim, `push_heap`)

Used after **inserting at the end**. The new element climbs while it beats its
parent.

```
push 0 into min-heap [1,3,2,7,4]

  step 1: append at the        step 2: 0 < 2,             step 3: 0 < 1,
  first free slot (index 5)    swap with parent           swap; now root: stop
          1                           1                           0
        /   \                       /   \                       /   \
       3     2                     3     0                     3     1
      / \   /                     / \   /                     / \   /
     7   4 [0]                   7   4 2                     7   4 2

array: [1,3,2,7,4,0]  ->  [1,3,0,7,4,2]  ->  [0,3,1,7,4,2]
```

Cost: at most one swap per level → **O(log n)**.

### Sift-down (a.k.a. heapify, sink, `__adjust_heap`)

Used after the **root is replaced**. The element sinks, each time swapping with
its **better** child (smaller child in a min-heap).

```
pop from min-heap [0,3,1,7,4,2]

  step 1: remember root 0,     step 2: children 3 and 1;    step 3: 2 has no
  move LAST (2) to the root    smaller is 1: swap           children: stop
          2                           1                           1
        /   \                       /   \                       /   \
       3     1                     3     2                     3     2
      / \                         / \                         / \
     7   4                       7   4                       7   4
```

**Why the smaller child?** Whichever child moves up becomes the parent of the
other one, so it must be the smaller of the two. Swap with the larger and you
create a new violation right there.

**Why move the last element to the root?** Removing the root leaves a hole;
the only element we can remove without breaking *completeness* is the last
one. So it fills the hole and sift-down fixes the *order*.

### The operations

| operation      | how                                                        |
|----------------|------------------------------------------------------------|
| `push(x)`      | append at `a[n]`, `n++`, sift-up from `n-1`                 |
| `peek()`       | return `a[0]`                                               |
| `pop()`        | save `a[0]`; `a[0] = a[n-1]`; `n--`; sift-down from 0       |
| `replace(x)`   | `a[0] = x`; sift-down (pop+push in one pass, used in top-k) |
| `remove(i)`    | `a[i] = a[n-1]`; `n--`; sift-up **or** sift-down from `i`   |
| `decrease_key` | change `a[i]` to a better value; sift-up from `i`           |

`remove(i)` must try **both** directions: the last element came from a
different subtree, so it might be smaller than `i`'s parent (go up) or larger
than `i`'s children (go down). It never needs both.

### Build-heap in O(n) and why

Given n unordered values, n pushes cost O(n log n). Floyd's method is faster:

```
for i = n/2 - 1 down to 0:  sift_down(i)
```

Leaves (the second half) are already valid one-element heaps. Walking
backwards, when we sift node `i` down, both its subtrees are already heaps, so
one sift-down makes the subtree at `i` a heap.

```
build max-heap from [3,9,2,1,4,5]   (n=6, start at i = 2)

 start          i=2: 2 < 5, swap   i=1: 9 >= 1,4: ok   i=0: 3 sinks twice
     3               3                  3                    9
    / \             / \                / \                  / \
   9   2    ->     9   5     ->       9   5      ->        4   5
  / \  /          / \  /             / \  /               / \  /
 1  4 5          1  4 2             1  4 2               1  3 2
```

**Why O(n), not O(n log n)?** A node can only sink as far as its *height*, and
most nodes are near the bottom:

```
height h from bottom   nodes at that height   max swaps each
        0  (leaves)           n/2                  0
        1                     n/4                  1
        2                     n/8                  2
        3                     n/16                 3
total  <=  n * (1/4 + 2/8 + 3/16 + 4/32 + ...)  =  n * 1  =  O(n)
```

Pushes are the opposite: the many leaves are the ones that may climb the full
height. **Sift-down is cheap where the nodes are; sift-up is expensive where
the nodes are.** `03_build_heap_max.c` measures it: on 65,536 ascending values
Floyd does 65,521 swaps, n pushes do 917,522.

### Heapsort

1. Build a **max**-heap in place: O(n).
2. Repeat n−1 times: swap `a[0]` (the max) with the last heap slot, shrink the
   heap by one, sift-down the new root: O(log n) each.

```
[9 5 8 1 3 2]           heap | sorted
swap 9 <-> 2, sift:     [8 5 2 1 3 | 9]
swap 8 <-> 3, sift:     [5 3 2 1 | 8 9]
swap 5 <-> 1, sift:     [3 1 2 | 5 8 9]
...                     [1 | 2 3 5 8 9]
```

Max-heap gives **ascending** order because the max goes to the end each time.
O(n log n) **worst case**, O(1) extra memory, **not stable** (equal keys get
reordered by the long-distance swaps).

### Indexed heap / decrease-key

A plain heap cannot find "the entry for vertex 17" without an O(n) scan. Keep
a side array `pos[key] = index in heap` and update it on **every** move
(do all moves through one `swap_slots()`/`place()` helper so you can't forget).
Then `contains` is O(1) and `decrease_key` / `erase` are O(log n). Used by
Dijkstra and Prim — and it is exactly what an event loop does to cancel a
timer (libevent stores `min_heap_idx` inside each event).

The C++ alternative when the STL heap can't do it: **lazy deletion** — push a
new entry, mark the old one stale, and skip stale entries when they reach the
top. Simpler; the heap may grow to O(number of updates).

### Applications

**Top-k largest of a stream — use a MIN-heap of size k.** (The surprising
part: the *min*-heap.) Its root is the *smallest of the current top-k*, the
bar a newcomer must beat. Each new value: if it beats the root, `replace`
the root; else drop it in O(1). O(n log k) time, O(k) memory, works on an
endless stream.

```
k=3, stream 5 1 9 3 7 2 8
after 5 1 9:  min-heap {1,5,9}   bar = 1
3 > 1:        {3,5,9}            bar = 3
7 > 3:        {5,7,9}            bar = 5
2 < 5:        ignored            (the common, cheap case)
8 > 5:        {7,8,9}            answer
```

**Merge k sorted lists — min-heap of the k current heads.** Pop the smallest
head, output it, push that list's next element. O(N log k). This is the inner
loop of external merge sort, `sort -m`, merging per-CPU trace buffers by
timestamp, and LSM-tree compaction in LevelDB/RocksDB.

**Running median — two heaps.**

```
     lo: MAX-heap                hi: MIN-heap
  (smaller half)               (larger half)
  [1 3 5 | top 7]   <=   [top 8 | 9 10 15]
                \           /
          median = (7 + 8) / 2
```

Invariants: every value in `lo` ≤ every value in `hi`, and `|lo| = |hi|` or
`|hi| + 1`. Insert: push to `lo` if `x <= lo.top()` else to `hi`, then move one
top across if the sizes drift. O(log n) insert, O(1) median.

### Timer queues and schedulers

An event loop's core is:

```
for (;;) {
    timeout = heap_empty ? -1 : heap_top.expires - now;   // O(1) peek
    epoll_wait(fd, events, n, timeout);                    // sleep exactly that long
    handle_io(events);
    while (heap_top.expires <= now) fire(pop());           // O(log n) each
}
```

Real systems:

* **libevent** (`minheap-internal.h`) and **libuv** (a heap of timers) use a
  binary min-heap; each timer stores its own heap index for O(log n) cancel.
* **systemd sd-event** uses `prioq`, an indexed heap of the same shape.
* **GLib main loop / GStreamer**: a `GstClockID` wait is "wake me at time T";
  `GstSystemClock` keeps pending async waits ordered by time and a thread
  sleeps until the earliest one; `gst_clock_id_unschedule()` is cancel. The
  sinks' "wait until this buffer's running-time" (sync=true) is exactly a timer
  queue entry.
* **Linux kernel**: `hrtimers` use a **red-black tree** (timerqueue), not a heap,
  because they need cheap arbitrary removal plus a cached leftmost node; the
  classic `timer_list` wheel trades precision for O(1). Same *interface*
  (next-to-expire), different structure — a nice lesson that a "priority queue"
  is an abstract interface.
* **Go runtime** timers: a 4-ary heap per P. **Java `PriorityQueue`,
  `DelayQueue`, `ScheduledThreadPoolExecutor`**: binary heaps.

Schedulers: SCHED_DEADLINE picks the earliest deadline (EDF) — a priority
queue keyed on deadline (the kernel uses an rbtree). CFS picked the smallest
vruntime (rbtree, leftmost cached).

---

## 4. Complexity table

| operation              | time          | space | why                                              |
|------------------------|---------------|-------|--------------------------------------------------|
| peek / top             | O(1)          | —     | best element is always at index 0                |
| push                   | O(log n) amortised | — | one sift-up path; vector growth amortised      |
| pop                    | O(log n)      | —     | one sift-down path                               |
| replace top            | O(log n)      | —     | one sift-down instead of pop+push                |
| build-heap (Floyd)     | **O(n)**      | O(1)  | sum of node heights is < n                       |
| build by n pushes      | O(n log n)    | O(1)  | many leaves may climb the full height            |
| search arbitrary value | O(n)          | —     | heap is only partially ordered                   |
| remove / decrease key  | O(log n)      | O(n) for pos[] | needs an index map to *find* the slot     |
| heapsort               | O(n log n) worst | O(1) | n-1 pops, in place; not stable               |
| top-k of n             | O(n log k)    | O(k)  | heap never exceeds k                             |
| merge k lists (N total)| O(N log k)    | O(k)  | heap holds one head per list                     |
| running median insert  | O(log n)      | O(n)  | push + at most one rebalance                     |
| merge two heaps        | O(n)          | —     | concat + build-heap (binomial/Fibonacci heaps do better) |

Fibonacci heaps give O(1) amortised decrease-key (theory behind Dijkstra's
O(E + V log V)), but their constants and pointer chasing make binary or 4-ary
heaps faster in practice.

---

## 5. C vs C++

**C**: no generic container. You write the two sift functions for your element
type (often an array of pointers to structs, so swaps are cheap), or a generic
version with `void *` + element size + comparator like `qsort` — see
`04_heapsort.c`. You own the buffer: `realloc` to grow, `free` at the end.

**C++**:

```cpp
std::priority_queue<int> maxq;                                      // max-heap
std::priority_queue<int, std::vector<int>, std::greater<>> minq;    // min-heap
auto later = [](const Req& a, const Req& b){ return a.deadline > b.deadline; };
std::priority_queue<Req, std::vector<Req>, decltype(later)> edf(later);  // earliest first

std::make_heap(v.begin(), v.end());   // O(n) Floyd
std::push_heap(v.begin(), v.end());   // after v.push_back(x)
std::pop_heap(v.begin(), v.end());    // best moved to v.back(); then v.pop_back()
std::sort_heap(v.begin(), v.end());   // heapsort
```

The rule that confuses everyone: **the comparator says "is a lower priority
than b?"; the top is the element no other element beats.** So `std::less`
(default) = max on top, `std::greater` = min on top, and "earliest deadline
first" is written with `>`.

What `std::priority_queue` cannot do: iterate, find, erase, or decrease-key.
Its `top()` returns `const&`, so you can't move a `unique_ptr` out of it — use
the heap algorithms on a `std::vector` you own instead (see
`04_merge_k_and_topk.cpp`). Need erase/decrease-key? Use an indexed heap
(`06_indexed_heap_dijkstra.cpp`), lazy deletion (`05_event_loop_scheduler.cpp`),
or a `std::set` / `std::multimap` (a red-black tree; ordered, erasable, but
~3x the memory and slower constants).

---

## 6. Where it is used in the real world

* **Linux kernel**: `lib/sort.c` is a **heapsort** (O(1) memory and no
  O(n²) worst case — both matter in the kernel); `include/linux/min_heap.h` is
  a generic min-heap used by perf events and bcache/bcachefs; the I/O and CPU
  schedulers use rbtrees as their priority queues.
* **Event loops**: libevent, libuv (Node.js), sd-event, Boost.Asio deadline
  timers, Netty's scheduled tasks — min-heaps of timers.
* **GStreamer**: clock waits (`GstClockID`), `queue`/`multiqueue` scheduling by
  running-time, `aggregator`/`compositor` picking the earliest buffer across
  pads — all "next by timestamp" problems.
* **Networking**: priority queuing disciplines, packet schedulers (WFQ picks the
  smallest virtual finish time), TCP retransmission timers.
* **Algorithms**: Dijkstra, Prim, A*, Huffman coding (repeatedly take the two
  least frequent), event-driven simulation, k-way merge in external sorting.
* **Databases**: top-k for `ORDER BY ... LIMIT k`, merge phases of sort/joins,
  LSM compaction merging k sorted runs.
* **C++ `std::sort`** (introsort) switches to heapsort when recursion is too
  deep, guaranteeing O(n log n).

---

## 7. Common mistakes and pitfalls

1. **Wrong index formula**: mixing 0-based `2i+1` with 1-based `i/2`. Pick one;
   in C use 0-based and `(i-1)/2`.
2. **`(i-1)/2` with `size_t i = 0`** underflows to a huge number. Guard with
   `while (i > 0)`.
3. **Sifting down with the wrong child**: always pick the *better* child, and
   check `r < n` before reading `a[r]`.
4. **Build-heap loop going forwards** (0 → n/2) — wrong: subtrees must already
   be heaps. Go backwards. With `size_t`, write `for (i = n/2; i-- > 0;)`.
5. **Inverted comparator in C++**: `std::priority_queue<int>` is a *max*-heap.
   "Earliest first" needs `>` (or `std::greater`).
6. **A comparator that is not a strict weak ordering** (`<=`, or inconsistent
   tie-breaks) — undefined behaviour in the STL heap algorithms.
7. **Expecting stability**: equal priorities come out in arbitrary order. Add a
   sequence number as a tie-breaker when FIFO among equals matters (timers!).
8. **Expecting a sorted array**: `a[1]` is *not* the second smallest; only
   `a[0]` is special. Printing the array does not show sorted order.
9. **Forgetting to update the stored index** in an indexed heap on one of the
   swap paths — route every move through a single helper.
10. **Cancel-then-fire races**: in an event loop, remove the timer from the
    heap *before* calling its callback, so the callback may safely re-arm or
    free it.
11. **`return a - b;` comparators** overflow for large ints. Use
    `(a > b) - (a < b)`.
12. **Using a heap when you need search or ordered iteration**: that is a
    balanced BST's job (see [09-trees](../09-trees/README.md)).

---

## 8. Files in this lesson

| file | what it shows | run |
|------|---------------|-----|
| [c/01_heap_index_arithmetic.c](c/01_heap_index_arithmetic.c) | array ⇄ tree, parent/child formulas, leaves = second half, heap check | `make run P=dsa/10-heaps-priority-queues/c/01_heap_index_arithmetic.c` |
| [c/02_min_heap.c](c/02_min_heap.c) | growable min-heap: push/pop/peek, sift-up/down, random check vs qsort | `make run P=dsa/10-heaps-priority-queues/c/02_min_heap.c` |
| [c/03_build_heap_max.c](c/03_build_heap_max.c) | max-heap, Floyd build-heap O(n) vs n pushes (swap counts) | `make run P=dsa/10-heaps-priority-queues/c/03_build_heap_max.c` |
| [c/04_heapsort.c](c/04_heapsort.c) | in-place heapsort, int and generic `qsort`-style versions, instability | `make run P=dsa/10-heaps-priority-queues/c/04_heapsort.c` |
| [c/05_topk_and_merge_k.c](c/05_topk_and_merge_k.c) | top-k with size-k min-heap; k-way merge of sorted arrays | `make run P=dsa/10-heaps-priority-queues/c/05_topk_and_merge_k.c` |
| [c/06_timer_queue.c](c/06_timer_queue.c) | event-loop timer queue, heap index per timer, O(log n) cancel, periodic timers | `make run P=dsa/10-heaps-priority-queues/c/06_timer_queue.c` |
| [cpp/01_priority_queue_template.cpp](cpp/01_priority_queue_template.cpp) | `PriorityQueue<T, Compare>` template, move-only `unique_ptr` payloads, STL heap algorithms | `make run P=dsa/10-heaps-priority-queues/cpp/01_priority_queue_template.cpp` |
| [cpp/02_std_priority_queue_comparators.cpp](cpp/02_std_priority_queue_comparators.cpp) | `std::priority_queue` with `greater`, struct and lambda comparators, FIFO ties, EDF | `make run P=dsa/10-heaps-priority-queues/cpp/02_std_priority_queue_comparators.cpp` |
| [cpp/03_running_median.cpp](cpp/03_running_median.cpp) | running median with a max-heap + min-heap | `make run P=dsa/10-heaps-priority-queues/cpp/03_running_median.cpp` |
| [cpp/04_merge_k_and_topk.cpp](cpp/04_merge_k_and_topk.cpp) | merge k `unique_ptr` linked lists by relinking; top-k frequent words | `make run P=dsa/10-heaps-priority-queues/cpp/04_merge_k_and_topk.cpp` |
| [cpp/05_event_loop_scheduler.cpp](cpp/05_event_loop_scheduler.cpp) | event loop with timers, lazy cancellation, GStreamer-style clock waits | `make run P=dsa/10-heaps-priority-queues/cpp/05_event_loop_scheduler.cpp` |
| [cpp/06_indexed_heap_dijkstra.cpp](cpp/06_indexed_heap_dijkstra.cpp) | indexed heap with decrease-key/erase; Dijkstra indexed vs lazy vs Bellman-Ford | `make run P=dsa/10-heaps-priority-queues/cpp/06_indexed_heap_dijkstra.cpp` |

Every program checks itself against a simple reference (qsort, `std::sort`,
`std::priority_queue`, brute force) with randomised inputs.

---

## 9. Exercises

1. **(easy)** By hand, draw the min-heap after pushing 8, 3, 5, 1, 9, 2 one at
   a time. Then pop twice. Check with `02_min_heap.c`.
2. **(easy)** Convert `02_min_heap.c` to a max-heap by changing *only* the
   comparisons. *Hint: flip every key comparison (three of them) and nothing else.*
3. **(easy)** Is `[1, 2, 3, 4, 5, 6]` a min-heap? Is `[1, 3, 2, 4]`? Is a
   sorted array always a heap? Is a heap ever sorted?
4. **(medium)** Find the k-th *smallest* of n numbers in O(n log k).
   *Hint: a MAX-heap of size k.*
5. **(medium)** Add `heap_replace()` and `heap_pushpop()` (push then pop in one
   sift) to `02_min_heap.c`. *Hint: pushpop: if x <= top, just return x.*
6. **(medium)** Make the C heap generic over element size with a comparator
   and a user context pointer (like the kernel's `min_heap_callbacks`).
7. **(medium)** Write a **d-ary heap** (children `d*i+1 … d*i+d`). Measure
   push/pop for d = 2, 4, 8. *Hint: wider = shallower = fewer cache misses on
   sift-up, but more comparisons on sift-down.*
8. **(hard)** Sliding-window median: like `03_running_median.cpp` but values
   also *leave* the window. *Hint: lazy deletion with a "to delete" hash map,
   or two `std::multiset`s.*
9. **(hard)** Turn `06_timer_queue.c` into a real event loop: `timerfd` or
   `epoll_wait` with the computed timeout, on your BeagleBone. Measure jitter
   of a 10 ms periodic timer with and without `SCHED_FIFO`.
10. **(hard)** Implement Huffman coding: build the tree by repeatedly popping
    the two least-frequent nodes. Verify encode/decode round-trips.

---

## 10. Remember forever

```
HEAP = complete binary tree in an array; parent beats children; best at a[0].
0-based: parent (i-1)/2, children 2i+1, 2i+2. Leaves: i >= n/2.
push = append + sift-UP.   pop = last -> root + sift-DOWN (swap with BETTER child).
remove(i) = last -> i, then sift up OR down.  All O(log n); peek O(1).
build-heap: sift-down from n/2-1 DOWN TO 0  => O(n) (most nodes are near the bottom).
heapsort: max-heap, swap root to end, shrink, sift-down. O(n log n), in place, unstable.
top-k LARGEST => MIN-heap of size k (root = bar to beat).  median => max-heap | min-heap.
C++: priority_queue<T> is MAX; greater<> => MIN; comparator answers "a below b?".
Timers: min-heap by expiry (+seq for FIFO); store heap index for O(log n) cancel.
```

Review this card on the schedule in
[docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why can a heap live in an array with no pointers, while a BST usually can't?
2. Write the parent and child formulas for 0-based and 1-based arrays.
3. In sift-down, why must you swap with the *smaller* child of a min-heap?
4. Why does pop move the **last** element to the root, not the smaller child?
5. Prove (or sketch) why build-heap is O(n). Why is n pushes O(n log n)?
6. Why does build-heap loop backwards from `n/2 - 1`?
7. For top-k largest, why a min-heap and not a max-heap? What is the memory cost?
8. How does removing an arbitrary element differ from pop? Why may it need sift-up?
9. What does `std::priority_queue<int, std::vector<int>, std::greater<int>>` give you, and why does `greater` mean *min*?
10. How would you cancel a timer in O(log n)? What is lazy deletion and when is it preferable?
11. Why does the Linux kernel's `sort()` use heapsort rather than quicksort?
12. Why do Linux hrtimers use an rbtree instead of a heap, if both answer "next to expire"?
13. Heapsort is O(n log n) worst case; quicksort is O(n²) worst case. Why is quicksort still usually faster?
