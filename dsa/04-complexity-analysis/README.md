# 04 — Complexity analysis: Big-O you can feel, and the memory hierarchy Big-O ignores

> Before choosing a data structure you must be able to say, in one breath,
> "this is O(n log n) time and O(n) space, and here is why" — and then know
> when the constant factors (caches!) overrule the Big-O.

---

## 1. Why this exists

Two programs can both be "correct" and differ by a factor of a million in
run time. Complexity analysis predicts **how cost grows as the input grows**,
without running anything, so you can:

* reject an approach before writing it ("O(n²) over 10⁶ detections per second will never fit in 33 ms");
* compare algorithms independent of machine, compiler and language;
* read library docs (`std::map::find` is O(log n), `std::unordered_map::find`
  O(1) average, O(n) worst) and know what it means for you.

Then the second half: Big-O hides constants, and on modern CPUs the biggest
constant is **memory**. A cache miss costs ~100 instructions. That is why an
O(n) walk over a linked list can be 50× slower than an O(n) walk over an array.

---

## 2. Mental model

**Analogy — counting steps on a staircase, not timing the climber.** Time
depends on the climber (CPU), the shoes (compiler), the weather (other
processes). The *number of steps* depends only on the staircase (algorithm)
and its height (n). Big-O describes how the number of steps grows when the
building gets taller: double the floors — does the climb take the same
(O(1)), one more step (O(log n)), twice (O(n)), or four times (O(n²))?

```
 steps
   ▲                                              O(2^n)  O(n^2)
   │                                               │      /
   │                                               │     /
   │                                               │    /      O(n log n)
   │                                              │   /      __/
   │                                             │  /    __/
   │                                            │ /  __/        O(n)
   │                                           │/_/     ______/
   │                                         _/│ ______/
   │                                    ____/ _/                 O(log n)
   │                          _________/ ____/      _______________________
   │________________________/________/____________/________________________ O(1)
   └──────────────────────────────────────────────────────────────────────────► n
```

**The doubling fingerprint** — what happens to the cost when n doubles:

| Class | Name | n → 2n | Example |
|---|---|---|---|
| O(1) | constant | same | array index, hash lookup (avg), push on a stack |
| O(log n) | logarithmic | +1 step | binary search, balanced-tree lookup, heap push |
| O(n) | linear | ×2 | scan, sum, linear search |
| O(n log n) | linearithmic | ×2 and a bit | good sorting (merge, heap, intro), FFT |
| O(n²) | quadratic | ×4 | all pairs, insertion/bubble sort, naive dedup |
| O(n³) | cubic | ×8 | naive matrix multiply, Floyd–Warshall |
| O(2ⁿ) | exponential | n+1 doubles it | all subsets, naive recursion (fib) |
| O(n!) | factorial | — | all permutations (brute-force TSP) |

---

## 3. How it works

### 3.1 Big-O, Big-Θ, Big-Ω — the intuition

Let f(n) be the exact number of steps.

* **f = O(g)** — *upper bound*: for large n, f(n) ≤ c·g(n). "At most this fast-growing."
* **f = Ω(g)** — *lower bound*: f(n) ≥ c·g(n). "At least this fast-growing."
* **f = Θ(g)** — *tight*: both. "Grows exactly like g, up to a constant."

```
  c2·g(n) ─────────────────────/       f is Θ(g) if, after some n0,
  f(n)    ──────────────────/~~        it stays sandwiched between
  c1·g(n) ───────────────/             c1·g(n) and c2·g(n).
                     n0 ↑
```

In practice people say "O(n log n)" when they mean Θ. Rules for simplifying:

1. **Drop constants**: 3n + 7 → O(n). (`5n` and `n` are the same class.)
2. **Keep only the dominant term**: n² + 1000n + 10⁶ → O(n²).
3. **Sequence adds, nesting multiplies**: loop A then loop B → O(a + b);
   loop A inside loop B → O(a·b).
4. **Different inputs keep different letters**: for each of n frames, for each
   of m detections → O(n·m), *not* O(n²).
5. **log base does not matter**: log₂ n = log₁₀ n × 3.32 → just "log n".

### 3.2 Counting operations

Choose a **basic operation** (a comparison, an assignment, a memory access) and
count how many times it runs as a function of n:

```c
for (i = 0; i < n; i++)            // runs n times
    for (j = i + 1; j < n; j++)    // runs n-1, n-2, ..., 1, 0 times
        cmp(a[i], a[j]);           // total = n(n-1)/2  →  Θ(n²)

for (i = n; i > 1; i /= 2)         // n, n/2, n/4, ... 1: log₂ n times → Θ(log n)

for (i = 1; i <= n; i *= 2)        // i = 1, 2, 4, ..., n
    for (j = 0; j < i; j++)        // 1 + 2 + 4 + ... + n = 2n - 1  → Θ(n)   (not n log n!)
```

`c/01_counting_operations.c` counts each shape exactly and prints the table:

```
     n     O(1)  O(logn)       O(n)     O(nlogn)       O(n^2)         O(n^3)
    16        3        4         16           64          120           4096
   256        3        8        256         2048        32640       16777216
   512        3        9        512         4608       130816      134217728
ratio f(2n)/f(n) at n = 256:  O(n): 2.00  O(nlogn): 2.25  O(n^2): 4.01  O(n^3): 8.00
```

Two traps it also demonstrates: **nested loops are not automatically O(n²)**
(if the inner index never resets — the two-pointer technique — the total is
O(n)); and the geometric inner loop above is O(n).

### 3.3 Common classes with concrete timings

At ~10⁹ simple operations per second (`c/02_growth_table.c`):

```
       n |    log2 n |         n |  n log2 n |         n^2 |          n^3 |              2^n
   1e+01 |      3 ns |     10 ns |     33 ns |      100 ns |       1.0 us |           1.0 us
   1e+03 |     10 ns |    1.0 us |   10.0 us |      1.0 ms |        1.0 s |   > universe age
   1e+05 |     17 ns |  100.0 us |    1.7 ms |      10.0 s |      12 days |   > universe age
   1e+06 |     20 ns |    1.0 ms |   19.9 ms |      17 min |     32 years |   > universe age
   1e+09 |     30 ns |     1.0 s |    29.9 s |    32 years | > universe age |   > universe age
```

Memorise the shape: **n = 10⁶ is fine for O(n log n), hopeless for O(n²).**
For a 30 fps camera pipeline you have 33 ms per frame; a 1920×1080 frame is
2·10⁶ pixels → you can afford a few O(n) passes per frame, never O(n²).

| Rough limit for 1 second | max n |
|---|---|
| O(n!) | ~11 |
| O(2ⁿ) | ~25–30 |
| O(n³) | ~1 000 |
| O(n²) | ~30 000 |
| O(n log n) | ~5·10⁷ |
| O(n) | ~10⁸–10⁹ |

**Measuring** (`c/02_growth_table.c`, `cpp/02_benchmark_harness.cpp`): run
with n, 2n, 4n … and look at the ratio T(2n)/T(n) ≈ 2^k → the exponent k.
Real output: `O(n^2) double loop ... => k ~ 2.03`, `std::sort => k ~ 1.08`.
Honest benchmarking: build with `make OPT=1 SAN=0`, use a monotonic clock
(`CLOCK_MONOTONIC`, `std::chrono::steady_clock`), warm up, take the best (or
median) of several runs, write results to a `volatile` sink so the optimiser
cannot delete the work, and pin the CPU frequency if you can. On the BeagleBone
use `perf stat` to count instructions and cache misses instead of trusting time alone.

### 3.4 Amortised analysis — why `push_back` is O(1)

A dynamic array (C `realloc` vector, `std::vector`) that is full must allocate
a bigger block and copy everything: **one** push costs O(n). But if capacity
**doubles**, copies happen at sizes 1, 2, 4, …, n/2 → total < 2n copies for n
pushes → **O(1) amortised** per push.

```
 push #:   1  2  3  4  5  6  7  8  9 ...16 17 ...
 copies:   0  1  2  -  4  -  -  -  8  ... - 16       (copy when full: 1+2+4+8+16 < 2n)
 capacity: 1  2  4  4  8  8  8  8  16 ...16 32
```

**Banker's view**: charge every push 3 coins — 1 for its own write, 2 saved.
When capacity c is full, the c/2 pushes since the last growth saved c coins,
exactly enough to copy all c elements. The bank never goes negative.

Real output of `c/03_amortised_dynamic_array.c`:

```
grow          n  reallocs       copies    copies/push   worst push       time
x2        64000        17        65535           1.02        32768     0.53 ms
x1.5      64000        29       184351           2.88        61447     0.72 ms
+16       64000      4000    127968000        1999.50        63984   240.02 ms
```

Growing by a **constant** (+16) makes the total O(n²): 450× slower here.
Note the "worst push" column: **amortised O(1) is not worst-case O(1)**. In a
hard real-time loop (audio callback, motor control ISR) one O(n) reallocation
is a missed deadline — `reserve()` up front or use a fixed-capacity buffer.
(libstdc++ grows ×2, MSVC ×1.5; ×1.5 lets freed blocks be reused by later growth.)

Other amortised O(1) classics: hash table rehashing, a queue built from two
stacks, union-find with path compression (nearly O(1)), splay trees (O(log n)).

### 3.5 Space complexity

Count the **extra** memory as a function of n (the input itself usually does not count):

| Algorithm | Extra space | Where it goes |
|---|---|---|
| sum of an array | O(1) | a few locals |
| reverse in place | O(1) | two indices |
| merge sort | O(n) | temporary buffer |
| quicksort | O(log n) average, O(n) worst | **recursion stack** |
| recursive binary search | O(log n) | stack frames (iterative: O(1)) |
| naive recursive `fib(n)` | O(n) | max recursion depth, not the number of calls |
| memoised fib | O(n) | memo table + stack |
| BFS on a graph | O(V) | queue + visited set |
| hash set of n items | O(n) | buckets + nodes (often 2–4× the raw data) |

**Recursion depth is space.** The main thread has 8 MiB of stack, a pthread
the same by default, a kernel thread 16 KiB. O(n)-deep recursion on n = 10⁶
crashes (see `03-debugging/c/04_stack_overflow_bug.c`); O(log n) never does.

### 3.6 Recursion and the Master theorem

A recursive algorithm's cost is a **recurrence**. Solve it by drawing the
recursion tree: work per level × number of levels.

```
 merge sort T(n) = 2T(n/2) + n
 level 0:              [n]                       n
 level 1:        [n/2]       [n/2]               n
 level 2:     [n/4] [n/4] [n/4] [n/4]            n
   ...               log2 n levels              ...
 total = n · log2 n  →  Θ(n log n);   depth (stack) = log2 n
```

**Master theorem** for T(n) = a·T(n/b) + O(n^d) (a subproblems of size n/b,
plus n^d work to split/combine). Compare d with log_b a:

| Case | Condition | Result | Intuition | Example |
|---|---|---|---|---|
| 1 | d < log_b a | Θ(n^(log_b a)) | leaves dominate | T = 2T(n/2) + 1 → Θ(n) (tree sum); Karatsuba 3T(n/2)+n → Θ(n^1.58) |
| 2 | d = log_b a | Θ(n^d · log n) | every level equal | binary search T(n/2)+1 → Θ(log n); merge sort → Θ(n log n) |
| 3 | d > log_b a | Θ(n^d) | root dominates | T = 2T(n/2) + n² → Θ(n²) |

Not covered by the Master theorem: **T(n) = T(n−1) + T(n−2) + 1** (naive
Fibonacci). Each call spawns two; the call count is 2·F(n+1) − 1 ≈ 1.618ⁿ —
exponential. Real output of `c/04_recursion_master_theorem.c`:

```
binary search n=65536: 15 calls, max depth 15   (log2 n = 16)  -> O(log n) time AND stack
merge sort    n=65536: 965797 comparisons (n log2 n = 1048576), 131071 calls (2n-1), depth 17
  fib(28) = 317811  calls = 1028457
  calls(28)/calls(27) = 1.6180  (golden ratio 1.6180) -> O(1.618^n)
memoised fib(90) = 2880067194370816120 with 179 calls -> O(n)
```

Memoisation turns exponential into linear by computing each subproblem once —
the core idea of **dynamic programming** (lesson 17). T(n) = T(n−1) + O(1) →
O(n) (linear recursion); T(n) = T(n−1) + O(n) → O(n²) (selection sort, recursive).

### 3.7 Best, average, worst case

The same algorithm can cost very different amounts **for the same n**,
depending on the input's shape. Real output of `c/05_best_avg_worst.c` (n = 4000):

```
linear search n=4000: best 1, average 2000, worst 4000 comparisons
algorithm            sorted     random   reversed
insertion sort         3999    3933471    7998000
quicksort(1st)      7998000      57611    7998000
```

* Insertion sort: best Θ(n) on sorted input, worst Θ(n²) on reversed. Great for
  **nearly sorted** data (that's why `std::sort` uses it for small partitions).
* Quicksort with the first element as pivot: average Θ(n log n) but **Θ(n²) on
  already-sorted input** — a classic denial-of-service hole. Real
  implementations pick median-of-3 or random pivots and switch to heapsort when
  recursion gets too deep (**introsort**, what `std::sort` is).
* Hash tables: O(1) average, O(n) worst when keys collide (adversarial inputs
  → randomised hash seeds).

Which one to quote? **Worst case** for guarantees (real-time, security),
**average** for throughput, **amortised** for sequences of operations.
**Best case** is almost never useful ("bubble sort is O(n) on sorted input").

### 3.8 Cache effects — why O(n) on a list is slower than O(n) on an array

```
              latency      size (typical laptop / Cortex-A72 on TDA4VM)
 register      0 cycles     —
 L1 cache      ~1 ns        32–48 KiB per core
 L2 cache      ~4 ns        512 KiB–2 MiB
 L3 / MSMC     ~10–15 ns    several MiB (the TDA4VM has 8 MiB MSMC SRAM)
 DRAM          ~80–120 ns   GiBs                       ← 100× slower than L1
```

The CPU never fetches one byte: it fetches a **64-byte cache line**, and a
**hardware prefetcher** detects sequential/strided access and fetches the next
lines *before* you ask.

```
 array:   [ v0 v1 v2 v3 v4 v5 v6 v7 ][ v8 ... ]   one miss loads 8 int64s;
           ───────── 64 B line ─────────            prefetcher streams the rest

 list (shuffled nodes):
   [n17]────────────►[n3]──────►[n942]──────────────►[n58]  every hop: address unknown
     ▲ next address known only AFTER this load finishes      until the previous load
                                                             completes → serial misses
```

Real output of `c/06_cache_effects.c` (`make OPT=1 SAN=0`, 2M elements):

```
  array                        0.80 ms  (0.38 ns/element)
  list, nodes in order         1.97 ms  (0.94 ns/element)  x2.5 vs array
  list, nodes shuffled       167.56 ms  (79.90 ns/element)  x209.6 vs array
2048x2048 int32 matrix sum:
  row by row (sequential)      0.77 ms
  column by column (stride 8192 B)   10.29 ms  x13.4
```

All three sums are Θ(n). The shuffled list pays one DRAM latency (~80 ns) per
element: **pointer chasing**. A list whose nodes happen to be contiguous (fresh
allocation) looks fine in a benchmark and degrades in production as the heap
fragments. Column-major traversal of a row-major C array touches one int per
64-byte line (stride 8 KiB) → mostly misses.

`cpp/03_vector_vs_list.cpp` does Stroustrup's experiment: insert n random
numbers into a sorted sequence. Both are O(n²) (linear search for the spot),
the list's O(1) insert does not help, and the vector wins 2–6× (optimised build)
because its search and `memmove` stream contiguous memory.

Practical rules: prefer arrays/`std::vector` by default; store structs
contiguously (array of structs, or struct of arrays for SIMD); iterate in
memory order (row by row); keep hot data small (fewer cache lines); use
indices instead of pointers into a pool; measure with
`perf stat -e cache-misses,cache-references ./prog`.

---

## 4. Complexity table — the cheat sheet you will use in every later lesson

| Structure | Access | Search | Insert | Delete | Space | Note |
|---|---|---|---|---|---|---|
| array / `std::vector` | O(1) | O(n), sorted: O(log n) | end: O(1) amort., middle: O(n) | end O(1), middle O(n) | O(n) | best cache behaviour |
| linked list | O(n) | O(n) | O(1) at a known node | O(1) at a known node | O(n) + 2 ptrs/node | pointer chasing |
| stack / queue (ring buffer) | top O(1) | — | O(1) | O(1) | O(n) | |
| hash table | — | O(1) avg, O(n) worst | O(1) avg (amortised rehash) | O(1) avg | O(n) | |
| balanced BST (`std::map`) | O(log n) | O(log n) | O(log n) | O(log n) | O(n) | ordered iteration |
| binary heap | top O(1) | O(n) | O(log n) | pop O(log n) | O(n) | priority queue |
| trie | — | O(key length) | O(L) | O(L) | large | prefixes |
| sorting (merge/heap/intro) | — | — | — | — | — | O(n log n) time; merge O(n) space |
| binary search | — | O(log n) | — | — | O(1) | needs sorted input |
| BFS / DFS | — | O(V + E) | — | — | O(V) | |

---

## 5. C vs C++

| | C | C++ |
|---|---|---|
| sort | `qsort` — comparator via function pointer, cannot inline: often 2–3× slower | `std::sort` — comparator inlined, introsort, guaranteed O(n log n) |
| search sorted | `bsearch` | `std::lower_bound`, `std::binary_search`, `std::equal_range` |
| dynamic array | hand-written `realloc` doubling | `std::vector` (×2 in libstdc++), `reserve()` |
| complexity guarantees | none in the standard (glibc docs) | **in the standard** for every algorithm/container |
| timing | `clock_gettime(CLOCK_MONOTONIC)` | `std::chrono::steady_clock` |
| counting ops | wrap the operation, count by hand | counting comparator / hasher / allocator (`cpp/01_stl_complexity.cpp`) |

Real output of `cpp/01_stl_complexity.cpp` (n = 131072): `std::sort` 2 693 250
comparisons (n log₂ n = 2 228 224); `std::lower_bound` 17 (= log₂ n);
`std::set` insert ×n 2 327 547; `unordered_map` lookups 1 hash each; `std::map`
lookups ~15.9 comparisons each (≈ log₂ 20000); inserting 20 000 at the front of
a vector shifts 199 990 000 elements (n²/2).

---

## 6. Where it is used in the real world

* **Linux kernel**: the CFS/EEVDF scheduler keeps runnable tasks in a red-black
  tree (O(log n) pick), the old O(1) scheduler's name was its selling point;
  `hlist` hash tables for PIDs, inodes, dentries; radix/XArray for the page
  cache; the kernel avoids O(n) work in interrupt context.
* **Networking**: routing tables as tries (longest-prefix match in O(address
  bits)), connection tracking hash tables with randomised seeds against
  collision attacks.
* **Databases**: B-trees (O(log n) with huge fan-out, designed around disk/cache
  blocks); query planners estimate the cost of each join order.
* **GStreamer / video**: per-frame budgets (33 ms at 30 fps) make O(pixels)
  passes the unit of cost; zero-copy (dmabuf) exists because copying 6 MB per
  frame is an O(n) you can avoid.
* **Embedded/real-time**: worst case and bounded memory matter more than
  average: fixed-size pools, pre-reserved vectors, no amortised reallocation in
  the control loop.
* **Security**: algorithmic-complexity attacks (hash flooding, quicksort
  killers, regex backtracking) turn an average-case assumption into a DoS.

---

## 7. Common mistakes and pitfalls

1. Counting nested loops as O(n²) without checking how many times the inner body *really* runs.
2. Forgetting hidden loops: `strlen` in a loop condition (`for (i = 0; i < strlen(s); i++)` is O(n²)),
   `list.size()` (O(n) for some old implementations), `vector.insert(begin())`, `std::find` inside a loop.
3. Using n for two different inputs (O(n·m) ≠ O(n²)).
4. Ignoring the recursion stack in space complexity.
5. Confusing amortised O(1) with worst-case O(1) in real-time code.
6. Quoting average case when inputs can be adversarial (hash flooding, sorted input to a naive quicksort).
7. Trusting Big-O alone for small n: an O(n²) insertion sort beats O(n log n)
   merge sort below ~16–32 elements; a linear scan of 50 elements beats a hash lookup.
8. Ignoring the memory hierarchy: linked structures, column-major traversal, huge structs.
9. Benchmarking a debug/ASan build, a single run, or code the optimiser deleted.
10. Dropping a term that is not actually dominated: O(n + m) when m can be n².

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_counting_operations.c](c/01_counting_operations.c) | exact step counts of every loop shape; doubling ratios; two traps | `make run P=dsa/04-complexity-analysis/c/01_counting_operations.c` |
| [c/02_growth_table.c](c/02_growth_table.c) | each class in seconds; measured doubling experiment | `make run P=dsa/04-complexity-analysis/c/02_growth_table.c` |
| [c/03_amortised_dynamic_array.c](c/03_amortised_dynamic_array.c) | ×2 vs ×1.5 vs +16 growth: copies, worst push, time | `make run P=dsa/04-complexity-analysis/c/03_amortised_dynamic_array.c` |
| [c/04_recursion_master_theorem.c](c/04_recursion_master_theorem.c) | binary search, merge sort, naive/memo fib, tree sum: counts vs theory | `make run P=dsa/04-complexity-analysis/c/04_recursion_master_theorem.c` |
| [c/05_best_avg_worst.c](c/05_best_avg_worst.c) | linear search, insertion sort, first-pivot quicksort on 3 input shapes | `make run P=dsa/04-complexity-analysis/c/05_best_avg_worst.c` |
| [c/06_cache_effects.c](c/06_cache_effects.c) | array vs in-order list vs shuffled list; row vs column traversal | `make run P=dsa/04-complexity-analysis/c/06_cache_effects.c` (numbers: `make OPT=1 SAN=0 run P=...`) |
| [cpp/01_stl_complexity.cpp](cpp/01_stl_complexity.cpp) | counting comparator/hasher verifies sort, lower_bound, set, map, unordered_map | `make run P=dsa/04-complexity-analysis/cpp/01_stl_complexity.cpp` |
| [cpp/02_benchmark_harness.cpp](cpp/02_benchmark_harness.cpp) | chrono harness, warm-up, best-of-N, exponent estimation | `make run P=dsa/04-complexity-analysis/cpp/02_benchmark_harness.cpp` |
| [cpp/03_vector_vs_list.cpp](cpp/03_vector_vs_list.cpp) | sorted inserts and traversal: vector vs list | `make OPT=1 SAN=0 run P=dsa/04-complexity-analysis/cpp/03_vector_vs_list.cpp` |

Benchmarks under the default ASan + `-O0` build are 3–10× slower and can even
invert small effects (unoptimised `std::vector` iterators are function calls);
the counts are exact either way. For timing, use `make OPT=1 SAN=0 run P=...`.

---

## 9. Exercises

1. **(easy)** Give the complexity of: (a) checking if an array has duplicates
   with two loops; (b) the same after sorting; (c) with a hash set. Time *and* space.
2. **(easy)** What is the complexity of `for (i = 0; i < strlen(s); i++) if (s[i] == ' ') count++;`? Fix it.
3. **(easy)** Solve with the Master theorem: T(n) = 4T(n/2) + n; T(n) = T(n/2) + n; T(n) = 8T(n/2) + n².
4. **(medium)** Add a ×1.25 growth policy to `03_amortised_dynamic_array.c`.
   Predict copies/push before running (hint: 1/(1.25 − 1)).
5. **(medium)** In `06_cache_effects.c`, vary N from 2¹² to 2²⁴ and plot ns/element
   for the shuffled list. Find your L1, L2, L3 sizes from the steps in the curve.
6. **(medium)** Make the quicksort in `05_best_avg_worst.c` use median-of-3 and
   show the sorted-input case drops to ~n log n.
7. **(hard)** Implement a queue with two stacks and prove (and measure) O(1) amortised dequeue.
8. **(hard)** Write `perf stat -e cycles,instructions,cache-misses` runs for
   `06_cache_effects` (OPT=1 SAN=0) on the PC and on the BeagleBone AI-64. Compare
   instructions-per-cycle for the array and the shuffled list.

---

## 10. Remember forever

```
Big-O = growth of step count. Drop constants & lower terms. Sequence adds, nesting multiplies.
n→2n: O(1) same, O(log n) +1, O(n) ×2, O(n log n) ×2+, O(n²) ×4, O(n³) ×8, O(2ⁿ): n+1 doubles.
10⁹ ops/s: n=10⁶ → O(n) 1 ms, O(n log n) 20 ms, O(n²) 17 min.
Doubling growth → push_back O(1) amortised (copies < 2n); +constant growth → O(n²) total.
Amortised ≠ worst case: one push can still be O(n) (reserve in real-time code).
Master: T = aT(n/b) + n^d; compare d with log_b a → n^(log_b a) | n^d log n | n^d.
Merge sort n log n, binary search log n, naive fib 1.618ⁿ, memo fib n.
Recursion depth = stack space.  Worst case for guarantees, average for throughput.
Cache line 64 B, DRAM ~100 ns: array ≫ shuffled list for the same O(n). Iterate in memory order.
```

---

## 11. Self-test questions

1. Define O, Ω and Θ in one sentence each. Which one is "the" complexity people usually mean?
2. Why is the inner geometric loop (`for i *= 2: for j < i`) O(n) and not O(n log n)?
3. How many comparisons does binary search need for 1 billion sorted items?
4. Why is `push_back` O(1) amortised? Explain with the banker's coins. What breaks with +16 growth?
5. Solve T(n) = 2T(n/2) + n and T(n) = 2T(n/2) + 1 by drawing the recursion tree.
6. Why does naive Fibonacci take exponential time and memoised Fibonacci linear time?
7. What is the space complexity of recursive merge sort? Of quicksort, best and worst?
8. Why is first-element-pivot quicksort O(n²) on sorted input, and how does `std::sort` avoid it?
9. Both are O(n): why can summing a linked list be 100× slower than summing an array?
10. Why does column-by-column traversal of `int m[2048][2048]` run much slower than row-by-row?
11. When is an O(n²) algorithm the right choice?
12. What four rules make a benchmark trustworthy?

Review schedule: see [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).
