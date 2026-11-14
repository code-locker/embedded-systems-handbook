# 19 — System design structures

Practical data structures built from the earlier lessons: an **LRU cache**, an
**LFU cache**, a **ring buffer** for producer/consumer, an **object pool / slab
allocator**, a **sparse matrix**, a **token-bucket rate limiter**,
**consistent hashing** and a **timer wheel**. None of them is a new idea.
Each one combines two simple parts, a hash map, a linked list, an array, a
binary search or a counting sort, to get a guarantee that neither part gives
on its own.

> Prerequisites: hashing (08), linked lists (06), queues (07), heaps (10),
> binary search and counting sort (14), pointers and memory (01, 02).

---

## 1. Why this exists

Real systems keep asking the same few questions:

| Question | Structure |
|---|---|
| "I can only keep N things in fast memory. Which one do I drop?" | **LRU / LFU cache** |
| "One thread produces data and another consumes it, at different speeds." | **Ring buffer** (bounded queue) |
| "I malloc/free millions of same-size objects and it is slow and fragmented." | **Object pool / slab** |
| "My matrix is 10 000 x 10 000 but 99.95 % zeros." | **Sparse matrix (CSR)** |
| "Allow at most r requests/s per client, with small bursts." | **Token bucket** |
| "Spread keys over N servers so that adding a server moves only a few keys." | **Consistent hashing** |
| "Keep 100 000 timeouts, most of them cancelled before they fire." | **Timer wheel** |

Each answer is an O(1) or O(log n) structure that replaces an O(n) one. That
is the whole reason they exist.

---

## 2. Mental model

The design rule behind every structure here: **pair a structure that FINDS
with a structure that ORDERS.**

```
                FIND (where is X?)          ORDER (who is next / oldest / due?)
LRU cache       hash map            +       doubly linked list (recency)
LFU cache       hash map            +       list of frequency buckets
Ring buffer     index arithmetic    +       circular array (FIFO)
Object pool     (the pointer itself)+       free list threaded through free slots
Sparse matrix   row_ptr offsets     +       column-sorted arrays (binary search)
Token bucket    one counter         +       lazy refill from elapsed time
Consistent hash binary search       +       sorted ring of hash points
Timer wheel     slot = expiry % S   +       per-slot lists (buckets by time)
```

Analogy: **a hotel.** The front desk has a register (hash map) that says
which room each guest is in. The cleaning schedule (linked list) says which
room was used longest ago. Neither one alone can answer "evict the guest who
has been idle longest, in O(1)". Together they can.

---

## 3. How it works

### LRU cache

*Least Recently Used*: when full, evict the entry that was touched longest
ago.

```
 hash map: key -> node*            doubly linked list, head = most recent
 +-----+                           head                               tail
 |  7  |------------------------->  [7] <-> [3] <-> [9] <-> [1]
 |  3  |---------------------------------^              ^      ^
 |  9  |-------------------------------------------------+      |
 |  1  |--------------------------------------------------------+
 +-----+

 get(9):  map finds node 9            O(1)
          unlink it, push at head     O(1)   -> [9] [7] [3] [1]
 put(5) when full:
          victim = tail (1)           O(1)
          erase 1 from map, free node
          new node 5 at head          -> [5] [9] [7] [3]
```

Step by step, capacity 3:

```
op         list (MRU..LRU)   note
put 1      1
put 2      2 1
put 3      3 2 1
get 1      1 3 2             1 touched -> moves to front
put 4      4 1 3             full: evict tail = 2
get 2      miss
put 5      5 4 1             evict 3
```

Why a **doubly** linked list? To unlink a node from the middle in O(1) you
need its predecessor. A singly linked list would need an O(n) walk to find it.
Why **sentinel** head/tail nodes? Then insert and unlink never need a NULL
check (see `c/01_lru_cache.c`).

In C++, `std::list::splice(begin, list, it)` moves a node to the front without
invalidating any iterator. That is why `unordered_map<K, list::iterator>` is
safe (`cpp/01_lru_cache.cpp`).

### LFU cache

*Least Frequently Used*: evict the entry with the smallest use count. Ties go
to the least recently used. LFU keeps hot keys alive even through a one-off
scan of cold keys, which an LRU cannot do.

The O(1) trick: a touched item moves from frequency `f` to `f+1`, never
further. So keep **frequency buckets**:

```
 C version: list of buckets, sorted ascending
   [f=1] -> 7, 2          evict from the back of the first bucket (2)
   [f=2] -> 4
   [f=5] -> 9, 1

 touch(4): bucket f=3 does not exist yet
   [f=1] -> 7, 2
   [f=3] -> 4             new bucket inserted right after f=2; f=2 is now empty and freed
   [f=5] -> 9, 1

 C++ version: unordered_map<freq, list<key>> + min_freq
   new key        -> min_freq = 1
   touch empties the min_freq bucket -> min_freq += 1   (only ever +1!)
```

### Ring buffer (bounded queue)

```
 cap 8, head = next read, tail = next write, count = 5
   0   1   2   3   4   5   6   7
 +---+---+---+---+---+---+---+---+
 | f | g |   |   |   | c | d | e |
 +---+---+---+---+---+---+---+---+
           ^tail       ^head
 push: buf[tail]=x; tail=(tail+1)%cap; count++
 pop : x=buf[head]; head=(head+1)%cap; count--
```

- Keep a `count`, or leave one slot empty. Otherwise `head == tail` could mean
  either empty or full.
- With a power-of-two size, `% cap` becomes `& (cap-1)`. The kernel's `kfifo`
  goes further: its indices run free and are masked on access, so `tail - head`
  is always the fill level, even after the counters overflow.

**Blocking version (mutex + 2 condition variables):**

```
producer                          consumer
lock                              lock
while full: wait(not_full)        while empty && !closed: wait(not_empty)
write, tail++                     read, head++
signal(not_empty)                 signal(not_full)
unlock                            unlock
```

Use `while`, not `if`: wake-ups can be spurious, and another thread may take
the slot first. `close()` must `broadcast` so that *every* waiting consumer
wakes up and exits.

**Lock-free SPSC version:** with exactly one producer and one consumer, each
index has a single writer. A release-store of `tail` publishes the data that
was written before it, and the acquire-load on the other side sees that data.
No lock is needed (`c/03`, `cpp/03`).

### Object pool / slab allocator

```
 slab = one malloc, cut into equal slots
 +------+------+------+------+------+------+
 | USED | free | USED | free | free | USED |
 +------+--|---+------+--|---+--|---+------+
 free_list-+      +------+   +--+
           +----->+          +---> NULL
 alloc = pop free_list   O(1)
 free  = push free_list  O(1)   (LIFO: the slot just freed is still in cache)
```

The "next" pointer is stored **inside the free slot itself**, so the free list
costs no extra memory. The slot must therefore be at least `sizeof(void*)`
and suitably aligned. In C++ the slot is a `union { Slot* next; storage }`,
objects are built with placement `new`, and `unique_ptr` with a custom deleter
hands the slot back to the pool (`cpp/04`).

### Sparse matrix (COO, DOK, CSR)

```
      dense                 CSR
 [ 5 0 0 0 0 ]   values  = [5 8 3 6 2 7]
 [ 0 8 0 3 0 ]   col_idx = [0 1 3 1 2 4]
 [ 0 0 0 0 0 ]   row_ptr = [0 1 3 3 6]     row r = [row_ptr[r], row_ptr[r+1])
 [ 0 6 2 0 7 ]
```

- **COO** (triplets) and **DOK** (`map<(r,c), v>`) are easy to build and change.
- **CSR** is compact and fast for arithmetic. To convert COO to CSR,
  *counting-sort the triplets by row*: count per row, prefix-sum, scatter.
  That is O(nnz + rows).
- `y = A x` costs O(nnz), not O(rows x cols).
- Transposing (CSR to CSC) is the same counting sort, by column.
- Sparse x sparse multiply (Gustavson's algorithm, `cpp/05`) works row by row.
  It uses a dense accumulator row plus a list of the columns it touched, so
  resetting the accumulator stays cheap.

### Rate limiter: token bucket

```
 tokens
 B |####        ###          refill r tokens/s, up to B
   |    ##     #   #
   |      #####     ######   empty -> reject
 0 +------------------------> t
```

On every request: `tokens = min(B, tokens + elapsed*r)`, then take one token
or reject. There is no background timer; the refill is computed *lazily*.

- Long-run rate is at most r. In any window of length W, at most `B + r*W`
  requests get through.
- Integer trick (`c/06`): store credit in **nanoseconds**, with one token
  equal to T = 1/r ns. That gives exact arithmetic with no float drift.
- **GCRA** stores a single timestamp (the theoretical arrival time) and makes
  the *same* decisions as the bucket. `c/06` checks this on random traffic.
- A **fixed-window counter** is simpler but lets 2x the limit through at a
  window boundary. A **sliding-window log** is exact but stores every
  timestamp (`cpp/06`).
- **Leaky bucket** is the same idea used as a *queue* drained at a fixed rate.
  It smooths output instead of rejecting.

### Consistent hashing

```
 server(key) = hash(key) % N        -> N changes => ~all keys move  (83% in c/07)

 ring of 2^32 positions; servers placed at hash("A#0"), hash("A#1"), ...
            A1
        C2      B1     key k: first server point clockwise from hash(k)
       A2        k1--> B1
        B2      C1
            A3
 add server D: only keys in the arcs D takes over move (~1/(N+1); 16.3% in c/07)
 lookup: lower_bound on the sorted points, wrap to index 0 past the end -> O(log(N*V))
```

**Virtual nodes** (V points per server) even out the load. In `c/07` the
relative spread of load falls from about 110 % with V=1 to about 4 % with
V=500. Weighting a server is just giving it more vnodes. **Replication**
(`cpp/07`): keep walking clockwise until you have N *distinct* physical
servers. When the primary fails, its keys land on the next replica, which
already holds the data.

### Timer wheel

A heap gives O(log n) add and cancel. Most timeouts (TCP retransmit,
keep-alive, request deadlines) are **cancelled** before they fire, so O(1)
add and cancel matter more than fast "find the minimum".

```
 hashed wheel (cpp/08): S slots, one tick per slot
   add(d): slot = (now+d) % S, rounds = d / S          O(1)
   tick : visit slot now%S: rounds==0 -> fire, else rounds--

 hierarchical wheel (c/08, classic Linux kernel/timer.c before 4.8):
   level 0: 64 slots x 1 tick       (exact)
   level 1: 64 slots x 64 ticks
   level 2: 64 slots x 4096 ticks ...
   When level 0 wraps to slot 0, one level-1 slot is "cascaded":
   its timers are re-inserted and fall into exact level-0 slots.

   tick:   0 ... 63 | 64 ... 127 | 128 ...
            level 0  | cascade L1 slot 1 -> into level 0
```

Linux 4.8+ removed cascading. Timers far away now land in coarse slots and
fire *slightly late*, with a bounded error of about 12 %. Timeouts almost
never fire, so cascading them was wasted work. Precise timers use
`hrtimer` (a red-black tree) instead.

---

## 4. Complexity table

| Structure | Operation | Time | Space | Why |
|---|---|---|---|---|
| LRU | get / put | O(1) avg | O(cap) | hash lookup + O(1) list unlink/push |
| LFU | get / put | O(1) avg | O(cap) | item moves only f -> f+1; min bucket known |
| Ring buffer | push / pop | O(1) | O(cap) fixed | index arithmetic; no allocation after init |
| Object pool | alloc / free | O(1) (amortised with growth) | O(peak) | pop/push of an intrusive free list |
| Sparse CSR | build from COO | O(nnz + rows) | O(nnz + rows) | counting sort by row |
| | y = Ax | O(nnz) | | only stored entries are touched |
| | A(r,c) | O(log nnz_row) | | binary search in a sorted row |
| | transpose | O(nnz + cols) | | counting sort by column |
| Token bucket | allow | O(1) | O(1) per key | lazy refill from elapsed time |
| Sliding log | allow | O(1) amortised | O(limit) per key | deque of timestamps |
| Consistent hash | lookup | O(log(N V)) | O(N V) | binary search on sorted points |
| | add/remove server | O(V log(N V)) | | insert V points |
| Hashed wheel | add / cancel | O(1) | O(S + n) | slot = expiry % S; lazy cancel flag |
| | tick | O(entries in slot) | | rounds counters are decremented |
| Hierarchical wheel | add / cancel | O(1) | O(L*S + n) | level from distance; hlist unlink |
| | tick | O(1) + fired + rare cascade | | each timer cascades at most L-1 times |
| Heap (reference) | add / cancel / pop | O(log n) | O(n) | what the wheel beats |

---

## 5. C vs C++

| Structure | C (this lesson) | C++ (this lesson) | Standard / library equivalent |
|---|---|---|---|
| LRU | chained hash table + intrusive doubly list with sentinels | `std::list` + `unordered_map<K, list::iterator>` + `splice` | none in the STL. Linux: `list_lru`; Redis: approximated LRU |
| LFU | list of frequency buckets (Shah et al.) | `unordered_map<freq, list>` + `min_freq` | Caffeine (Java) W-TinyLFU |
| Ring buffer | array + `pthread_mutex_t` + 2 `pthread_cond_t`; C11 `_Atomic` SPSC | template `BoundedQueue<T>` with `std::condition_variable`; `std::atomic` SPSC with `alignas(64)` | `boost::lockfree::spsc_queue`; kernel `kfifo`, `ptr_ring` |
| Object pool | slab + intrusive free list of raw slots | `union Slot`, placement `new`, `unique_ptr<T, Releaser>` | `std::pmr::unsynchronized_pool_resource`; kernel `kmem_cache_*` |
| Sparse | COO -> CSR via counting sort, binary-search get | `std::map` DOK -> CSR, SpMV, SpGEMM | Eigen `SparseMatrix`, SciPy `csr_matrix`, cuSPARSE |
| Rate limiter | integer ns token bucket, GCRA, fixed window | template on `Clock`, `std::chrono`, per-client map, sliding log | nginx `limit_req`, Linux `tc tbf`, Guava `RateLimiter` |
| Consistent hash | sorted array + `qsort` + manual lower_bound | `std::map::lower_bound`, weights, replicas | Ketama (memcached), Cassandra/Dynamo token rings |
| Timer wheel | 4-level hierarchical, hlist, function pointers | hashed wheel with rounds, `std::function`, lazy cancel | kernel `timer_list`, Netty `HashedWheelTimer`, libuv uses a heap |

C makes you see the pointers (intrusive lists, the pointer-to-pointer
`pprev`). C++ hides them behind iterators and RAII, so ownership mistakes
(forgetting to free, returning a slot twice) become hard to make.

---

## 6. Where it is used in the real world

- **LRU:** CPU caches and TLBs (pseudo-LRU in hardware), the Linux page
  cache (active/inactive lists, a two-list LRU approximation), the
  dentry/inode caches, browser caches, the DNS resolver cache, the
  `memcached` eviction policy, database buffer pools (PostgreSQL uses clock
  sweep, an LRU approximation).
- **LFU:** CDN edge caches, Redis `allkeys-lfu` (with a logarithmic
  probabilistic counter), Caffeine's TinyLFU admission filter.
- **Ring buffer:** NIC RX/TX descriptor rings (DMA), audio (ALSA period
  buffers), the kernel `printk` log buffer, `perf`/ftrace per-CPU buffers,
  `io_uring` submission/completion queues, the GStreamer `queue` element
  (`max-size-buffers` is its capacity, and EOS plays the role of `close()`),
  UART drivers on your BeagleBone, TI IPC between the TDA4VM's A72 and R5F
  cores (shared-memory rings via RPMsg/virtio vrings).
- **Object pool / slab:** Linux SLAB/SLUB `kmem_cache` (`skbuff_head_cache`,
  `inode_cache`, see `/proc/slabinfo`), GStreamer `GstBufferPool` (V4L2 and
  DMA-BUF buffers are recycled, not reallocated), DPDK mempools, game entity
  pools, RTOS fixed-block pools (FreeRTOS, Zephyr `k_mem_slab`).
- **Sparse matrix:** finite-element and circuit simulation (SPICE), graph
  algorithms written as matrix products (PageRank, GraphBLAS),
  recommendation systems, sparse neural networks, ML feature vectors
  (bag-of-words).
- **Token bucket:** Linux traffic control (`tc qdisc tbf`), cgroup I/O
  throttling, API gateways (one bucket per API key), AWS/GCP API quotas,
  `printk_ratelimit()` and `net_ratelimit()`, Bluetooth/Wi-Fi QoS, and network
  policing with `iptables -m limit` (a token bucket).
- **Consistent hashing:** memcached clients (Ketama), Amazon Dynamo,
  Cassandra, Riak, CDN request routing, load balancers (Maglev, a related
  table-based scheme), sharded caches.
- **Timer wheel:** Linux `timer_list` (jiffies, TCP retransmit and keepalive
  timers), Netty, Kafka's purgatory, Linux `hrtimer` (a red-black tree, for
  contrast), the timeouts of every event loop.

---

## 7. Common mistakes and pitfalls

1. **LRU: erasing from the list before the map.** In C++, if you `pop_back()`
   first, `items_.back().first` is gone and you erase the wrong key or read
   freed memory. Read the key, erase the map entry, *then* pop.
2. **LRU: forgetting to touch on `put` of an existing key.** An update is a
   use.
3. **LFU: updating `min_freq` wrongly.** It resets to 1 only on insert of a
   *new* key. On a touch it grows by 1 only if the old bucket was the minimum
   *and* became empty.
4. **Ring buffer: `if` instead of `while` around `cond_wait`.** Spurious
   wake-ups and stolen items corrupt the count.
5. **Ring buffer: `signal` instead of `broadcast` on close.** All but one
   consumer hang forever.
6. **Ring buffer: confusing full with empty** when you only have head and
   tail. Keep a count, or waste one slot.
7. **SPSC with plain variables or `volatile`.** You need acquire/release
   ordering, otherwise the consumer may see the new index before the data.
   `volatile` is not a memory barrier.
8. **False sharing:** producer and consumer indices on the same cache line
   make the CPUs fight over that line. Use `alignas(64)`.
9. **Pool: slot smaller than a pointer, or misaligned.** The free-list link
   lives in the slot. Round the size up to `max_align_t`.
10. **Pool: double free** puts a slot on the free list twice, and two objects
    will later share it. The kernel's SLUB debug (`slub_debug=FZP`) and
    freelist hardening exist to catch this.
11. **Pool + ASan:** a pool hides use-after-free from AddressSanitizer,
    because the memory is never really freed. Real pools call
    `ASAN_POISON_MEMORY_REGION` on free.
12. **Sparse: unsorted or duplicate column indices** break binary search and
    many libraries. Sort each row and merge duplicates when you build.
13. **Token bucket: using the wall clock.** NTP can step it backwards, which
    gives negative elapsed time and an unsigned underflow. Use
    `CLOCK_MONOTONIC` or `steady_clock`.
14. **Token bucket: refill and take done non-atomically** across threads. Two
    threads spend the same token. Lock both together (`cpp/06` proves exactly
    `burst` is allowed with 8 threads).
15. **Consistent hashing with a weak hash** (raw FNV on "node#1", "node#2"):
    the points cluster and the load is very uneven. Mix the hash bits.
16. **Timer wheel: a callback re-arms into the slot being processed** and
    fires in the same pass. Detach the slot before running callbacks
    (`cpp/08`), and save `next` before calling the callback (`c/08`).
17. **Timer wheel: resetting state inside `add()`** that a re-arming callback
    relies on. This was a real bug while writing `c/08`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_lru_cache.c](c/01_lru_cache.c) | chained hash + sentinel doubly linked list, randomised check vs reference | `make run P=dsa/19-system-design-structures/c/01_lru_cache.c` |
| [c/02_lfu_cache.c](c/02_lfu_cache.c) | O(1) LFU with a linked list of frequency buckets | `make run P=dsa/19-system-design-structures/c/02_lfu_cache.c` |
| [c/03_ring_buffer_producer_consumer.c](c/03_ring_buffer_producer_consumer.c) | MPMC ring with mutex + 2 condvars + close; lock-free SPSC with C11 atomics | `make run P=dsa/19-system-design-structures/c/03_ring_buffer_producer_consumer.c` |
| [c/04_object_pool.c](c/04_object_pool.c) | slab allocator with an intrusive free list, LIFO reuse, arena teardown | `make run P=dsa/19-system-design-structures/c/04_object_pool.c` |
| [c/05_sparse_matrix.c](c/05_sparse_matrix.c) | COO -> CSR via counting sort, SpMV, get, transpose, memory comparison | `make run P=dsa/19-system-design-structures/c/05_sparse_matrix.c` |
| [c/06_token_bucket.c](c/06_token_bucket.c) | integer-ns token bucket, GCRA equivalence, fixed-window boundary flaw | `make run P=dsa/19-system-design-structures/c/06_token_bucket.c` |
| [c/07_consistent_hashing.c](c/07_consistent_hashing.c) | ring with vnodes, keys moved vs `% N`, load balance vs vnode count | `make run P=dsa/19-system-design-structures/c/07_consistent_hashing.c` |
| [c/08_timer_wheel.c](c/08_timer_wheel.c) | 4-level hierarchical wheel with cascading (classic Linux), exact-tick check | `make run P=dsa/19-system-design-structures/c/08_timer_wheel.c` |
| [cpp/01_lru_cache.cpp](cpp/01_lru_cache.cpp) | `LruCache<K,V>` with `std::list::splice` | `make run P=dsa/19-system-design-structures/cpp/01_lru_cache.cpp` |
| [cpp/02_lfu_cache.cpp](cpp/02_lfu_cache.cpp) | `LfuCache<K,V>` with freq map + `min_freq`; scan resistance | `make run P=dsa/19-system-design-structures/cpp/02_lfu_cache.cpp` |
| [cpp/03_ring_buffer.cpp](cpp/03_ring_buffer.cpp) | `BoundedQueue<T>` moving `unique_ptr<Frame>`; `SpscRing<T,N>` | `make run P=dsa/19-system-design-structures/cpp/03_ring_buffer.cpp` |
| [cpp/04_object_pool.cpp](cpp/04_object_pool.cpp) | `ObjectPool<T>`: placement new, RAII handles that return to the pool | `make run P=dsa/19-system-design-structures/cpp/04_object_pool.cpp` |
| [cpp/05_sparse_matrix.cpp](cpp/05_sparse_matrix.cpp) | DOK -> CSR, SpMV, Gustavson SpGEMM (2-hop path counting) | `make run P=dsa/19-system-design-structures/cpp/05_sparse_matrix.cpp` |
| [cpp/06_rate_limiter.cpp](cpp/06_rate_limiter.cpp) | `TokenBucket<Clock>`, per-client limiter, thread safety, sliding log | `make run P=dsa/19-system-design-structures/cpp/06_rate_limiter.cpp` |
| [cpp/07_consistent_hashing.cpp](cpp/07_consistent_hashing.cpp) | `std::map` ring, weights, 3-way replica placement, failover | `make run P=dsa/19-system-design-structures/cpp/07_consistent_hashing.cpp` |
| [cpp/08_hashed_timer_wheel.cpp](cpp/08_hashed_timer_wheel.cpp) | hashed wheel with rounds, lazy cancel, TCP-style backoff, vs min-heap | `make run P=dsa/19-system-design-structures/cpp/08_hashed_timer_wheel.cpp` |

The thread programs (`c/03`, `cpp/03`) were also run under ThreadSanitizer.
To repeat that by hand (TSan cannot be combined with ASan):
`g++ -std=c++17 -O1 -g -fsanitize=thread dsa/19-system-design-structures/cpp/03_ring_buffer.cpp -lpthread && setarch -R ./a.out`
(`setarch -R` works around TSan's "unexpected memory mapping" error on newer
kernels).

---

## 9. Exercises

1. **(easy)** Add `lru_peek()` to `c/01`: it reads a value *without* touching
   the recency order. Where would you want this? (Hint: monitoring or stats
   code should not change eviction.)
2. **(easy)** Make the ring buffer in `c/03` power-of-two sized and replace
   `% cap` with `& (cap - 1)`. Assert the size at init.
3. **(easy)** Add `pool_stats()` to `c/04` that walks the free list and checks
   `free + in_use == nslabs * per_slab`.
4. **(medium)** Turn `cpp/01` into a **TTL + LRU** cache: each entry also
   expires after T seconds. (Hint: inject a clock as in `cpp/06`.)
5. **(medium)** Implement the **CLOCK** (second-chance) algorithm: a
   circular array with a reference bit per entry. Compare its hit rate with
   LRU on a Zipf-like key stream.
6. **(medium)** Add `try_push_for(timeout)` to `BoundedQueue` using
   `cv.wait_for`. That is what GStreamer's `queue` does when it is "leaky".
7. **(medium)** Give the rate limiter in `cpp/06` a `cost` parameter, then
   rate-limit *bytes* instead of requests (`tc tbf` does this).
8. **(medium)** Add a **sliding-window counter** (two fixed windows,
   weighted). Measure its worst-case error against the sliding log.
9. **(hard)** Add `ASAN_POISON_MEMORY_REGION` / `ASAN_UNPOISON_MEMORY_REGION`
   (from `<sanitizer/asan_interface.h>`) to `c/04`. Write a `_bug.c` that uses
   an object after `pool_free` and watch ASan catch it.
10. **(hard)** Replace the hierarchical wheel's cascading with the **Linux
    4.8+ design**: no cascading; a timer goes into a coarse slot and fires up
    to one slot-width late. Assert that the lateness bound holds.
11. **(hard)** Implement **jump consistent hash** (Lamping & Veach). It uses
    no memory, O(log N) time, and buckets must be numbered 0..N-1. Compare how
    many keys move with `c/07`.
12. **(hard)** Make the C LRU thread-safe in two ways: (a) one global mutex;
    (b) 16 shards, each its own LRU with its own lock (memcached and the
    kernel's `list_lru` are per-node). Benchmark with `make OPT=1 SAN=0`.

---

## 10. Remember forever

```
LRU   = hash map (find) + doubly linked list (recency). Touch = move to head.
        Evict = tail. Erase the map entry BEFORE freeing the node.
LFU   = map + freq buckets; an item only moves f -> f+1. min_freq = 1 on insert.
RING  = array + head/tail (+count). %cap or &(cap-1). while(cond) wait. Broadcast on close.
        SPSC lock-free: write data, THEN release-store index.
POOL  = free list threaded through the free slots. alloc = pop, free = push. O(1), LIFO hot.
CSR   = values, col_idx, row_ptr[R+1]. Build with a counting sort by row. Ax = O(nnz).
TOKEN = tokens = min(B, tokens + dt*r); take 1 or reject. Lazy. Monotonic clock.
CHASH = sorted ring of server points (V per server); owner = lower_bound(hash) w/ wrap.
        Adding 1 of N+1 servers moves ~1/(N+1) of the keys.
WHEEL = slot = expiry % S (+ rounds, or levels that cascade). O(1) add/cancel.
```

Review this card using the schedule in
[LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why does an LRU need a *doubly* linked list and not a singly linked one?
2. In the C++ LRU, why can the map store `list::iterator`s safely, and which
   operation would break that if you used `std::vector` instead?
3. Give an access pattern where LRU does badly and LFU does well. Then give
   one where LFU does badly. (Hint: a once-popular key that is now dead.)
4. Why does the LFU never need to search for the minimum frequency?
5. Why must `pthread_cond_wait` sit inside a `while` loop? What two things
   can go wrong with `if`?
6. In a lock-free SPSC ring, what goes wrong if the producer stores `tail`
   *before* writing the data slot?
7. Why does a pool reuse the *most recently* freed slot first?
8. How does a free list cost zero extra memory?
9. Build the CSR arrays for `[[0,2,0],[1,0,3],[0,0,0]]` in your head.
10. How many requests can a token bucket (B=5, r=10/s) let through in any
    500 ms window? How does a fixed 1 s window with limit 10 compare?
11. With `hash % N`, what fraction of keys moves when N goes from 5 to 6?
    With consistent hashing?
12. What problem do virtual nodes solve, and what do they cost?
13. Why do timer wheels beat heaps for network timeouts specifically?
14. In a hierarchical wheel, when exactly does a level-1 timer move to level 0?
    Why did Linux 4.8 stop doing that?
15. Your BeagleBone's UART driver gets bytes in an IRQ and a thread reads
    them. Which structure from this lesson goes between them, and which
    variant (locked or lock-free) fits, and why?
