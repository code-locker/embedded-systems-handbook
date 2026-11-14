# 14 — Sorting and searching

> Sorting puts data in an order you can rely on. Searching uses that order
> to throw away half of the remaining data at every step.

---

## 1. Why this exists

Unordered data supports only one way of searching: look at everything, which
is O(n). Sorted data supports much more:

- **Fast lookup**: binary search finds an item in 1,000,000 sorted items with
  about 20 comparisons instead of 1,000,000.
- **Grouping and deduplication**: equal items end up next to each other
  (`sort | uniq`).
- **Merging and joining**: two sorted lists combine in one linear pass. Linker
  symbol tables, database merge-joins and `git` packfile indexes rely on this.
- **Ranking**: median, percentiles (p99 latency), top-k.
- **Scheduling**: earliest deadline first, frames in presentation-timestamp
  order, timers in expiry order.

Sorting is also the best-studied problem in computer science, so it is where
you learn divide and conquer, randomisation, lower bounds, stability, cache
effects and hybrid algorithms. All of these ideas come back in later lessons.

The lesson has two halves:

| Half | Algorithms |
|------|------------|
| **Sorting** | bubble, selection, insertion → merge, quick, heap → counting, radix, bucket → introsort (what libraries actually ship) |
| **Searching** | binary search as *"find the first true"*, lower/upper bound, search on the answer, rotated arrays, quickselect / `nth_element` |

---

## 2. Mental model

### Every comparison sort = a sorted region growing into an unsorted one

```
   sorted (final or partial)   |   unsorted
  [ 1  3  4  7 ]               | [ 9  2  8  5 ]
                               ^
             the algorithms differ ONLY in how elements cross this border
```

| algorithm | how one element crosses the border | analogy |
|-----------|------------------------------------|---------|
| bubble    | big elements are carried right by neighbour swaps | bubbles rising in a glass |
| selection | the *smallest remaining* element is fetched and placed | picking the shortest kid for the front of the line |
| insertion | the *next* element is slid left into place | sorting a hand of playing cards |
| merge     | two sorted halves are zipped together | merging two sorted stacks of exam papers |
| quick     | a pivot splits the items into "small" and "big" | a teacher saying "under 150 cm on the left" |
| heap      | the max is popped from a heap kept in the unsorted part | a tournament bracket that keeps naming the winner |

### Why O(n log n) is the floor for comparison sorts

A comparison sort only ever asks "is a < b?". Draw every possible run as a
decision tree. It must have one leaf per possible input order, so n! leaves.

```
                    a<b?
                 /        \
             b<c?          a<c?
            /    \        /    \
        [a b c]  a<c?  [b a c]  b<c?
                /   \           /   \
           [a c b] [c a b] [b c a] [c b a]     3! = 6 leaves
```

A binary tree with n! leaves has height at least log2(n!) ≈ n·log2(n) − 1.44n.
So **any** comparison sort needs about n log n comparisons in the worst case.
Counting and radix sort get under this floor because they never compare
elements. They use the key itself as an array index.

### Binary search = the "guess my number" game

```
 "number between 1 and 100?"   50? lower.  25? higher.  37? lower ...
 each answer halves the range -> at most ceil(log2 100) = 7 guesses
```

The way to remember it for good: binary search finds the **first position
where a yes/no question flips from NO to YES**.

```
  index:   0  1  2  3  4  5  6
  a[i]:    1  2  4  4  4  7  9
  a[i]>=4? F  F  T  T  T  T  T        lower_bound(4) = first T = 2
  a[i]> 4? F  F  F  F  F  T  T        upper_bound(4) = first T = 5
```

Any question that is monotone (all F, then all T) can be binary searched,
even when there is no array, only a range of possible answers.

---

## 3. How it works

### 3.1 The elementary sorts

All three are O(n²) in the worst case and in-place. They still matter:
insertion sort is the fastest algorithm for tiny or nearly sorted arrays, and
every production sort uses it for small pieces.

**Bubble sort**: compare neighbours and swap them if they are out of order.
After pass *k*, the *k* largest items are in their final places. If a pass
makes no swaps, the array is sorted, so stop early.

```
start          [5 1 4 2 8]
pass 1:  5>1 swap [1 5 4 2 8], 5>4 swap [1 4 5 2 8], 5>2 swap [1 4 2 5 8], 5<8
after pass 1   [1 4 2 5 | 8]        8 is final
after pass 2   [1 2 4 | 5 8]
after pass 3   [1 2 | 4 5 8]   no swaps -> stop
```

**Selection sort**: find the minimum of the unsorted part and swap it to the
front. It always does n(n−1)/2 comparisons, but at most n−1 swaps. That makes
it a sensible choice when writes are very expensive, for example when each
write wears a flash cell. It is **not stable**, because the long swap can
jump an element over its equal twin:

```
[2a 2b 1] -> min is 1, swap with a[0] -> [1 2b 2a]     2a now after 2b
```

**Insertion sort**: take the next element (the *key*), shift the larger
sorted elements one slot right, and drop the key into the gap.

```
[1 4 5 | 2 8]   key = 2
[1 4 5 5 8]     5 > 2: shift
[1 4 4 5 8]     4 > 2: shift
[1 2 4 5 8]     1 <= 2: stop, drop key in
```

The inner loop runs once per **inversion**, meaning a pair i<j with
a[i]>a[j]. So insertion sort costs **O(n + inversions)**: n−1 comparisons on
sorted input, n(n−1)/2 shifts on reversed input. Program `01` measures both.

### 3.2 Merge sort

Split the array in half, sort each half recursively, then **merge** the two
halves. Splitting is free; all the work happens in the merge.

```
                 [38 27 43 3 9 82 10]
                /                    \
         [38 27 43]              [3 9 82 10]
          /      \                /        \
       [38]   [27 43]         [3 9]     [82 10]
              /    \          /   \      /    \
            [27]  [43]      [3]   [9]  [82]  [10]
   merge:      [27 43]          [3 9]      [10 82]
         [27 38 43]              [3 9 10 82]
                 [3 9 10 27 38 43 82]
```

**Merge** with two fingers:

```
left  = [1 4 7]     right = [2 3 9]
         i                   j
take the smaller of left[i], right[j]; on a TIE take LEFT (this is what makes it stable)
out: 1 | 1 2 | 1 2 3 | 1 2 3 4 | 1 2 3 4 7 | + rest 9
```

- Every level of the tree does O(n) merging, and there are log2(n) levels, so
  the cost is **O(n log n) in every case**.
- It needs an O(n) buffer. Allocate it **once**, not inside every recursive
  call.
- Skip the merge when `a[mid-1] <= a[mid]` (the halves are already in order).
  This makes sorted input O(n).
- **Bottom-up** merge sort needs no recursion. Merge runs of width 1, then 2,
  then 4, and so on:

```
width 1: [38|27|43|3|9|82|10] -> [27 38|3 43|9 82|10]
width 2:                         [3 27 38 43|9 10 82]
width 4:                         [3 9 10 27 38 43 82]
```

Bottom-up merge sort is the natural fit for **linked lists**. The Linux
kernel's `list_sort()` is one: it needs O(1) extra memory and is stable. It is
also the natural fit for **external sorting**, where the data is too big for
RAM: sort chunks into sorted run files, then merge the runs.

### 3.3 Quicksort

Pick a **pivot**. **Partition** the array so smaller items go left and larger
items go right. Recurse on both sides. There is no merge step: once both sides
are sorted, the whole array is sorted.

**Lomuto partition** (pivot = last element). The invariant while `j` scans:

```
 lo           i            j              hi
 [  < pivot  |  >= pivot  |  unknown  | pivot ]

[7 2 1 6 8 5 3 | 4]   pivot = 4
 j: 7 skip | 2 -> swap to front | 1 -> swap | 6,8,5 skip | 3 -> swap
[2 1 3 6 8 5 7 4]     i = 3
swap pivot into a[i]:  [2 1 3 | 4 | 8 5 7 6]    4 is now FINAL
```

**Hoare partition** (the original, about 3× fewer swaps): two fingers walk
inward. Each stops at an element that is on the wrong side; swap the two
elements and continue until the fingers cross.

```
[7 2 1 6 8 5 3 4]   pivot value = 6 (middle)
 i->7         j<-4   swap  [4 2 1 6 8 5 3 7]
 i->6       j<-3     swap  [4 2 1 3 8 5 6 7]
 i->8   j<-5         swap  [4 2 1 3 5 8 6 7]
 i->8 j<-5 crossed -> return j = 4:   [4 2 1 3 5 | 8 6 7]
recurse on [lo..j] and [j+1..hi]   (NOT j-1: the pivot is not necessarily at j)
```

#### Why the worst case is O(n²)

Take sorted input with "last element" as the pivot. The pivot is always the
maximum, so every partition peels off a single element:

```
size n   -> split (n-1, 0)      n-1 comparisons
size n-1 -> split (n-2, 0)      n-2 comparisons
...
total = (n-1) + (n-2) + ... + 1 = n(n-1)/2  = O(n^2)
recursion depth = n-1 frames   <- on an 8 KB kernel or RTOS stack this CRASHES
```

Program `03` measures exactly 1,999,000 comparisons and depth 1999 for n=2000.
The same thing happens with Lomuto on **all-equal** input: `a[j] < pivot` is
never true. Hoare stops on equal keys, so its fingers meet in the middle and
the splits stay balanced.

#### The fixes

| Problem | Fix |
|---------|-----|
| sorted or reversed input | median-of-3 pivot (first, middle, last), or a **random** pivot |
| an adversary who knows your pivot rule | random pivot, or introsort's depth limit (3.7) |
| many duplicates | **3-way partition** `[ <p | ==p | >p ]`; the `==` block is never touched again |
| O(n) stack depth | recurse into the **smaller** side and loop on the larger one, so depth ≤ log2 n |
| slow on tiny pieces | stop at about 16 elements and finish with insertion sort |

Why the average is O(n log n): even a 1:9 split gives log₁₀⁄₉(n) levels of
O(n) work, which is still O(n log n). A random pivot is that good or better
with high probability.

### 3.4 Heapsort

Treat the array as a binary tree: `parent(i) = (i-1)/2`, `left = 2i+1`,
`right = 2i+2`. This is the same layout as in lesson 10.

```
1. build a MAX-heap bottom-up: sift_down every internal node, last to first. O(n)!
2. repeat: swap a[0] (the max) with the last heap slot, shrink the heap, sift_down(0).

start           [4 10 3 5 1 8 7]
max-heap built  [10 5 8 4 1 3 7]
pop -> a[6]     [8 5 7 4 1 3 | 10]
pop -> a[5]     [7 5 3 4 1 | 8 10]
pop -> a[4]     [5 4 3 1 | 7 8 10]
...             [1 | 3 4 5 7 8 10]
```

- O(n log n) **worst case**, O(1) extra memory, no recursion.
- Not stable, not adaptive. Its memory access jumps from i to 2i+1, which is
  unfriendly to the cache, so it is usually about 2× slower than quicksort.
- **Why building the heap is O(n), not O(n log n)**: half the nodes are leaves
  and cost 0. A quarter of them sift at most 1 level, an eighth at most 2, and
  so on. The sum Σ n·h/2^(h+1) ≤ n.

### 3.5 Non-comparison sorts

**Counting sort** for keys in [0, k). It is O(n + k) and stable.

```
input  [2 5 3 0 2 3 0 3]                 k = 6
count  value: 0 1 2 3 4 5
              2 0 2 3 0 1
prefix start: 0 2 2 4 7 7       start[v] = first output slot for value v
scatter LEFT->RIGHT: out[start[v]++] = v      (equal keys keep input order -> STABLE)
output [0 0 2 2 3 3 3 5]
```

**LSD radix sort**: run a stable counting sort on each digit, **least
significant digit first**.

```
start     [170  45  75  90 802  24   2  66]
by 1s     [170  90 802   2  24  45  75  66]
by 10s    [802   2  24  45  66 170  75  90]   802 stays before 2: kept from the last pass
by 100s   [  2  24  45  66  75  90 170 802]
```

It is correct **only because each pass is stable**. For 32-bit keys, use
base 256: 4 passes of a counting sort with 256 buckets, O(4(n+256)) = O(n).
To radix-sort signed ints, flip the sign bit. For IEEE floats, flip the sign
bit of positive numbers and all bits of negative numbers (`cpp/05`).

**Bucket sort**: for keys spread uniformly over [0,1), put x into bucket
⌊x·n⌋. Each bucket then holds about one item, so sorting the buckets and
concatenating them is O(n) expected. On skewed input everything lands in one
bucket, and the cost becomes whatever the per-bucket sort costs.

```
[.78 .17 .39 .26 .72 .94 .21 .12 .23 .68]    10 buckets
 b1: .17 .12   b2: .26 .21 .23   b3: .39   b6: .68   b7: .78 .72   b9: .94
 sort each bucket -> concatenate
```

### 3.6 Stability, in-place, adaptivity

| property | meaning | why you care |
|----------|---------|--------------|
| **stable** | equal keys keep their original relative order | sort by name, then *stable* sort by priority, and you get "priority, then name" (clicking table columns works this way). Required for LSD radix. |
| **in-place** | O(1) (or O(log n) stack) extra memory | kernel, ISR, MCU with 64 KB RAM; huge arrays |
| **adaptive** | faster when the input is already partly sorted | real data is often almost sorted: log files, timestamps, appended records |

To make any sort stable, sort `(key, original_index)` pairs.

### Comparison table

| algorithm | best | average | worst | extra memory | stable | in-place | adaptive |
|-----------|------|---------|-------|--------------|--------|----------|----------|
| bubble (early exit) | n | n² | n² | 1 | yes | yes | yes |
| selection | n² | n² | n² | 1 | **no** | yes | no |
| insertion | n | n² | n² | 1 | yes | yes | **yes** (n + inversions) |
| merge (top-down) | n log n (n with the skip check) | n log n | n log n | n | **yes** | no | with skip check |
| quick (random pivot) | n log n | n log n | **n²** (very unlikely) | log n stack | no | yes | no |
| heap | n log n | n log n | n log n | 1 | no | yes | no |
| introsort (`std::sort`) | n log n | n log n | n log n | log n | no | yes | partly |
| timsort (Python, Java objects) | n | n log n | n log n | n | yes | no | **very** |
| counting | n + k | n + k | n + k | n + k | yes | no | — |
| radix (d digits, base b) | d(n + b) | d(n + b) | d(n + b) | n + b | yes | no | — |
| bucket | n | n | n² | n | yes* | no | — |

\* if the per-bucket sort is stable.

### 3.7 Introsort and what real libraries use

**Introsort** (David Musser, 1997) combines three sorts so that each covers
another's weakness:

```
introsort(a, n):
    depth_limit = 2 * floor(log2 n)
    loop(lo, hi, depth):
        while hi - lo > 16:
            if depth == 0: heapsort(a[lo..hi]); return     <- adversarial input detected
            depth--
            pivot = median_of_3; partition
            recurse on the smaller side, loop on the larger side
        (pieces <= 16 are left alone)
    insertion_sort(whole array)     <- every element is at most 16 slots away: O(16 n)
```

Program `09` feeds sorted input to quicksort with a deliberately bad pivot.
Plain quicksort makes 200 million comparisons. Introsort detects the problem,
falls back to heapsort, and makes 1.1 million.

| library | what it really does |
|---------|---------------------|
| `std::sort` (libstdc++, GCC) | introsort: median-of-3 Hoare-style partition, depth limit 2·log2 n → heapsort, final insertion sort on pieces < 16 |
| `std::sort` (libc++, Clang) | also an introsort-style hybrid with small-array special cases; newer versions add branchless (BlockQuicksort-style) partitioning |
| `std::stable_sort` | merge sort into a temporary buffer; if the buffer cannot be allocated, an in-place merge in O(n log² n) |
| `std::partial_sort` | heap-based selection, O(n log k) |
| `std::nth_element` | introselect: quickselect with a fallback for bad pivots |
| `std::list::sort` | merge sort on the nodes (relinks pointers, copies nothing) |
| C `qsort` (glibc) | **the C standard guarantees neither the algorithm, the complexity nor stability.** glibc has long used a merge sort into a temporary buffer (so it is often stable by accident), with an in-place quicksort/introsort fallback; the details have changed between releases. Never rely on qsort being stable. |
| C `qsort` (musl) | smoothsort, a heapsort variant that is O(n) on sorted input |
| Linux kernel `sort()` (`lib/sort.c`) | **heapsort**: no recursion (the kernel stack is only 16 KB), no allocation, O(n log n) guaranteed. Used for exception tables, jump-label entries, ftrace call sites and more. |
| Linux kernel `list_sort()` (`lib/list_sort.c`) | bottom-up merge sort on `struct list_head`, stable, O(1) extra memory |
| Python `sorted`, Java `Arrays.sort(Object[])` | Timsort: finds existing runs, extends short ones with binary insertion sort, merges with "galloping" |
| Java `Arrays.sort(int[])` | dual-pivot quicksort |
| Rust `sort_unstable`, Go `sort` (≥1.19) | pattern-defeating quicksort (pdqsort) and its descendants |

### 3.8 Binary search and its variants

**One template that never gets off-by-one wrong.** Use a half-open range
`[lo, hi)`, loop while `lo < hi`, and keep the invariant "the answer is in
[lo, hi]":

```c
size_t lo = 0, hi = n;                   /* answer in [lo, hi]; hi = n means "none" */
while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;     /* never (lo+hi)/2: it can overflow */
    if (pred(mid)) hi = mid;             /* mid might be the answer: keep it */
    else           lo = mid + 1;         /* mid is not: discard it */
}
return lo;                               /* first index where pred is true */
```

Trace of `lower_bound(4)` on `[1 2 4 4 4 7 9]` (pred = `a[mid] >= 4`):

```
lo=0 hi=7  mid=3 a=4 T -> hi=3
lo=0 hi=3  mid=1 a=2 F -> lo=2
lo=2 hi=3  mid=2 a=4 T -> hi=2
lo=2 hi=2  stop -> 2
```

| variant | predicate | result |
|---------|-----------|--------|
| `lower_bound(x)` | `a[i] >= x` | first slot ≥ x: where to insert x, or the first copy of x |
| `upper_bound(x)` | `a[i] > x` | first slot > x |
| count of x | — | `upper_bound − lower_bound` |
| last element ≤ x | — | `upper_bound(x) − 1` (seeking to the keyframe before t) |
| first / last occurrence | lower_bound, then upper_bound − 1, then check equality | |
| rotated array: min | `a[i] <= a[n-1]` | the rotation point; then binary search the correct half |
| **search on the answer** | `feasible(c)` over a *range of values* | smallest c that works |

**Search on the answer** is a common interview pattern and a common design
pattern. "What is the smallest buffer, capacity, speed or bandwidth that
works?" If being feasible is monotone (once c works, every bigger c works),
binary search c over [min_possible, max_possible] and test feasibility each
time:

```
packets 1..10, 5 slots, find min capacity C
feasible(C)? C:  10 11 12 13 14 15 16 ...
                  F  F  F  F  F  T  T     -> 15   ([1..5][6 7][8][9][10])
```

The cost is O(n · log(range)) instead of trying every value.

Classic bugs: `(lo+hi)/2` overflow (this bug sat in Java's `binarySearch`
for nine years), `hi = n-1` with `size_t` when n = 0, `hi = mid` combined with
a closed `while (lo <= hi)` loop (infinite loop), and `std::lower_bound` on a
`std::set` (it compiles, but takes O(n) iterator steps; use `set.lower_bound`).

### 3.9 Selection: quickselect and nth_element

To find the k-th smallest item you don't need to sort. Partition, then keep
only the side that contains k:

```
k = 3 in [9 1 8 2 7 3 6]   pivot 6 -> [1 2 3 | 6 | 7 9 8]   pivot index 3 == k -> done
expected work: n + n/2 + n/4 + ... = 2n  -> O(n) average  (worst n^2, like quicksort)
```

Afterwards a[k] is correct, and the array is **partitioned around it**. That
is exactly the contract of `std::nth_element`. Median-of-medians (groups of 5)
guarantees a 30/70 split, giving O(n) in the worst case, but its constant is
large, so libraries only use it as a fallback.

There are three ways to get the top-k:

| method | cost | when |
|--------|------|------|
| `nth_element`, then sort the first k | O(n + k log k) | all data in memory |
| `partial_sort` | O(n log k) | simplest code |
| min-heap of size k | O(n log k), **O(k) memory** | **streaming**: sensor samples, packets, logs |

### 3.10 Generic sorting: qsort vs templates

C is generic over **bytes**:

```c
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
/* element i lives at (char *)base + i * size      -- void* arithmetic is not ISO C */
```

C++ is generic over **types**:

```cpp
template <class RandomIt, class Compare = std::less<>>
void sort(RandomIt first, RandomIt last, Compare cmp = {});
```

| | `qsort` | `std::sort` |
|-|---------|-------------|
| element move | `memcpy` of `size` bytes | the type's move/copy (could be a register) |
| comparison | indirect call through a function pointer, never inlined | the lambda is inlined into the loop |
| type safety | none: the wrong cast compiles | checked by the compiler |
| typical speed (program `cpp/06`) | ~1.8× slower | baseline |

Comparator rules (both languages):

- C must return <0, 0 or >0. Write `(a > b) - (a < b)`, **never `a - b`**,
  because `INT_MAX - (-1)` overflows.
- C++ must be a **strict weak ordering**. Use `<`, **never `<=`**: `cmp(x,x)`
  must be false, or `std::sort` may read out of bounds.
- For an array of `char *`, the comparator receives `char **`. Unwrap one
  level before calling `strcmp`.
- For multiple keys in C++, compare `std::tie(a.k1, a.k2) < std::tie(b.k1, b.k2)`.
  To make one key descending, swap a and b for that key only.

### 3.11 Benchmark

`c/10_sort_benchmark.c` and `cpp/06_sort_benchmark.cpp` time everything on
random, sorted, reversed and few-unique input, and check every result. Sample
numbers (x86-64 laptop, n = 2,000,000 ints, `-O2`, no sanitizers, in ms; your
numbers will differ):

```
C                 random   sorted  reversed  few-unique
merge              116      1.3      14.5       34.6
quick (random)      94     13.4      13.9       24.2
heap               158     61.2      67.8       64.1
radix               12     27.2      25.7       24.2
qsort (glibc)      174     29.7      32.6       79.8

C++               random   sorted  reversed  few-unique
std::sort (lambda)  96     10.4       6.9       24.7
std::sort (fn ptr) 126     33.5      23.5       39.9
std::stable_sort   113      7.7      13.4       35.1
qsort              174     30.9      35.0       81.3
```

Lessons from the numbers:

- Heapsort's cache misses cost real time.
- Radix sort is unbeatable on random ints, and never benefits from presorted
  data.
- An inlined comparator is worth about 30%.
- qsort pays for genericity.

Run the benchmarks yourself. **Delete the old binary first**, because make
does not rebuild when only the flags change:

```
rm -f build/dsa/14-sorting-and-searching/c/10_sort_benchmark
make run P=dsa/14-sorting-and-searching/c/10_sort_benchmark.c OPT=1 SAN=0
```

---

## 4. Complexity table

| operation | time | space | why |
|-----------|------|-------|-----|
| bubble / insertion, sorted input | O(n) | O(1) | one pass with no swaps / every key stops immediately |
| bubble / selection / insertion, worst | O(n²) | O(1) | n(n−1)/2 comparisons |
| insertion sort, general | O(n + inversions) | O(1) | one shift per inversion |
| merge sort | O(n log n) always | O(n) | log n levels × O(n) merge |
| quicksort, average / random pivot | O(n log n) | O(log n) | balanced-enough splits with high probability |
| quicksort, worst | O(n²) | O(n) stack (O(log n) with smaller-side-first) | (n−1, 0) splits every time |
| heapsort | O(n log n) always | O(1) | O(n) build + n sift-downs of O(log n) |
| build heap bottom-up | O(n) | O(1) | most nodes are near the bottom |
| introsort | O(n log n) worst | O(log n) | heapsort fallback after 2 log n levels |
| counting sort | O(n + k) | O(n + k) | one histogram pass, one prefix pass, one scatter pass |
| LSD radix, d digits base b | O(d(n + b)) | O(n + b) | d stable counting passes |
| bucket sort, uniform input | O(n) expected | O(n) | ~1 item per bucket |
| any comparison sort | Ω(n log n) | — | decision tree has n! leaves |
| binary search / lower / upper bound | O(log n) | O(1) | the range halves each step |
| search on the answer | O(cost(feasible) · log range) | O(1) | binary search over values |
| quickselect / nth_element | O(n) avg, O(n²) worst | O(1) | n + n/2 + n/4 + … |
| median of medians | O(n) worst | O(log n) | T(n) ≤ T(n/5) + T(7n/10) + O(n) |
| top-k with a heap | O(n log k) | O(k) | each item costs one heap op on a size-k heap |

---

## 5. C vs C++

| task | C | C++ |
|------|---|-----|
| sort ints | `qsort(a, n, sizeof *a, cmp)` | `std::sort(v.begin(), v.end())` |
| stable sort | write your own (`c/07`) or rely on nothing | `std::stable_sort` |
| descending | write a reversed comparator | `std::sort(..., std::greater<>{})` |
| multi-key | chained `if` in the comparator | `std::tie(...) < std::tie(...)` |
| find in sorted | `bsearch` (returns *a* match, not the first) | `lower_bound`, `upper_bound`, `equal_range`, `binary_search` |
| k-th / top-k | write quickselect (`c/08`) | `nth_element`, `partial_sort`, `priority_queue` |
| linked list | write merge sort / kernel `list_sort` | `std::list::sort` |
| heap | write sift-down (`c/04`) | `make_heap`, `push_heap`, `pop_heap`, `sort_heap` |
| radix / counting | write it (`c/05`) | write it, but once, as a template (`cpp/05`) |
| dedupe | sort, then a compaction loop | `sort` + `unique` + `erase` |

`bsearch` in C cannot answer "where would x go?" because it only returns a
pointer to a match or NULL. That is why `c/06` writes `lower_bound` by hand.
In C++, use `lower_bound` and almost never `binary_search`, which only
returns a `bool`.

---

## 6. Where it is used in the real world

- **Linux kernel**:
  - `sort()` (heapsort) sorts the exception table at boot, so a fault address
    can be binary-searched (`search_exception_tables`).
  - Also used for jump-label entries and ftrace call sites.
  - `list_sort()` orders linked lists in filesystems such as XFS and UBIFS.
    `bsearch()` lives in `lib/bsearch.c`.
- **Symbol lookup**: `scripts/kallsyms` sorts the kernel's symbol table by
  address at build time, so turning an oops address into `function+offset` is
  a binary search ("last symbol ≤ address" = `upper_bound − 1`). `addr2line`
  and debuggers do the same with ELF symbol tables.
- **GStreamer and multimedia**:
  - Demuxers keep a sorted index of keyframe timestamps; a seek is an
    `upper_bound − 1` lookup (`cpp/03`).
  - Jitter buffers (`rtpjitterbuffer`) keep packets ordered by sequence
    number and timestamp; reordering a nearly sorted stream is insertion-sort
    territory.
  - Median filters remove "salt-and-pepper" sensor noise using selection
    (`cpp/04`).
- **Embedded and vision (TDA4VM)**:
  - Non-maximum suppression in object detection sorts boxes by score
    (top-k).
  - Radix and counting sort are the standard for 8/16-bit pixel or depth
    values and on DSP/GPU hardware.
  - Percentile latency (p99) is computed with selection.
- **Databases**: ORDER BY uses external merge sort when data exceeds memory.
  Merge joins, B-tree bulk loading (sort first, then build) and index lookups
  are all binary search over pages.
- **Networking**: routing tables use longest-prefix match (sorted prefixes or
  a trie). Strict-priority packet schedulers bucket traffic by priority
  (counting sort, `cpp/05`), and top-k "heavy hitters" are tracked with a
  heap.
- **Tools**: `sort`(1) is an external merge sort with temp files. `git
  bisect` is binary search over history. `diff` and `uniq` assume sorted
  input.

---

## 7. Common mistakes and pitfalls

1. **`return a - b;` in a comparator.** It overflows for large or opposite-sign
   values and then gives a wrong order. Use `(a > b) - (a < b)`.
2. **`<=` in a C++ comparator.** That is not a strict weak ordering, so it is
   undefined behaviour, and `std::sort` can read past the end.
3. **Passing `strcmp` directly to `qsort` on an array of `char *`.** The
   comparator gets `char **`, not `char *`.
4. **`mid = (lo + hi) / 2`** overflows on huge arrays. Use
   `lo + (hi - lo) / 2`.
5. **Mixing binary-search styles.** A closed `[lo, hi]` interval with
   `hi = mid` loops forever. Pick the half-open template and always use it.
6. **`size_t hi = n - 1` with n = 0** wraps to SIZE_MAX.
7. **Hoare partition: recursing on `[lo, p-1]`.** With Hoare the pivot is not
   at p; recurse on `[lo, p]` and `[p+1, hi]`. Also, a pivot taken from the
   last element with Hoare can loop forever.
8. **Quicksort on sorted or all-equal input with a fixed pivot**: O(n²) time
   and O(n) stack, which crashes small stacks. Use a random pivot, median-of-3,
   introsort, a 3-way partition and smaller-side-first recursion.
9. **Assuming `qsort` or `std::sort` is stable.** They are not; use
   `stable_sort` or add the index as a tiebreaker.
10. **Allocating the merge buffer inside the recursion**: n mallocs instead
    of one.
11. **An unstable inner pass in LSD radix sort.** It silently gives wrong
    results.
12. **Radix-sorting signed ints or floats as raw bits.** Negative numbers come
    out after positive ones. Flip the sign bit (and for negative floats, flip
    all bits).
13. **NaN in a float sort.** `NaN < x` is always false, which breaks strict
    weak ordering. Filter NaNs out first.
14. **`std::lower_bound` on a `std::set`/`std::map`** is O(n). Use the member
    function.
15. **`bsearch` on unsorted data, or with a different comparator from the
    one used to sort.** Both silently give wrong answers.
16. **Benchmarking a sanitizer or -O0 build,** or with a stale binary after
    changing flags.

---

## 8. Files in this lesson

| file | what it shows | run |
|------|---------------|-----|
| [c/01_elementary_sorts.c](c/01_elementary_sorts.c) | bubble/selection/insertion with pass-by-pass traces, comparison counts on sorted and reversed input, the stability demo | `make run P=dsa/14-sorting-and-searching/c/01_elementary_sorts.c` |
| [c/02_merge_sort.c](c/02_merge_sort.c) | top-down (recursion trace) and bottom-up (width trace) merge sort, stability check on records | `make run P=dsa/14-sorting-and-searching/c/02_merge_sort.c` |
| [c/03_quick_sort.c](c/03_quick_sort.c) | Lomuto and Hoare partitions traced, random pivot + smaller-side-first, 3-way, a measured O(n²) worst case | `make run P=dsa/14-sorting-and-searching/c/03_quick_sort.c` |
| [c/04_heap_sort.c](c/04_heap_sort.c) | bottom-up heap build, pop-to-the-end trace, non-adaptivity measured | `make run P=dsa/14-sorting-and-searching/c/04_heap_sort.c` |
| [c/05_counting_radix_bucket.c](c/05_counting_radix_bucket.c) | counting sort (histogram → prefix → scatter), decimal radix trace, 32-bit radix, bucket sort | `make run P=dsa/14-sorting-and-searching/c/05_counting_radix_bucket.c` |
| [c/06_binary_search.c](c/06_binary_search.c) | classic search, lower/upper bound, first/last occurrence, search on the answer, isqrt, rotated array | `make run P=dsa/14-sorting-and-searching/c/06_binary_search.c` |
| [c/07_generic_sort_qsort.c](c/07_generic_sort_qsort.c) | a qsort-compatible **stable** generic sort on `void *`, comparators for ints/structs/strings, the `a - b` bug, bsearch | `make run P=dsa/14-sorting-and-searching/c/07_generic_sort_qsort.c` |
| [c/08_quickselect.c](c/08_quickselect.c) | quickselect, median-of-medians, linear comparison counts, p99 latency | `make run P=dsa/14-sorting-and-searching/c/08_quickselect.c` |
| [c/09_introsort.c](c/09_introsort.c) | full introsort; bad pivots: 200M comparisons for plain quicksort vs 1.1M for introsort | `make run P=dsa/14-sorting-and-searching/c/09_introsort.c` |
| [c/10_sort_benchmark.c](c/10_sort_benchmark.c) | timings for all the sorts on 4 input shapes (use `OPT=1 SAN=0`, see 3.11) | `make run P=dsa/14-sorting-and-searching/c/10_sort_benchmark.c` |
| [cpp/01_generic_sorts_templates.cpp](cpp/01_generic_sorts_templates.cpp) | insertion/merge/quick/heap sort as iterator+comparator templates on vector, deque, strings, frames | `make run P=dsa/14-sorting-and-searching/cpp/01_generic_sorts_templates.cpp` |
| [cpp/02_stl_sorting.cpp](cpp/02_stl_sorting.cpp) | `sort` with `std::tie`, `stable_sort` two-pass trick, `partial_sort`, `nth_element`, argsort, `list::sort`, strict weak ordering | `make run P=dsa/14-sorting-and-searching/cpp/02_stl_sorting.cpp` |
| [cpp/03_binary_search_stl.cpp](cpp/03_binary_search_stl.cpp) | `lower_bound`/`upper_bound`/`equal_range`/`partition_point`, generic `first_true`, keyframe seek, max-min-gap, bisection | `make run P=dsa/14-sorting-and-searching/cpp/03_binary_search_stl.cpp` |
| [cpp/04_quickselect_topk.cpp](cpp/04_quickselect_topk.cpp) | template quickselect vs `nth_element`, top-k three ways (streaming heap), median filter | `make run P=dsa/14-sorting-and-searching/cpp/04_quickselect_topk.cpp` |
| [cpp/05_radix_counting_bucket.cpp](cpp/05_radix_counting_bucket.cpp) | one radix sort for uint8..uint64, signed ints, float/double (key bit tricks); stable counting sort of packets | `make run P=dsa/14-sorting-and-searching/cpp/05_radix_counting_bucket.cpp` |
| [cpp/06_sort_benchmark.cpp](cpp/06_sort_benchmark.cpp) | `std::sort` (lambda vs function pointer) vs `stable_sort` vs `qsort` vs radix | `make run P=dsa/14-sorting-and-searching/cpp/06_sort_benchmark.cpp` |

Every program checks itself against a reference (`qsort`, `std::sort`,
`std::stable_sort` or a linear scan) on hundreds of random arrays, including
n = 0, n = 1, heavy duplicates, and sorted and reversed input.

---

## 9. Exercises

1. *(easy)* Change `bubble_sort` in `c/01` to alternate directions each pass
   (cocktail shaker sort). Which input does it fix? Hint: a single small
   element at the far right, the "turtle".
2. *(easy)* Using `lower_bound`/`upper_bound`, write `count_in_range(a, n, lo, hi)`,
   which returns the number of elements in [lo, hi]. Check it against a
   linear count.
3. *(easy)* Make `merge` in `c/02` unstable by changing one character. Watch
   the stability assert fire.
4. *(medium)* Sort an array of 0s, 1s and 2s in one pass. Hint: this is
   exactly the 3-way partition from `c/03` (the Dutch national flag).
5. *(medium)* Find the **first bad version** in 1..n given `is_bad(v)`, using
   the half-open template. Then find the **peak** of a mountain array using
   the predicate `a[i] > a[i+1]`.
6. *(medium)* Implement **binary insertion sort**: find the insert position
   with `upper_bound` (why upper and not lower?). How many comparisons does
   it save? How many moves?
7. *(medium)* **Kth smallest in two sorted arrays** in O(log(m+n)). Hint:
   binary search on how many elements to take from the first array.
8. *(medium)* Count **inversions** in O(n log n) by adding a counter to merge
   sort's merge step. Check it against the shift count of insertion sort from
   `c/01`.
9. *(hard)* **External sort**: sort 1,000,000 ints while holding only 10,000
   in memory at a time. Write sorted runs to temp files (`tmpfile()`), then do
   a k-way merge with a min-heap.
10. *(hard)* Implement **MSD radix sort** for strings (recursive, 256 buckets,
    insertion sort for small buckets) and compare it with `std::sort` on
    100,000 random words.
11. *(hard)* Write a **median-of-3 killer**: an input that drives a naive
    median-of-3 quicksort (without a depth limit) to O(n²). Confirm that
    `c/09` introsort handles it. Hint: search for McIlroy's "A Killer
    Adversary for Quicksort".
12. *(hard)* Port the Linux kernel's `lib/sort.c` idea: a generic heapsort
    with `size` and a `swap_func`. Add the "bottom-up" sift-down optimisation
    (walk down to a leaf first, then back up) and count how many comparisons
    it saves.

---

## 10. Remember forever

```
Sorted region grows; algorithms differ in how ONE element crosses the border.
Insertion: O(n + inversions) -> best for tiny/nearly-sorted; finishes every real sort.
Merge: split, sort halves, zip (ties from LEFT = stable). n log n always, O(n) buffer.
Quick: partition around pivot, recurse; sorted input + fixed pivot = n^2, depth n.
   fixes: random/median-3 pivot, 3-way for dups, recurse smaller side, introsort.
Heap: build O(n) bottom-up, swap max to end, sift down. n log n worst, O(1) memory.
Comparison sorts >= log2(n!) ~ n log n. Counting/radix beat it: key = array index.
LSD radix works ONLY because each counting pass is stable.
Binary search = first index where a monotone predicate turns TRUE:
   lo=0, hi=n; while lo<hi: mid=lo+(hi-lo)/2; pred(mid) ? hi=mid : lo=mid+1.
Comparators: (a>b)-(a<b) in C, strict '<' in C++. std::sort=introsort, kernel sort()=heapsort.
```

Review this card using the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. Why is insertion sort O(n) on sorted input but selection sort is not?
   Count the comparisons for each.
2. Show with a 3-element example why selection sort is not stable.
3. Which single character in merge sort's merge decides stability?
4. Draw the decision tree argument. Why does it not apply to counting sort?
5. Give two inputs that make a "last element pivot" Lomuto quicksort O(n²).
   What happens to the stack?
6. In Hoare's partition, why do you recurse on `[lo, p]` rather than
   `[lo, p-1]`?
7. How does recursing into the smaller side bound the stack at O(log n)?
8. Why is building a heap O(n) while inserting n items one by one is
   O(n log n)?
9. What does introsort do when recursion gets too deep, and why heapsort and
   not merge sort?
10. What does `std::sort` use? `std::stable_sort`? glibc `qsort`? The Linux
    kernel's `sort()`? Why did the kernel pick its algorithm?
11. Write the half-open binary search template from memory. What do
    `lower_bound` and `upper_bound` return for an x that is not present?
12. "Find the minimum capacity such that…": how do you recognise
    search-on-the-answer, and what must be true of the predicate?
13. Why is LSD radix sort wrong if a pass is unstable? How do you radix-sort
    negative ints and floats?
14. What is the expected cost of quickselect, and why is it n + n/2 + … and
    not n log n?
15. Why is `std::sort` with a lambda faster than `qsort` on the same data?
16. Name three bugs that have appeared in real comparators or binary searches.
