# 07 — Stacks and Queues

> Two rules about *order*. A **stack** gives back the newest thing first
> (LIFO). A **queue** gives back the oldest thing first (FIFO). Almost every
> buffer in a computer — call stacks, UART FIFOs, DMA descriptor rings,
> GStreamer queues, network RX rings — is one of these two.

---

## 1. Why this exists

Often you do not need "find any element" or "keep everything sorted". You
only need **the next one**, and the only question is *which* next one:

| You need...                                   | Use      |
|-----------------------------------------------|----------|
| the thing you started most recently           | stack    |
| the thing that has waited longest             | queue    |
| either end, flexibly                          | deque    |
| the most important / smallest one             | priority queue (heap, lesson 10) |

Restricting access to one or two ends is what makes every operation **O(1)**
and the data structure trivially simple — simple enough to live inside an
interrupt handler or to be shared between two CPU cores *without a lock*.

---

## 2. Mental model

**Stack = a pile of plates.** You put a plate on top and take the top plate
off. The plate at the bottom waits the longest. Your function call stack
works exactly like this: the most recently called function returns first.

**Queue = a supermarket line.** People join at the back, are served at the
front. Nobody overtakes.

**Ring buffer = a circular conveyor belt / a clock face.** The writer's hand
and the reader's hand both move clockwise; the data is everything between
the reader's hand and the writer's hand.

```
 STACK (LIFO)              QUEUE (FIFO)

   push   pop              enqueue                 dequeue
     \    ^                   |                       ^
      v  /                    v                       |
   +------+  <- top        +----+----+----+----+      |
   |  30  |                | 40 | 30 | 20 | 10 | -----+
   +------+                +----+----+----+----+
   |  20  |                 back           front
   +------+
   |  10  |
   +------+

 RING BUFFER (N = 8): an array whose end is glued to its start

        idx:  0   1   2   3   4   5   6   7
            +---+---+---+---+---+---+---+---+
            | a | b | c | d |   |   |   |   |   --+
            +---+---+---+---+---+---+---+---+     |  slot 7 is followed
              ^               ^                   |  by slot 0 again
             head            tail          <------+
         (next read)     (next write)
```

The single sentence to remember: **a stack has one hot end, a queue has two
ends with one job each, a ring buffer is a queue that never moves its data.**

---

## 3. How it works

### 3.1 Stack on an array  (`c/01_stack_array.c`)

One array plus one index `top` = "number of elements" = "next free slot".

```
  data: [ 10 | 20 | 30 |    |    ]
                         ^ top = 3
  push(x): data[top++] = x       pop(): return data[--top]
```

`top == 0` → empty, `top == CAP` → full. A fixed-capacity stack needs no
heap at all (ideal for firmware). A growable one **doubles** the buffer when
full: n pushes cause only log₂n reallocs, and the total copying is
1 + 2 + 4 + … + n < 2n, so push is **amortised O(1)**. Growing by a constant
(+1, +16) would make it O(n²) overall.

### 3.2 Stack on a linked list  (`c/02_stack_linked.c`)

Push and pop at the **head** of a singly linked list — the only place where
both are O(1):

```
  push(30):  n->next = top;  top = n;
     top -> [30] -> [20] -> [10] -> NULL
  pop():     old = top; top = old->next; value = old->value; free(old);
```

No capacity limit, no big realloc copies — but one `malloc` per push and
nodes scattered in memory (cache misses). In practice the array version is
faster; the list version is what you use when elements are already nodes
(intrusive lists, lesson 06).

### 3.3 Queue on a linked list  (`c/04_queue_linked.c`)

Keep **two** pointers: `head` (front, dequeue here) and `tail` (back,
enqueue here). Arrows point from older to newer.

```
  head -> [10] -> [20] -> [30] -> NULL
                           ^ tail
  enqueue(40): tail->next = n; tail = n;
  dequeue():   old = head; head = head->next; free(old);
```

Why not dequeue from the tail? Removing the tail needs the node *before* it,
which a singly linked list cannot reach in O(1). Direction is a design
decision.

The bug everyone writes once: after removing the **last** node, `head`
becomes `NULL` but `tail` still points at freed memory. Always reset
`tail = NULL` when the queue becomes empty.

### 3.4 Ring buffer — the queue that embedded people actually use  (`c/03_ring_buffer.c`)

A fixed array, a read index `head`, a write index `tail`, both wrapping
around:

```
   push: buf[tail] = x; tail = (tail + 1) % N
   pop : x = buf[head]; head = (head + 1) % N

   after wrapping, data may be split in two pieces:
   +---+---+---+---+---+---+---+---+
   | g | h |   |   |   | d | e | f |        logical order: d e f g h
   +---+---+---+---+---+---+---+---+
             ^           ^
            tail        head
```

**The full/empty ambiguity.** Empty means `head == tail`. Now fill the buffer
completely: tail goes all the way round and lands on head again —
`head == tail`. Same picture, two meanings. You need one extra bit of
information. Three standard answers:

| Method | Empty when | Full when | Capacity | Notes |
|---|---|---|---|---|
| **A. Waste one slot** | `head == tail` | `(tail+1) % N == head` | N − 1 | Producer writes only `tail`, consumer only `head` → lock-free friendly. Classic in UART drivers. |
| **B. Keep a count** | `count == 0` | `count == N` | N | Simplest to read, but `count` is written by *both* sides → needs a lock/atomic RMW if two contexts touch it. |
| **C. Free-running indices, N = 2ᵏ** | `tail − head == 0` | `tail − head == N` | N | Indices never wrap explicitly; slot = `i & (N−1)`. Unsigned subtraction survives overflow. **Linux `kfifo`**, DPDK, most DMA rings. |

Method C in one picture (N = 8, 32-bit counters):

```
   head = 4294967293   tail = 5   (tail overflowed past 2^32)
   used = tail - head = 5 - 4294967293 = 8 (mod 2^32)  -> full. Correct!
   slot of head = 4294967293 & 7 = 5
```

Why must N be a power of two for C? (1) `i & (N−1)` equals `i % N` only
then, and a mask is one cycle while `%` is a division. (2) 2³² must be a
multiple of N, or the slot sequence jumps when the counter wraps.

**Overflow policy** is a separate design choice: a UART RX interrupt cannot
block, so on full it *drops* the byte and counts an overrun; an audio
capture ring usually *overwrites* the oldest data; a network stack drops
the newest packet and bumps `rx_dropped`. The program shows the UART case.

### 3.5 Deque  (`c/05_deque.c`)

Push/pop at **both** ends in O(1). On a ring buffer, `push_front` simply
moves `head` one step left with wrap:

```
   [ a | b | _ | _ ]  head = 0   push_front(z)  -> head = (0 - 1) & 3 = 3
   [ a | b | _ | z ]  logical order: z a b
```

Growing must **unroll** the ring into the new buffer in logical order — a
plain `realloc` keeps the physical layout and scrambles the order.

`std::deque` is built differently: a "map" (array of pointers) to
fixed-size blocks. Growing never moves existing elements, so references to
elements stay valid — but it is not one contiguous array.

### 3.6 Monotonic stack and queue  (`c/07_monotonic_stack_queue.c`, `cpp/04_monotonic.cpp`)

A trick, not a new structure: keep the stack/deque **sorted** by throwing
away elements that can *never* be an answer again.

**Next greater element** (decreasing stack of indices). Elements on the stack
are "still waiting for something bigger". When a bigger value arrives, it is
the answer for every smaller waiting element — pop them all.

```
   a = [2, 1, 5, 3, 4]
   x=2  stack [2]
   x=1  stack [2 1]
   x=5  pop 1 -> ans 5, pop 2 -> ans 5      stack [5]
   x=3  stack [5 3]
   x=4  pop 3 -> ans 4                       stack [5 4]
   end: 5, 4 have no answer (-1)           result [5 5 -1 4 -1]
```

**Sliding-window maximum** (decreasing deque of indices). Front = current
max. Before pushing `i`, pop from the back everything `<= a[i]` (older *and*
not bigger: can never win). Pop the front when it slides out of the window.

**Why O(n) despite the inner while loop:** every index is pushed once and
popped at most once — at most 2n operations in total. This "count the
pushes and pops, not the loop nesting" argument is amortised analysis.

### 3.7 Applications that are secretly stacks  (`c/06_brackets_and_postfix.c`, `cpp/03_expression_eval.cpp`)

- **Balanced brackets:** push openers; a closer must match the top; the
  stack must be empty at the end. Three failure modes: closer on empty
  stack, mismatched closer, leftovers.
- **Infix → postfix (Dijkstra's shunting-yard):** numbers go straight out;
  operators wait on a stack until something with lower precedence arrives.
  `(` is pushed, `)` pops until `(`. Right-associative `^` does *not* pop an
  equal-precedence `^`.

  ```
   3 + 4 * 2 - 1
   tok  output        op stack
   3    3
   +    3             +
   4    3 4           +
   *    3 4           + *       (* binds tighter: + stays)
   2    3 4 2         + *
   -    3 4 2 * +     -         (pop * and +: both >= -)
   1    3 4 2 * + 1   -
   end  3 4 2 * + 1 -
  ```
- **Postfix evaluation:** numbers are pushed; an operator pops **b first,
  then a**, pushes `a op b`. That order is the bug magnet for `-` and `/`.

### 3.8 Lock-free SPSC ring buffer  (`c/08_spsc_lockfree_ring.c`, `cpp/05_spsc_ring.cpp`)

With exactly **one producer thread and one consumer thread**, a ring buffer
needs no mutex: the producer is the only writer of `tail`, the consumer the
only writer of `head`. No variable has two writers, so no lock is needed —
but **ordering** is.

```
   producer                       consumer
   buf[t] = x;          (1)       if (tail != head)    (3)
   tail = t + 1;        (2)           y = buf[head];   (4)
```

Modern CPUs (ARM's Cortex-A72 on the TDA4VM especially) and compilers may
make (2) visible to the other core **before** (1). The consumer then sees
the new `tail` and reads a stale slot. The fix is a matched pair:

- **release store** on `tail` (producer): *"everything I wrote before this
  is published together with it."* — seal the envelope, then post it.
- **acquire load** of `tail` (consumer): *"if I see that value, I also see
  everything that was published with it."* — open the envelope, the
  contents are there.

```
   producer:  write buf[t]  ──►  store(tail, release)
                                        │  synchronises-with
   consumer:                      load(tail, acquire)  ──►  read buf[t]
```

The same pair runs the other way on `head`, so the producer never
overwrites a slot the consumer is still reading. A thread loading its *own*
index can use `relaxed` (nobody else writes it). On ARMv8 acquire/release
compile to `LDAR`/`STLR`; on x86 to plain `MOV` — which is why a missing
`release` "works on my PC" and breaks on the board.

Two performance details in the code: each index sits on its own 64-byte
cache line (avoids **false sharing**, two cores fighting over one line), and
the C++ version caches the other side's index so the shared line is touched
only when the ring *looks* full/empty.

SPSC only. With two producers, both could read the same `tail` and write the
same slot — you then need CAS loops (MPMC queues) or a lock.

---

## 4. Complexity table

| Structure / operation | Time | Space | Why |
|---|---|---|---|
| Array stack push | O(1) amortised, O(n) worst | O(n) | doubling: total copies < 2n |
| Array stack pop / peek | O(1) | — | index arithmetic |
| Linked stack push / pop | O(1) | O(n) + pointer per node | head insert/remove; malloc per push |
| Linked queue enqueue / dequeue | O(1) | O(n) + pointer per node | needs both head and tail pointers |
| Ring buffer push / pop | O(1) | O(N) fixed | wrap with `%` or `& mask` |
| Deque push/pop both ends | O(1) amortised | O(n) | ring with wrap; grow unrolls O(n) |
| Deque random access `[i]` | O(1) | — | `(head + i) & mask` |
| Search for a value in any of them | O(n) | — | they are not built for search |
| Next greater element (monotonic stack) | O(n) | O(n) | each index pushed/popped once |
| Sliding window max (monotonic deque) | O(n) | O(k) | same argument; brute force O(n·k) |
| Balanced brackets / postfix eval | O(n) | O(n) | one pass, stack depth ≤ n |
| Shunting-yard infix → postfix | O(n) | O(n) | each operator pushed/popped once |
| `std::priority_queue` push / pop / top | O(log n) / O(log n) / O(1) | O(n) | binary heap in a vector |
| SPSC ring push / pop | O(1), wait-free | O(N) | no loops inside, no locks |

---

## 5. C vs C++

| Concept | C (this lesson) | C++ hand-written | C++ standard library |
|---|---|---|---|
| Stack | array + `top`, or list head | `Stack<T>` adaptor over `std::vector` | `std::stack<T, Container = std::deque<T>>` |
| Queue | list with head+tail, or ring | `RingQueue<T>` with `unique_ptr<T[]>` | `std::queue<T, Container = std::deque<T>>` |
| Deque | growable ring | — | `std::deque<T>` (block map) |
| Priority queue | — (lesson 10) | — | `std::priority_queue<T, vector<T>, less<T>>` = **max**-heap |
| Errors | return `bool` | exceptions / `std::optional<T>` | UB on `top()`/`pop()` of empty! |
| Memory | explicit `malloc`/`free` | RAII: destructor frees | RAII |
| Atomics | `<stdatomic.h>`, `atomic_store_explicit(…, memory_order_release)` | `std::atomic<T>::store(v, std::memory_order_release)` | same |

Things to know about the STL adaptors (`cpp/02_stl_adaptors.cpp`):

- `pop()` returns `void`. Read `top()`/`front()` first. Reason: returning by
  value could throw during the copy *after* the element was removed — the
  element would be lost.
- `std::stack<int, std::vector<int>>` is often faster than the default deque.
- `std::queue` cannot sit on `std::vector` (no `pop_front`).
- `std::priority_queue` with `std::less` is a **max**-heap; use
  `std::greater<T>` for a min-heap. Its comparator means "a has *lower*
  priority than b". It is not stable — encode arrival order if ties matter.
- Calling `top()`/`pop()` on an empty adaptor is undefined behaviour, not an
  exception.

---

## 6. Where it is used in the real world

- **CPU and the call stack.** Every function call pushes a frame (return
  address, saved registers, locals); return pops it. Stack overflow is
  literally this stack running out of its fixed region. Recursion = implicit
  stack; converting recursion to iteration = making the stack explicit.
- **Linux kernel.**
  - `include/linux/kfifo.h`: a generic ring buffer, power-of-two size,
    free-running `in`/`out` counters, lock-free for one reader + one writer
    (method C above, with `smp_wmb()`/`smp_rmb()` barriers).
  - `include/linux/circ_buf.h` + `Documentation/core-api/circular-buffers.rst`:
    `CIRC_CNT`/`CIRC_SPACE` macros, the waste-one-slot style (method A),
    used by serial drivers (`struct uart_state` → `xmit` buffer).
  - Network drivers: RX/TX **descriptor rings** shared with the NIC's DMA
    engine (`ethtool -g eth0` shows their sizes). On the TDA4VM, the CPSW
    Ethernet and the UDMA engine use rings of descriptors.
  - `io_uring`: two SPSC rings in shared memory (submission and completion)
    between user space and the kernel, using acquire/release on head/tail.
  - Workqueues and the block layer: FIFO request queues.
- **Audio.** ALSA's PCM buffer is a ring of *periods*; the DMA writes, the
  app reads; an "xrun" is the ring becoming full (overrun) or empty
  (underrun). JACK and PipeWire use lock-free SPSC rings between the
  real-time thread and the rest.
- **GStreamer.** The `queue` element is a thread boundary with a FIFO
  inside (`max-size-buffers/bytes/time`, `leaky=upstream|downstream` = the
  overflow policy from section 3.4). `GstAdapter` is a byte FIFO;
  `GstBufferPool` recycles buffers through a queue.
- **Embedded/RTOS.** FreeRTOS queues, Zephyr `k_msgq` and `ring_buf`,
  UART RX buffers filled from an ISR, inter-processor communication between
  the A72 and the R5F/C7x cores on the TDA4VM (RPMsg uses virtio rings
  — "vrings" — in shared memory).
- **Compilers and interpreters.** Parsers use stacks (shift-reduce
  parsing), the JVM, CPython and WebAssembly are *stack machines*,
  expression evaluation uses shunting-yard.
- **Algorithms.** DFS = stack, BFS = queue (lesson 13), undo/redo = two
  stacks, browser back/forward = two stacks, schedulers and Dijkstra =
  priority queues.

---

## 7. Common mistakes and pitfalls

1. **Pop/peek on empty** — in C reads garbage or out of bounds; with
   `std::stack` it is UB. Always check `empty()` first.
2. **Overflow on a fixed array stack** — `data[top++]` with `top == CAP`
   silently corrupts the next variable. ASan catches it for heap/stack
   arrays; on a microcontroller nothing does.
3. **`realloc` straight into the pointer**: `p = realloc(p, n)` leaks the
   old block when it fails. Use a temporary.
4. **Dangling tail in a linked queue** after the last dequeue.
5. **Ring buffer full vs empty confusion** — pick one of the three methods
   and stick to it. Mixing "count" and "head==tail" logic is the usual bug.
6. **Using `%` with a non-power-of-two size and free-running counters** —
   breaks at counter wrap-around. Use method C only with N = 2ᵏ.
7. **Wrong operand order in postfix evaluation** — first pop is the
   *right* operand.
8. **Plain `realloc` to grow a ring/deque** — the wrapped part ends up in
   the wrong place. Unroll in logical order.
9. **Lock-free ring with plain variables or `volatile`** — `volatile` does
   not order memory between cores in C/C++. Use atomics with
   release/acquire (or the kernel's `smp_store_release`/`smp_load_acquire`).
10. **SPSC ring used by two producers** (e.g. two threads, or a thread and
    an ISR on another core both writing) — needs MPMC design or a lock.
11. **False sharing** — head and tail in the same cache line can make a
    lock-free ring slower than a mutex one. Align each to 64 bytes.
12. **Testing concurrency only on x86** — x86 hides most ordering bugs.
    Run ThreadSanitizer (`-fsanitize=thread`, cannot be combined with
    ASan) and test on the ARM board. On recent kernels TSan may die with
    "unexpected memory mapping"; run it as `setarch $(uname -m) -R ./prog`.
13. **`std::priority_queue` direction** — default is max-heap. "Why is my
    min-heap returning the biggest?" — because `std::less` means max.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_stack_array.c](c/01_stack_array.c) | Fixed and growable array stack, amortised doubling, reversing a string | `make run P=dsa/07-stacks-and-queues/c/01_stack_array.c` |
| [c/02_stack_linked.c](c/02_stack_linked.c) | Stack on a linked list, pointer rewiring, undo history | `make run P=dsa/07-stacks-and-queues/c/02_stack_linked.c` |
| [c/03_ring_buffer.c](c/03_ring_buffer.c) | Ring buffer three ways (waste slot, count, free-running + mask), counter wrap, UART overrun | `make run P=dsa/07-stacks-and-queues/c/03_ring_buffer.c` |
| [c/04_queue_linked.c](c/04_queue_linked.c) | Queue with head+tail pointers, dangling-tail bug avoided, BFS order | `make run P=dsa/07-stacks-and-queues/c/04_queue_linked.c` |
| [c/05_deque.c](c/05_deque.c) | Growable circular deque, push/pop both ends, unrolling on grow, stress test, palindrome | `make run P=dsa/07-stacks-and-queues/c/05_deque.c` |
| [c/06_brackets_and_postfix.c](c/06_brackets_and_postfix.c) | Balanced brackets, shunting-yard infix→postfix, postfix evaluation | `make run P=dsa/07-stacks-and-queues/c/06_brackets_and_postfix.c` |
| [c/07_monotonic_stack_queue.c](c/07_monotonic_stack_queue.c) | Next greater element, sliding-window max, checked against brute force | `make run P=dsa/07-stacks-and-queues/c/07_monotonic_stack_queue.c` |
| [c/08_spsc_lockfree_ring.c](c/08_spsc_lockfree_ring.c) | Lock-free SPSC ring with C11 atomics + pthreads, acquire/release | `make run P=dsa/07-stacks-and-queues/c/08_spsc_lockfree_ring.c` |
| [cpp/01_stack_queue_template.cpp](cpp/01_stack_queue_template.cpp) | `Stack<T>` adaptor and RAII `RingQueue<T>`, `optional`, move semantics | `make run P=dsa/07-stacks-and-queues/cpp/01_stack_queue_template.cpp` |
| [cpp/02_stl_adaptors.cpp](cpp/02_stl_adaptors.cpp) | `std::stack`, `std::queue`, `std::deque`, `std::priority_queue` (max, min, custom scheduler) | `make run P=dsa/07-stacks-and-queues/cpp/02_stl_adaptors.cpp` |
| [cpp/03_expression_eval.cpp](cpp/03_expression_eval.cpp) | Brackets, shunting-yard and RPN with `std::stack` and `string_view` | `make run P=dsa/07-stacks-and-queues/cpp/03_expression_eval.cpp` |
| [cpp/04_monotonic.cpp](cpp/04_monotonic.cpp) | Next greater, daily temperatures, largest rectangle in histogram, sliding max | `make run P=dsa/07-stacks-and-queues/cpp/04_monotonic.cpp` |
| [cpp/05_spsc_ring.cpp](cpp/05_spsc_ring.cpp) | Template SPSC ring with `std::atomic`, cached indices, cache-line alignment, strings across threads | `make run P=dsa/07-stacks-and-queues/cpp/05_spsc_ring.cpp` |

---

## 9. Exercises

**Easy**
1. Add `fs_size()` and a `print` function to the fixed stack in `01`.
2. Implement a queue with **two stacks** (`in` and `out`; move all of `in` to
   `out` only when `out` is empty). Prove dequeue is amortised O(1).
   *Hint:* each element is moved at most once.
3. Make the ring buffer in `03` method A *overwrite the oldest* element when
   full instead of refusing (audio-style).

**Medium**
4. **Min-stack:** a stack that also returns its minimum in O(1).
   *Hint:* push a second stack of "minimum so far".
5. Implement a stack with **one queue** (push is O(n)).
6. Add `stack shrinking` to `01`: halve the capacity when the stack is a
   quarter full. Why a quarter and not a half? *Hint:* think of a push/pop
   sequence right at the boundary.
7. Extend the shunting-yard in `06` with unary minus (`-3 + 4`) and
   multi-letter functions like `max(a, b)`.

**Hard**
8. **Bulk operations on the SPSC ring:** `push_n(const T*, n)` and
   `pop_n(T*, n)` that copy in at most two `memcpy`s (the wrap split).
   This is how `kfifo_in()` works — read `lib/kfifo.c`.
9. Build a **blocking** bounded queue for many producers/consumers with a
   mutex and two condition variables (`not_full`, `not_empty`). Then
   explain why `while (full) wait(...)` must be `while`, not `if`.
10. Change `cpp/05_spsc_ring.cpp` to use `memory_order_relaxed` for the
    `tail` store, build with `-fsanitize=thread` (no ASan), and read the
    report. Then cross-compile it for the BeagleBone AI-64 and run it with
    a `struct` payload whose two fields must always match — try to catch a
    torn read on real ARM hardware.
11. Largest rectangle of 1s in a binary matrix. *Hint:* each row is a
    histogram; reuse `largest_rectangle` from `cpp/04`.

---

## 10. Remember forever

```
STACK  = one end.  push: a[top++]=x   pop: x=a[--top]        LIFO
QUEUE  = two ends. list: insert at tail, remove at head;  reset tail when empty
RING   : head=read, tail=write, wrap.  head==tail is EMPTY... and FULL
         -> waste a slot | keep count | free-running idx, N=2^k, used=tail-head
MONOTONIC: pop what can never win; each index in+out once => O(n)
POSTFIX: number->push; op-> b=pop, a=pop, push(a op b)
SPSC   : producer owns tail, consumer owns head.
         write data, then store(tail, RELEASE); load(tail, ACQUIRE), then read
std::priority_queue default = MAX-heap; adaptors' pop() returns void
```

Review this card with the schedule in
[../../docs/LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why is pushing onto a doubling array stack O(1) *amortised* but not O(1)
   worst case? What is the total copy cost of n pushes?
2. In a linked-list queue, why does the head (front) pointer point to the
   *oldest* element and not the newest?
3. Draw a ring buffer with N = 4 that is full, and one that is empty. Why
   do they look the same, and what are three ways to tell them apart?
4. Why must a free-running-index ring have a power-of-two size?
5. Which ring-buffer method is safe for lock-free SPSC use and why is the
   `count` method not?
6. In sliding-window maximum, why can we discard an older element that is
   smaller than the new one? Why is the whole algorithm O(n)?
7. Convert `(1 + 2) * 3 - 4 / 2` to postfix by hand and evaluate it.
8. Explain release and acquire in one sentence each. What can go wrong on
   ARM if the producer stores `tail` with `relaxed`?
9. Why is `volatile` not enough for a ring shared between two cores?
10. What does the GStreamer `queue` element's `leaky` property correspond
    to in ring-buffer terms?
11. Why does `std::stack::pop()` return `void`?
12. Name three places in the Linux kernel where a ring buffer is used.
