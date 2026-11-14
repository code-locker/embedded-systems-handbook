# 12 — Advanced trees: segment tree, Fenwick tree, B-tree / B+ tree, skip list, interval tree

> Prerequisites: [09-trees](../09-trees/README.md) (BST, rotations, AVL /
> red-black) and [10-heaps](../10-heaps-priority-queues/README.md) (the
> "tree stored in an array, children at 2i and 2i+1" trick).

---

## 1. Why this exists

A plain balanced BST answers *"is key k here?"* in O(log n). Real systems ask
harder questions, and each structure in this lesson exists for one of them:

| Question you keep asking | Naive cost | Structure | Cost |
|---|---|---|---|
| "Sum / min / max of `a[l..r]`, while `a` keeps changing" | O(n) per query | **Segment tree** | O(log n) |
| "Add v to every element in `a[l..r]`" | O(n) | **Segment tree + lazy propagation** | O(log n) |
| "Prefix sum of `a[1..i]`, with point updates", in 10 lines of code | O(n) | **Fenwick tree (BIT)** | O(log n) |
| "Find a key when every node visit is a **disk read**" | log2(n) ~ 30 reads | **B-tree / B+ tree** | log_B(n) ~ 3–4 reads |
| "Ordered set, concurrent-friendly, no rotations" | — | **Skip list** | O(log n) expected |
| "Which stored ranges overlap `[a, b]`?" | O(n) | **Interval tree** | O(log n + k) |

---

## 2. Mental model

**One analogy per structure** — these are the pictures to keep:

* **Segment tree = a company org chart of subtotals.** Each manager knows the
  total of their whole team. To get the total of "employees 3..11" you ask a
  few managers whose teams fit exactly inside, not every employee.
* **Fenwick tree = a ruler's tick marks.** Index 8 is a long tick covering 8
  cells, index 4 a medium one covering 4, odd indices tiny ones covering 1.
  The length of the tick is the lowest set bit of the index.
* **B-tree = a library card catalogue.** Each drawer (a disk page) holds
  hundreds of sorted cards and tells you which drawer to open next. Three
  drawers get you to any of a billion books.
* **Skip list = express and local trains.** Ride the express (top lane) as far
  as you can without overshooting, then change to slower lines.
* **Interval tree = a BST of bookings where every manager also remembers
  "the latest end time anywhere in my team"**, so whole teams can be skipped.

```
segment tree over a = [5, 2, 7, 1]       Fenwick over 8 cells (who covers what)
                                          i:  1  2  3  4  5  6  7  8
           [0..3] 15                          |  |  |  |  |  |  |  |
          /         \                     8:  ========================
     [0..1] 7     [2..3] 8                4:  ==========
     /    \        /    \                 6:              ======
   [0]5  [1]2   [2]7   [3]1               2:  ====
                                          1,3,5,7: one cell each

B-tree node = one disk page               skip list
+----+----+----+-----+----+               L2 H ----------> 9 ------------> 21 -> NIL
| 10 | 20 | 30 | ... |k_m |               L1 H ----> 4 --> 9 ------> 14 -> 21 -> NIL
+----+----+----+-----+----+               L0 H -> 1 -> 4 -> 7 -> 9 -> 12 -> 14 -> 17 -> 21 -> NIL
c0  c1   c2   c3 ... c_m+1  (m ~ 100s)
```

---

## 3. How it works

### 3.1 Segment tree

**Layout.** Store it like a heap: node 1 is the root covering `[0, n-1]`; node
`i` covering `[lo, hi]` has children `2i` → `[lo, mid]` and `2i+1` →
`[mid+1, hi]`. Allocate `4n` slots (always enough). Each node holds the
*combine* of its slice: sum, min, max, gcd... — anything **associative with an
identity** (a *monoid*).

**Build** — O(n): recurse to leaves, then each parent = combine(children).

**Query `[l, r]`** — three cases at every node:

```
node slice [lo, hi] vs query [l, r]

 1. disjoint      lo..hi    l..r         -> return identity (0 for sum, +inf for min)
 2. fully inside      l [lo..hi] r       -> return node value, DO NOT descend
 3. partial         lo..[l..hi]..r       -> ask both children, combine
```

Example: `sum[1..3]` on `[5, 2, 7, 1]`:

```
            [0..3] 15        partial -> both children
           /         \
     [0..1] 7      [2..3] 8   <- fully inside: take 8, stop
     /     \
  [0]5    [1]2               <- [0] disjoint (0), [1] inside (2)
  answer = 0 + 2 + 8 = 10
```

Why O(log n): on each level at most **two** nodes are partial (the ones
containing the left and right ends of the query), so at most ~4 nodes per
level are touched.

**Point update `a[pos] = v`** — O(log n): walk root→leaf, set the leaf, then
recompute every node on the path back up ("pull").

**Iterative bottom-up form** (cpp/01): leaves at `t[n..2n-1]`, parent of `i`
is `i/2`. Query `[l, r)`: move `l` and `r` up together; when `l` is a right
child (odd) take `t[l]` and step right; when `r` is odd step left and take
`t[r]`. Same O(log n), 2n memory, no recursion.

### 3.2 Lazy propagation (range update)

Adding `v` to `a[l..r]` would touch every leaf in the range: O(n). Instead,
stop at fully-covered nodes like a query does, and leave a **sticky note**:

* `apply(node, v)`: fix the node's own aggregate now
  (`sum += v * len`, `min += v`) and add `v` to `lazy[node]`, meaning
  *"my children still owe +v"*.
* `push(node)`: before going **below** a node, hand its note to both children
  (`apply` each) and clear it.

```
add +3 to [0..7] on 8 zeros:

  [0..7] sum=24 lazy=+3        <- fully covered: stop here, O(1)
    /        \
 [0..3] 0   [4..7] 0           <- stale, but nobody has looked yet

later query sum[2..4] must go below [0..7]  -> push:

  [0..7] sum=24 lazy=0
    /             \
 [0..3] 12 +3   [4..7] 12 +3   <- children fixed, notes moved down one level
```

Golden rule: **every function that recurses into children calls `push`
first**, and **every function that changes children re-pulls after**.

### 3.3 Fenwick tree (Binary Indexed Tree)

`lowbit(i) = i & -i` keeps only the lowest set 1-bit of `i`. Why it works in
two's complement (`-i = ~i + 1`):

```
   i   = 0110 1000
  ~i   = 1001 0111
  ~i+1 = 1001 1000     (the +1 ripples through the trailing 1s of ~i,
                        i.e. the trailing 0s of i, and stops at i's lowest 1)
 i & -i = 0000 1000   = 8
```

`bit[i]` (1-based!) stores the sum of the `lowbit(i)` cells **ending at i**:
`[i - lowbit(i) + 1, i]`.

```
 i  binary lowbit  covers         query prefix(7):            update add(3, d):
 1  0001    1     [1,1]           7 = 0111 -> bit[7]  [7,7]    3 = 0011 -> bit[3]
 2  0010    2     [1,2]           6 = 0110 -> bit[6]  [5,6]    4 = 0100 -> bit[4]
 3  0011    1     [3,3]           4 = 0100 -> bit[4]  [1,4]    8 = 1000 -> bit[8]
 4  0100    4     [1,4]           0  stop                     16 > n  stop
 5  0101    1     [5,5]           i -= lowbit(i)               i += lowbit(i)
 6  0110    2     [5,6]           "strip the lowest 1"         "add the lowest 1"
 7  0111    1     [7,7]
 8  1000    8     [1,8]
```

* **prefix(i)** — strip the lowest bit until 0: each step removes one 1-bit,
  so ≤ log2(n) steps. The visited blocks tile `[1, i]` exactly.
* **add(i, d)** — add the lowest bit until > n: each step moves the lowest 1
  strictly left, so ≤ log2(n) steps. The visited nodes are exactly those whose
  block contains `i`.
* **range(l, r)** = `prefix(r) - prefix(l-1)` (needs an *invertible* op: sum
  or xor yes, min/max **no** — use a segment tree for those).
* **O(n) build**: copy `a` into `bit`, then for each `i` do
  `bit[i + lowbit(i)] += bit[i]`.
* Tricks in cpp/03: range-add + range-sum with two BITs, `find_kth` by binary
  lifting (order statistics), counting inversions.

### 3.4 B-tree

Minimum degree `T` (CLRS): every node except the root has **T-1 .. 2T-1
keys**; an internal node with `k` keys has `k+1` children; **all leaves are at
the same depth**. With `T` in the hundreds, height is
log_T(n): ~3 levels for a billion keys.

**Search** — in each node find the first key ≥ k (binary search); equal →
found; else go to that child. O(log_T n) node reads.

**Insert (top-down, pre-emptive split)** — walk down; *before* stepping into a
full child, split it, so there is always room for the key that moves up:

```
T = 3 (2..5 keys). Child y is full; parent x has room.

 x: [ 40 ]                         x: [ 15  40 ]
     /                                 /   \
 y: [5 10 15 20 25]       --->   y: [5 10]  z: [20 25]
            ^ median moves up
```

If the **root** is full, make a new empty root above it and split the old root
under it. **That is the only way the height grows — at the top — which is why
every leaf stays at the same depth.** (Contrast a BST, which grows at the
bottom and can become lopsided.)

**Delete** (implemented in c/04, explained here) — top-down again: *before
descending into a child that has only T-1 keys, give it one more*, so the
eventual removal can never underflow.

```
case 1  key in a LEAF                -> just remove it (the leaf has >= T keys by the rule)
case 2  key in an INTERNAL node x:
   2a   left child has >= T keys     -> replace key by its predecessor, delete pred below
   2b   right child has >= T keys    -> replace by successor, delete succ below
   2c   both have T-1                -> MERGE left + key + right, recurse into merged node
case 3  key below child c which has only T-1 keys -> top it up first:
   3a   a sibling has >= T keys      -> BORROW through the parent (a rotation):

        x: [ .. 20 .. ]                  x: [ .. 15 .. ]
           /       \          --->          /       \
     [5 10 15]   c:[30 40]            [5 10]    c:[20 30 40]

   3b   both siblings have T-1       -> MERGE c + parent key + sibling
If the root ends up with 0 keys, its single child becomes the root:
the height SHRINKS at the top, mirroring insert.
```

### 3.5 B+ tree

The variant that databases and filesystems actually use:

1. **All records live in the leaves.** Internal nodes hold only *copies* of
   keys (signposts), so an internal page fits more children → even shorter tree.
2. **Leaves are linked** into a sorted list → a range scan is one descent plus
   a sequential walk (perfect for disk read-ahead).
3. Split rules differ: a **leaf** split *copies* the first key of the new right
   leaf up; an **internal** split *moves* the middle key up.

```
                     [ 30 | 60 ]
               /          |          \
        [10 | 20]     [40 | 50]     [70 | 80]
        /  |   \      /  |   \      /  |   \
     [1 5][10 15][20 25][30 35][40 45][50 55][60 65][70 75][80 90]
       -->   -->    -->   -->   -->   -->   -->   -->       leaf chain

 SELECT * WHERE k BETWEEN 12 AND 55  =  descend to [10 15], then walk right.
```

### 3.6 Skip list

Level 0 is a sorted linked list. Each node is promoted to the next lane with
probability p (1/2 here, 1/4 in Redis). Search: start top-left; move right
while the next key is < target; otherwise drop one lane.

```
search 17:
 L2 H ---------------> 9 ------------------------> 21    9 ok; 21 overshoots -> drop
 L1 H -------> 4 ----> 9 -------> 14 ------------> 21    14 ok; 21 overshoots -> drop
 L0 H -> 1 --> 4 -> 7 -> 9 -> 12 -> 14 -> 17 ----> 21    17 found
```

Insert = search while recording `update[i]` (last node < key on each lane),
flip coins for the height, splice into each lane like an ordinary linked list.
Delete = the same, unlinking. **No rotations, no rebalancing** — the coin
flips keep it balanced *in expectation*. That locality (an insert only touches
a few neighbouring pointers) is why lock-free/concurrent ordered maps
(Java `ConcurrentSkipListMap`, LevelDB's memtable) prefer it.

### 3.7 Interval tree (augmented BST)

Store intervals `[lo, hi]` in a balanced BST ordered by `lo`, and **augment**
each node with `maxhi` = the largest `hi` in its subtree.

```
                 [15,20] | 30          (number after | is maxhi)
                /              \
        [5,8] | 10            [17,19] | 30
        /                         \
   [1,10] | 10                  [25,30] | 30

query [12,14]: root no. left.maxhi = 10 < 12  -> nothing on the left can
reach 12, skip the whole subtree. Right side: all lo >= 15 > 14, skip. none.
```

*Find any overlap* walks one path: go left if `left.maxhi >= a`, else right.
*Report all* prunes subtrees with `maxhi < a`, and right subtrees when
`node.lo > b`: O(log n + k). The crucial implementation detail: **every
rotation must recompute `maxhi` for the two rotated nodes (lower one first)** —
the Linux kernel's augmented rbtree passes exactly such a callback.

### 3.8 Which tree do I choose?

| Need | Choose | Why not the others |
|---|---|---|
| Ordered set / map in RAM | red-black tree (`std::map`, kernel `rbtree`) or AVL | B-tree wins only if cache misses dominate; skip list if you need concurrency |
| Ordered map on disk / SSD / flash | **B+ tree** | binary trees do one I/O per level — 30 I/Os instead of 3 |
| Write-heavy store (logs, KV) | LSM tree (memtable = **skip list**) + sorted files | B+ tree random writes are expensive on flash |
| Range sum / xor with point updates | **Fenwick** | smallest code and memory; segment tree also works |
| Range min / max / gcd, or any monoid | **segment tree** | Fenwick needs an invertible op |
| Range *updates* + range queries | **lazy segment tree** (or 2 Fenwicks for add+sum) | |
| Static array, range min, no updates | sparse table (O(1) query) | segment tree is fine too |
| Overlapping intervals / time ranges | **interval tree** (augmented rbtree) | segment tree needs coordinates known in advance |
| Integer keys, prefix / range by bits | radix tree / trie / xarray ([11-tries](../11-tries/README.md)) | comparisons wasted when keys are bit strings |
| Priority only (min/max out) | heap ([10](../10-heaps-priority-queues/README.md)) | a full ordered tree is overkill |
| Concurrent ordered map | skip list | rebalancing rotations need wide locks |

---

## 4. Complexity table

| Structure | Build | Query | Update | Space | Why |
|---|---|---|---|---|---|
| Segment tree | O(n) | O(log n) range | O(log n) point | 4n (rec) / 2n (iter) | ≤ 2 partial nodes per level; one root-leaf path for update |
| Lazy segment tree | O(n) | O(log n) | O(log n) range | 8n (value + tag) | full-cover nodes stop early and leave a tag |
| Fenwick tree | O(n) | O(log n) prefix | O(log n) point | n+1 | one bit changes per loop step |
| B-tree (degree T) | — | O(log_T n) node reads, O(log n) compares | O(T log_T n) | O(n) | height log_T n; O(T) shifting inside a node |
| B+ tree | O(n) bulk-load | O(log_B n), range O(log_B n + k) | O(B log_B n) | O(n) | leaves linked |
| Skip list | — | O(log n) expected | O(log n) expected | ~2n pointers (p=1/2) | ~log2 n lanes, ~2 steps per lane; worst case O(n) (astronomically unlikely) |
| Interval tree | — | any: O(log n); all: O(log n + k) | O(log n) | O(n) | balanced BST + O(1) augmentation per rotation |

---

## 5. C vs C++

| | C (`c/`) | C++ (`cpp/`) |
|---|---|---|
| Segment tree | recursive, `calloc` 4n, explicit `seg_free` | `SegmentTree<T, Op>` over any monoid, bottom-up, `std::vector` storage |
| Lazy segtree | range add + sum | range add + sum **and** min in one node struct |
| Fenwick | `unsigned` lowbit to avoid signed overflow UB | `Fenwick<T>`, range-add trick, `find_kth`, inversions |
| B-tree | fixed arrays in node, `memmove`, **insert + delete** | `BTree<K, V, T>`, `std::vector` + `std::unique_ptr` children, `std::lower_bound` |
| B+ tree | — | `BPlusTree<K, V, M>`, leaf chain is non-owning `Node*` |
| Skip list | flexible array member `next[]` (one malloc per node) | level 0 owns via `unique_ptr`, iterative destructor |
| Interval tree | treap + `maxhi` augmentation | use the C version; in C++ you would pair a balanced tree with the same `pull` |

**Standard library equivalents.** There is *no* segment tree, Fenwick, B-tree
or skip list in the C++ standard library. `std::map`/`std::set` are red-black
trees; `std::partial_sum` gives static prefix sums. GNU libstdc++ ships
`__gnu_pbds::tree` (an order-statistics rbtree) and Abseil ships
`absl::btree_map` (a cache-friendly in-memory B-tree, a drop-in for
`std::map`). In C, glibc offers only `tsearch` (a red-black tree).

---

## 6. Where it is used in the real world

* **Linux kernel**
  * `include/linux/interval_tree_generic.h`, `lib/interval_tree.c`: augmented
    rbtree; used for VMA reverse mappings (`mm/interval_tree.c` — "which
    processes map this file page?") and DRM GPU address-space management.
  * `lib/maple_tree.c`: since 6.1 the per-process VMA set is a **maple tree**,
    a B-tree variant tuned for ranges and RCU-safe lockless reads (replaced the
    rbtree + linked list).
  * `lib/radix-tree.c` / `lib/xarray.c`: page cache index (see 11-tries).
  * Filesystems: **ext4** directory index (htree, a B-tree-like hash tree) and
    extent tree, **XFS** and **Btrfs** (B-trees everywhere — Btrfs is literally
    "B-tree FS"), **F2FS** node tree. On your BeagleBone eMMC/SD card the ext4
    root filesystem resolves paths through these.
* **Databases**: InnoDB clustered indexes, PostgreSQL `nbtree`, SQLite tables
  and indexes — all **B+ trees** sized to one page (4–16 KiB).
  **Redis** sorted sets (`ZADD`/`ZRANGE`) = skip list + hash table.
  **LevelDB / RocksDB** memtable = skip list (concurrent inserts, ordered
  flush to SSTables).
* **Networking**: interval/segment trees for firewall port ranges and TCP
  out-of-order segment reassembly (Linux keeps out-of-order skbs in an rbtree).
* **Multimedia / GStreamer**: a timeline editor (GES) answers "which clips
  overlap time t?" — an interval query. Buffer-pool and timestamp bookkeeping
  often needs "sum of bytes queued in [t1, t2]" — Fenwick territory.
* **Competitive programming / analytics**: segment trees and BITs for
  range statistics, inversion counts, order statistics.
* **Embedded**: a Fenwick tree is ~15 lines and an array — ideal for running
  histograms or percentiles (e.g. latency of video frames on a TDA4VM
  pipeline) without heap allocation.

---

## 7. Common mistakes and pitfalls

1. **Segment tree array too small** — `2n` is not enough for the *recursive*
   layout when n is not a power of two. Use `4n`.
2. **Forgetting `push` in a query** in a lazy tree — answers are silently
   stale. Every path that descends must push first.
3. **Wrong identity**: returning 0 for a *min* query on a disjoint node.
   Identity of min is +∞, of max is −∞, of gcd is 0, of product is 1.
4. **`apply` that ignores length**: for range-add + sum, the node's sum grows
   by `v * (hi - lo + 1)`, not `v`.
5. **Fenwick with index 0**: `i & -i` is 0 → `add(0)` loops forever. Fenwick
   is **1-based**; shift your input by one.
6. **Fenwick for min/max ranges** — `prefix(r) - prefix(l-1)` needs an inverse.
   Min has none.
7. **`i & -i` on a signed `int`** with `INT_MIN` is overflow UB; use unsigned.
8. **B-tree split on the way up without room** — the top-down version only
   works if you split *before* entering a full child.
9. **B+ tree: moving instead of copying** the separator on a leaf split
   (the key disappears from the leaves and the range scan misses it).
10. **Skip-list destructor via recursive `unique_ptr` chain** — a 1M-element
    list overflows the stack. Tear down iteratively (cpp/06).
11. **Skip list with a fixed `MAX_LEVEL` too low** — degrades toward O(n)
    once n ≫ 2^MAX_LEVEL.
12. **Interval tree: forgetting to recompute `maxhi` after rotations** or
    recomputing the upper node before the lower one.
13. **Mid overflow**: `(lo + hi) / 2` on large indices; use `lo + (hi - lo) / 2`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_segment_tree.c](c/01_segment_tree.c) | recursive segment tree: range sum + range min, point update | `make run P=dsa/12-advanced-trees/c/01_segment_tree.c` |
| [c/02_segment_tree_lazy.c](c/02_segment_tree_lazy.c) | lazy propagation: range add, range sum | `make run P=dsa/12-advanced-trees/c/02_segment_tree_lazy.c` |
| [c/03_fenwick_tree.c](c/03_fenwick_tree.c) | Fenwick tree, lowbit walks printed in binary, O(n) build | `make run P=dsa/12-advanced-trees/c/03_fenwick_tree.c` |
| [c/04_btree.c](c/04_btree.c) | B-tree search, insert with pre-emptive split, **full delete** (borrow / merge), invariant checker | `make run P=dsa/12-advanced-trees/c/04_btree.c` |
| [c/05_skip_list.c](c/05_skip_list.c) | skip list insert / search / erase, lanes printed | `make run P=dsa/12-advanced-trees/c/05_skip_list.c` |
| [c/06_interval_tree.c](c/06_interval_tree.c) | interval tree = treap + `maxhi` augmentation; any / all overlaps | `make run P=dsa/12-advanced-trees/c/06_interval_tree.c` |
| [cpp/01_segment_tree.cpp](cpp/01_segment_tree.cpp) | generic bottom-up `SegmentTree<T, Op>` for sum / min / gcd | `make run P=dsa/12-advanced-trees/cpp/01_segment_tree.cpp` |
| [cpp/02_lazy_segment_tree.cpp](cpp/02_lazy_segment_tree.cpp) | lazy tree with range add, range sum **and** range min | `make run P=dsa/12-advanced-trees/cpp/02_lazy_segment_tree.cpp` |
| [cpp/03_fenwick_tree.cpp](cpp/03_fenwick_tree.cpp) | `Fenwick<T>`, range-add/range-sum with two BITs, `find_kth`, inversions | `make run P=dsa/12-advanced-trees/cpp/03_fenwick_tree.cpp` |
| [cpp/04_btree.cpp](cpp/04_btree.cpp) | `BTree<K, V, T>` with `unique_ptr` children, range visit, vs `std::map` | `make run P=dsa/12-advanced-trees/cpp/04_btree.cpp` |
| [cpp/05_bplus_tree.cpp](cpp/05_bplus_tree.cpp) | B+ tree insert, lookup, range scan along the leaf chain | `make run P=dsa/12-advanced-trees/cpp/05_bplus_tree.cpp` |
| [cpp/06_skip_list.cpp](cpp/06_skip_list.cpp) | `SkipList<K>` with owning lane 0, `lower_bound`, iterative destructor, vs `std::set` | `make run P=dsa/12-advanced-trees/cpp/06_skip_list.cpp` |

Every program checks itself with thousands of random operations against a
brute-force reference (plain array, `std::map`, `std::set`).

---

## 9. Exercises

**Easy**
1. Change c/01 to also support range **max**. *Hint:* identity is `INT_MIN`.
2. In c/03, print which `bit[]` cells `prefix(13)` reads. Predict first in
   binary (1101 → 1100 → 1000 → 0).
3. In c/04 set `T 2` (a 2-3-4 tree) and watch the printed tree get taller.

**Medium**
4. Segment tree **"first index ≥ x"**: descend into the left child if its max
   is ≥ x, else right. O(log n).
5. Lazy segment tree with **range assign** (`a[l..r] = v`) instead of add.
   *Hint:* the tag is "assign v or nothing"; assign overrides older adds.
6. 2-D Fenwick: `bit[i][j]`, two nested lowbit loops; sum of a sub-rectangle.
7. Skip list **rank**: store on every forward pointer how many level-0 nodes it
   jumps (the "span"); this is exactly how Redis answers `ZRANK` in O(log n).

**Hard**
8. Add `erase` to cpp/05 (B+ tree): borrow from / merge with a sibling leaf,
   fix the parent separator, keep the leaf chain intact.
9. Persistent segment tree: on update, copy only the O(log n) nodes on the
   path; keep every version's root. Answer "k-th smallest in a[l..r]".
10. Build an interval tree on an **AVL** tree (reuse 09-trees) instead of a
    treap; the only change is calling `pull` inside each rotation.

---

## 10. Remember forever

Review this card on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

```
SEGMENT TREE : node = combine(slice). query: disjoint->identity, inside->take, else split.
               ≤2 partial nodes per level => O(log n). Array 4n, children 2i, 2i+1.
LAZY         : apply = fix node now + tag children later. push before descending.
FENWICK      : lowbit = i & -i. prefix: i -= lowbit. update: i += lowbit. 1-based!
               bit[i] covers (i - lowbit(i), i].  Needs an inverse for ranges.
B-TREE       : T-1..2T-1 keys, all leaves same depth, split full child BEFORE entering;
               grows/shrinks only at the ROOT. Height log_T n = disk reads.
B+ TREE      : data only in linked leaves; leaf split COPIES key up, internal MOVES.
SKIP LIST    : coin-flip lanes; right while next < key, else down. No rotations.
INTERVAL     : BST by lo + maxhi; go left iff left.maxhi >= a. Re-pull on rotation.
```

---

## 11. Self-test questions

1. Why does a segment-tree query touch at most ~4 nodes per level?
2. What exactly does `lazy[node]` mean: is the node's own value already
   updated or not? What about its children?
3. Write `i & -i` for i = 12 in binary step by step. Which cells does
   `prefix(12)` read, and which does `add(5)` update for n = 16?
4. Why can't a Fenwick tree answer range-min queries over arbitrary `[l, r]`?
5. A B-tree with T = 256 holds 10⁹ keys. Roughly how many node reads per
   lookup? How many for a balanced binary tree?
6. Why does a B-tree only grow at the root, and why does that guarantee
   equal leaf depth?
7. In B-tree deletion, why do we top up a child to T keys *before* descending?
8. Name two differences between a B-tree and a B+ tree, and why a database
   prefers the latter for `BETWEEN` queries.
9. Why are skip lists attractive for concurrent data structures?
10. In an interval tree, prove that if `left.maxhi >= a` and nothing in the
    left subtree overlaps `[a, b]`, then nothing in the right subtree does
    either.
11. Which structure would you pick for: (a) an SQL index, (b) "sum of bytes
    received in the last N seconds, updated per packet", (c) Linux VMAs,
    (d) Redis leaderboards?
