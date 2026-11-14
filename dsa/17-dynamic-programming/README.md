# 17 — Dynamic Programming

> **One sentence to keep:** DP is recursion that never solves the same
> subproblem twice: name the subproblem, write the recurrence, then either
> cache it (top-down) or fill a table in dependency order (bottom-up).

---

## 1. Why this exists

Many problems ask for the *best* or the *number of ways* among an astronomical
number of choices: which items go in the knapsack, how to align two strings,
which order to visit cities in. Trying every choice is exponential.

The good news is that these choices are made of **smaller versions of the
same problem**, and the smaller versions **repeat**. "Best way to pay 11 with
coins {1,2,5}" needs "best way to pay 10", "9" and "6", and those need "best
way to pay 8, 5, 4..." again and again. Solve each smaller version once, write
it down, look it up afterwards. Exponential becomes polynomial.

```
naive fib(40):  331,160,281 calls        memoised fib(40): 79 calls
```

That is the whole idea. Everything else in this chapter is about *finding* the
subproblem, and filling the table in the right order.

---

## 2. Mental model

**The notebook analogy.** You are doing a long calculation by hand. Each
time you finish a sub-calculation you write the result in a notebook, indexed
by the question it answers ("cheapest way to pay 7 = 2 coins"). Before
starting any sub-calculation, you look in the notebook first. The notebook is
the DP table; the index is the **state**.

**The DAG picture.** Every DP is a shortest/longest/counting path problem in
a directed acyclic graph of states. Each state points to the smaller states
it depends on. Fill the states in *topological order* (dependencies first)
and every state is computed exactly once.

```
 Fibonacci as a recursion TREE (exponential)     ...as a DAG of states (linear)

               f5                                  f5 -> f4 -> f3 -> f2 -> f1
           /        \                               \     \     \     \
         f4          f3                              +---->+---->+---->+--> f0
        /  \        /  \                          (each node points to the two
      f3    f2    f2    f1                         below it; n+1 nodes total)
     /  \   / \   / \
   f2  f1 f1 f0 f1 f0                              fill order: f0, f1, f2, ... f5
   / \
 f1  f0             15 nodes, but only 6 distinct
```

Recognise DP when **both** properties hold:

| Property | Meaning | Test question |
|---|---|---|
| **Optimal substructure** | The best answer is built from best answers to subproblems | "If I fix the last decision, is the rest an independent smaller instance of the same problem?" |
| **Overlapping subproblems** | The recursion reaches the same subproblem many times | "Draw the recursion tree for a small input. Do nodes repeat?" |

If subproblems don't overlap (merge sort: each half is different), it is
plain divide and conquer, and caching is useless. If the "best" answer is not
made of best sub-answers (longest *simple* path in a general graph), DP does
not apply directly.

Typical phrasing that smells like DP: *minimum/maximum ...*, *number of
ways ...*, *is it possible to ...*, *longest/shortest ... subsequence*, and
the input is a sequence, a string, a grid, a tree, a capacity, or a small set.

---

## 3. How it works

### 3.1 The 5-step method (use it every time)

1. **State.** Define `dp[...]` in one precise English sentence.
   *"dp[i][w] = best value using only the first i items with capacity w."*
   If you cannot say it in a sentence, you do not have a state yet.
2. **Choice / transition.** Look at the *last* decision (last item, last
   character, last coin, last step). List the options; each option leads to a
   smaller state. Write the recurrence.
3. **Base cases.** The smallest states whose answers are obvious (empty
   prefix, zero capacity, amount 0). Also decide what "impossible" is (INF,
   -1, 0 ways).
4. **Order.** Fill so that every state is computed after the states it reads.
   1-D: left to right or right to left. 2-D prefix DPs: row by row.
   Interval DPs: by increasing length. Bitmasks: by increasing mask.
5. **Answer & optimise.** Where does the answer live (`dp[n]`, `dp[n][W]`,
   `max over i`)? Then: can you keep fewer rows? Do you need to reconstruct
   the actual choices (keep a parent table, or walk the table back)?

### 3.2 Top-down vs bottom-up vs space-optimised

```
              top-down (memo)              bottom-up (table)           space-optimised
code shape    recursion + cache            loops in dependency order   loops + rolling vars/rows
order         found automatically          you must work it out        same as bottom-up
computes      only reachable states        every state                 every state
overhead      call stack, cache lookups    none                        none
stack risk    deep recursion can overflow  no                          no
easy to       write from the recurrence    optimise space              -- (that's the point)
```

Write top-down first when you are still thinking; convert to bottom-up when
it works, then roll the array. [`c/01_fibonacci_three_ways.c`](c/01_fibonacci_three_ways.c)
does all four versions side by side and prints the table filling:

```
  i= 2  dp: 0 1 [1] . . . . . . . .
  i= 3  dp: 0 1 1 [2] . . . . . . .
  i= 4  dp: 0 1 1 2 [3] . . . . . .
  ...
  i=10  dp: 0 1 1 2 3 5 8 13 21 34 [55]
```

Space rule: **if `dp[i]` reads only rows `i-1` (and `i-2`), keep only those.**
Fibonacci needs 2 variables; knapsack needs one row; LCS needs two rows.

### 3.3 Fibonacci and climbing stairs

Climbing `n` stairs with steps of 1 or 2: the *last* step was 1 (from `n-1`)
or 2 (from `n-2`), so `ways(n) = ways(n-1) + ways(n-2)`, with `ways(0) = 1`.
That *is* Fibonacci, shifted: `ways(n) = fib(n+1)`. For a general step set
`S`, `ways(n) = Σ ways(n-s)` for `s ∈ S`.

### 3.4 Coin change

**Min coins.** `dp[a]` = fewest coins summing to `a`.
`dp[a] = 1 + min(dp[a-c])` over coins `c ≤ a`, `dp[0] = 0`, INF if
unreachable. Coins {1,2,5}, amount 11, filled cell by cell:

```
 a :  0  1  2  3  4  5  6  7  8  9 10 11
dp :  0  1  1  2  2  1  2  2  3  3  2  3      dp[11] = dp[10]+1 = (5+5)+1
```

**Greedy fails** for non-canonical coin systems: coins {1,3,4}, amount 6.
Greedy takes 4+1+1 (3 coins); DP finds 3+3 (2 coins). DP is always right.

**Number of ways.** Loop **coins outside, amounts inside**:

```
coins {1,2,5}, amount 5      a: 0 1 2 3 4 5
start                           1 0 0 0 0 0
after coin 1                    1 1 1 1 1 1
after coin 2                    1 1 2 2 3 3
after coin 5                    1 1 2 2 3 4     -> 4 ways
```

Swap the loops (amounts outside) and you count *orderings*: 1+2 and 2+1
become different, giving 9 instead of 4. Same code, different problem. This
is the single most common DP bug.

### 3.5 0/1 and unbounded knapsack

Items `(weight, value)`, capacity `W`. 0/1: each item at most once.
`dp[i][w] = max(dp[i-1][w], dp[i-1][w-wt_i] + val_i)` (skip or take).

```
items (wt,val): (1,1) (3,4) (4,5) (5,7),  W = 7
capacity w:     0  1  2  3  4  5  6  7
row 1 (1,1):    0  1  1  1  1  1  1  1
row 2 (3,4):    0  1  1  4  5  5  5  5
row 3 (4,5):    0  1  1  4  5  6  6  9   <- 9 = row2[3] + 5  (take item 3 on top of item 2)
row 4 (5,7):    0  1  1  4  5  7  8  9
```

**Reconstruct**: walk from `dp[n][W]` upward; if `dp[i][w] != dp[i-1][w]`,
item `i` was taken, so `w -= wt_i`.

**One row is enough**, and the loop direction is the entire difference
between the two problems:

```
0/1        for w = W down to wt:  dp[w] = max(dp[w], dp[w-wt] + val)
unbounded  for w = wt up to W:    dp[w] = max(dp[w], dp[w-wt] + val)

why: going UP, dp[w-wt] has already been updated with this item, so the item
gets added again (reuse). Going DOWN, dp[w-wt] is still last row's value.
```

Bounded (item `i` at most `c_i` times): split `c` into 0/1 pieces of size
1, 2, 4, ..., rest — see [`cpp/02_knapsack_and_coins.cpp`](cpp/02_knapsack_and_coins.cpp).

### 3.6 Longest increasing subsequence

**O(n²):** `dp[i]` = length of the LIS **ending at** `i`.
`dp[i] = 1 + max(dp[j])` over `j < i` with `a[j] < a[i]`. Answer `max(dp)`.
"Ending at" is the trick: it pins down the last value, so we know what may
precede it.

```
a     : 10  9  2  5  3  7 101 18  4 19
dp    :  1  1  1  2  2  3   4  4  3  5
parent: -1 -1 -1  2  2  3   5  5  4  7      -> 2 5 7 18 19
```

**O(n log n), patience sorting:** `tails[L]` = smallest tail of any
increasing subsequence of length `L+1`. It is sorted, so binary-search each
element: replace the first tail `>= x` or append.

```
x= 10 append   tails: 10
x=  9 replace  tails: 9
x=  2 replace  tails: 2
x=  5 append   tails: 2 5
x=  3 replace  tails: 2 3
x=  7 append   tails: 2 3 7
x=101 append   tails: 2 3 7 101
x= 18 replace  tails: 2 3 7 18
x=  4 replace  tails: 2 3 4 18     <- NOT a real subsequence (4 comes after 18)!
x= 19 append   tails: 2 3 4 18 19  -> length 5 is right; the contents are not
```

`tails` gives the right *length*; to recover the sequence store indices and a
parent per element ([`c/04_lis.c`](c/04_lis.c)). Use `lower_bound` for
strictly increasing, `upper_bound` for non-decreasing.

### 3.7 LCS (longest common subsequence)

`dp[i][j]` = LCS of prefixes `a[0..i)` and `b[0..j)`.
Match: `dp[i-1][j-1] + 1`. Else: `max(dp[i-1][j], dp[i][j-1])`.

```
         -  B  D  C  A  B  A
    -    0  0  0  0  0  0  0
    A    0  0  0  0  1  1  1          each cell looks at:
    B    0  1  1  1  1  2  2             diag  up
    C    0  1  1  2  2  2  2                 \  |
    B    0  1  1  2  2  3  3            left -- [cell]
    D    0  1  2  2  2  3  3
    A    0  1  2  2  3  3  4          walk back from the corner:
    B    0  1  2  2  3  4  4          diagonal on a match emits a char -> "BCBA"
```

`diff` is LCS over **lines**: lines in the LCS are unchanged, the rest are
`-` (only in old) or `+` (only in new) — [`cpp/03_sequence_dp.cpp`](cpp/03_sequence_dp.cpp)
prints a real diff. (GNU diff and git use Myers' O(ND) algorithm, which finds
the same shortest edit script faster when the files are similar.)

### 3.8 Edit distance (Levenshtein)

`dp[i][j]` = min edits turning `a[0..i)` into `b[0..j)`. Base: `dp[i][0] = i`
(delete everything), `dp[0][j] = j` (insert everything).

```
match:    dp[i-1][j-1]
else: 1 + min( dp[i-1][j-1]   replace
               dp[i-1][j]     delete a[i-1]
               dp[i][j-1] )   insert b[j-1]

         -  s  i  t  t  i  n  g
    -    0  1  2  3  4  5  6  7
    k    1  1  2  3  4  5  6  7       kitten -> sitting = 3
    i    2  2  1  2  3  4  5  6         replace k -> s
    t    3  3  2  1  2  3  4  5         replace e -> i
    t    4  4  3  2  1  2  3  4         insert  g
    e    5  5  4  3  2  2  3  4
    n    6  6  5  4  3  3  2  3
```

Used by spell checkers ("did you mean"), `git`'s "did you mean this
command?", fuzzy search, and DNA/protein alignment (Needleman–Wunsch and
Smith–Waterman are the same table with scores instead of unit costs).

### 3.9 Matrix chain multiplication (interval DP)

`A1(10×30) A2(30×5) A3(5×60)`: `(A1A2)A3` costs 1500 + 3000 = 4500, but
`A1(A2A3)` costs 9000 + 18000 = 27000. Same result, 6× the work.

`dp[i][j]` = min cost for `Ai..Aj`;
`dp[i][j] = min over k of dp[i][k] + dp[k+1][j] + d[i-1]·d[k]·d[j]`.

Fill by **chain length** (diagonals), since a range only reads shorter ranges:

```
CLRS chain 30,35,15,5,10,20,25:
     0  15750   7875   9375  11875  15125     <- answer, top-right
     .      0   2625   4375   7125  10500
     .      .      0    750   2500   5375     filled diagonal by diagonal,
     .      .      .      0   1000   3500     main diagonal first (len 1 = 0)
     .      .      .      .      0   5000
     .      .      .      .      .      0     -> ((A1(A2A3))((A4A5)A6))
```

O(n³). Same pattern: optimal BST, polygon triangulation, burst balloons,
and the CYK parsing algorithm in compilers.

### 3.10 Longest palindromic subsequence

`dp[i][j]` for `s[i..j]`: ends equal → `2 + dp[i+1][j-1]`; else
`max(dp[i+1][j], dp[i][j-1])`. Also by length. Fun fact checked in the code:
`LPS(s) = LCS(s, reverse(s))`.

### 3.11 Grid paths

A robot moving only right/down reaches `(r,c)` from above or from the left:
`dp[r][c] = dp[r-1][c] + dp[r][c-1]`, 0 on obstacles.

```
grid           paths per cell
. . . .         1  1  1  1
. # . .         1  #  1  2
. . . #         1  1  2  #
# . . .         #  1  3  3     -> 3 paths
```

Min path sum is the same with `cost + min(up, left)`, and fits in one row.

### 3.12 Partition problems (subset sum)

`can[s]` = some subset sums to `s`. 0/1 knapsack with booleans, loop `s` down.

```
items {3,1,5}      s: 0 1 2 3 4 5 6 7 8 9
start                 1 . . . . . . . . .
+3                    1 . . 1 . . . . . .
+1                    1 1 . 1 1 . . . . .
+5                    1 1 . 1 1 1 1 . 1 1
```

* Equal partition: total even and `can[total/2]`.
* Min-difference partition: largest reachable `s ≤ total/2`, answer `total - 2s`.
* Count subsets with sum `S`: replace OR by `+`.
* Partition into **k** equal groups: bitmask DP (3.13).

These are *pseudo-polynomial*: O(n·S) is great for small sums, useless for
sums like 10¹⁸ (subset sum is NP-complete in general).

### 3.13 Bitmask DP

When the state is "which of n ≤ ~20 things have been used", store the set
as bits of an integer.

```
mask = 0b1011  ->  items {0, 1, 3} used
add i     : mask | (1 << i)          test i : (mask >> i) & 1
remove i  : mask & ~(1 << i)          size   : popcount(mask)
all submasks of m:  for (s = m; s; s = (s - 1) & m)   -- O(3^n) summed over all m
```

**TSP (Held–Karp):** `dp[mask][v]` = shortest path from city 0 visiting
exactly `mask`, ending at `v`. The order *within* `mask` no longer matters,
only the set and the endpoint: n! orderings collapse into 2ⁿ·n states.
O(2ⁿ·n²): n = 15 takes milliseconds versus 14! ≈ 87 billion tours.

**Assignment:** `best[mask]` = min cost when the first `popcount(mask)`
workers got the jobs in `mask`. O(2ⁿ·n).

### 3.14 DP on trees

Root the tree; a node's answer combines its **children's** answers, so
compute in post-order. Often a node needs two values:

```
max-weight independent set:
  take[v] = w[v] + Σ skip[c]          (children cannot be taken)
  skip[v] =        Σ max(take[c], skip[c])

          (0) w=3  [11|10]               [take|skip]
         /      \
   (1) w=4 [4|2]  (2) w=5 [5|6]
     /  \            \
 (3)1   (4)1         (5)6               best = 11 = {0,3,4,5}
```

**Rerooting** gets an answer *for every node as root* in O(n): pass 1
(post-order) computes subtree values; pass 2 (pre-order) moves the root from
parent `p` to child `c`. For "sum of distances":
`ans[c] = ans[p] - size[c] + (n - size[c])` (c's subtree gets 1 closer,
everyone else 1 farther). See [`cpp/06_tree_dp_rerooting.cpp`](cpp/06_tree_dp_rerooting.cpp).

Implementation tip: for deep trees use an explicit stack (or a
parent-before-child array) instead of recursion.

### 3.15 Pattern catalogue

When you meet a new problem, find its shape here; the state follows.

| Pattern | State | Transition shape | Examples | Program |
|---|---|---|---|---|
| Linear, look back k | `dp[i]` = answer for first i / ending at i | `dp[i] = f(dp[i-1], dp[i-2], ...)` | Fibonacci, stairs, house robber, decode ways | c/01, cpp/01 |
| "Ending at i" | `dp[i]` = best that ends exactly at i | extend previous or start fresh | Kadane, LIS O(n²) | c/04, cpp/07 |
| Unbounded choice | `dp[x]` over capacity/amount | loop capacity **up** | coin change, unbounded knapsack, rod cutting | c/02, c/03 |
| 0/1 choice | `dp[i][x]` or 1-D with **down** loop | skip or take item i | 0/1 knapsack, subset sum, partition, target sum | c/03, c/07 |
| Two sequences | `dp[i][j]` on prefixes | diag on match, else up/left | LCS, edit distance, diff, regex/wildcard match | c/05, cpp/03 |
| Interval | `dp[i][j]` on a range | split point k, fill by length | matrix chain, LPS, palindrome cuts, burst balloons | c/06, cpp/04 |
| Grid | `dp[r][c]` | from up/left | unique paths, min path sum, dungeon game | c/07 |
| Segmentation | `ok[i]` for prefix | try last piece `s[j..i)` | word break, palindrome partitioning | cpp/07, cpp/04 |
| Counting structures | `T[n]` | sum over root/split of products | Catalan: BSTs, brackets, triangulations | cpp/07 |
| State machine | best per state per step | transitions between few states | stock with cooldown/fee, regex NFA | cpp/07 |
| Bitmask | `dp[mask]` / `dp[mask][v]` | add one element / submask | TSP, assignment, k-partition, bin packing | c/08, cpp/05 |
| Tree | `dp[v][...]` | combine children (post-order); reroot | MIS, vertex cover, diameter, sum of distances | c/09, cpp/06 |
| Digit | `(pos, carry-state, tight)` | choose next digit ≤ limit when tight | count numbers in [0,N] with a digit property | cpp/07 |

---

## 4. Complexity table

The rule: **time = (number of states) × (work per state)**,
**space = number of states you must keep alive**.

| Problem | States | Work/state | Time | Space (optimised) | Why |
|---|---|---|---|---|---|
| Fibonacci / stairs | n | O(1) (or k steps) | O(n) / O(nk) | O(1) / O(k) | reads 2 (k) previous cells |
| Coin change (min / ways) | A | k coins | O(A·k) | O(A) | one cell per amount |
| 0/1 knapsack | n·W | O(1) | O(n·W) | O(W) | downward 1-D loop |
| Unbounded knapsack | n·W | O(1) | O(n·W) | O(W) | upward 1-D loop |
| LIS | n | O(n) / O(log n) | O(n²) / O(n log n) | O(n) | binary search on tails |
| LCS, edit distance | n·m | O(1) | O(n·m) | O(min(n,m)) | two rows (or one + diag) |
| Matrix chain | n² | O(n) splits | O(n³) | O(n²) | every range × every split |
| LPS | n² | O(1) | O(n²) | O(n) | by length, two diagonals |
| Grid paths | R·C | O(1) | O(R·C) | O(C) | one row |
| Subset sum / partition | n·S | O(1) | O(n·S) | O(S) | pseudo-polynomial |
| TSP Held–Karp | 2ⁿ·n | O(n) | O(2ⁿ·n²) | O(2ⁿ·n) | vs O(n!) brute force |
| Submask DP | Σ 2^\|m\| | — | O(3ⁿ) | O(2ⁿ) | each element: out / in m / in sub |
| Tree DP / rerooting | n | O(deg) | O(n) | O(n) | each edge seen twice |
| Digit DP | digits·states·2 | 10 | O(L·S·10) | O(L·S) | L = number of digits |

---

## 5. C vs C++

| | C | C++ |
|---|---|---|
| Table | fixed-size arrays or `static int dp[N][M]` (big tables must not live on the stack); `calloc` for runtime sizes | `std::vector<std::vector<T>>` or one flat `std::vector<T>` indexed `i*m + j` (faster, one allocation) |
| "Impossible" | `INF = INT_MAX/2` so `INF + 1` does not overflow | same, or `std::optional<int>` for the *answer* |
| Memoisation | array + `known[]` flags, or a sentinel like `-1` | `std::map` / `std::unordered_map`, a generic `Memo<Key,Value>` wrapper, or a recursive lambda via `std::function` |
| Binary search (LIS) | hand-written `lower_bound` loop | `std::lower_bound` / `std::upper_bound` |
| Bit tricks | `__builtin_popcount`, `__builtin_ctz` (GCC/Clang) | same, or C++20 `std::popcount`, `std::countr_zero` |
| Permutation brute force | hand recursion | `std::next_permutation` |

There is no "DP library" in either standard library; DP is a technique, not
a container. What you reuse is the *pattern*.

---

## 6. Where it is used in the real world

* **diff / patch / git**: LCS-style edit scripts between file versions
  (git uses Myers' algorithm, a shortest-edit-script search over the same
  edit graph; `git diff --patience` adds an LIS step on unique lines).
* **Spell checkers, fuzzy finders, `git help` suggestions**: edit distance.
* **Bioinformatics**: Needleman–Wunsch (global) and Smith–Waterman (local)
  alignment are edit-distance tables with scoring matrices; BLAST uses
  heuristics on top of them.
* **Speech recognition and decoders**: the **Viterbi** algorithm is DP over
  (time, hidden state). The same algorithm decodes convolutional codes in
  Wi-Fi, LTE and satellite links.
* **Video codecs** (H.264/HEVC encoders, including hardware ones on SoCs like
  the TDA4VM): rate–distortion optimisation and trellis quantisation choose
  coefficients with a Viterbi-style DP; mode decision is a cost minimisation
  over a tree of block partitions.
* **Compilers**: instruction selection by tree tiling (DP on the expression
  tree, as in BURG-style selectors), CYK parsing (interval DP), optimal
  register spilling in some allocators.
* **Databases**: query optimisers (System R and PostgreSQL's standard
  planner) choose join orders with DP over subsets of tables — bitmask DP.
  PostgreSQL switches to a genetic optimiser (GEQO) when there are too many
  tables, exactly because 2ⁿ grows too fast.
* **Networking**: Bellman–Ford (RIP distance-vector routing) is DP over
  "shortest path with at most k edges".
* **Text layout**: TeX's line breaking (Knuth–Plass) minimises total
  "badness" over a paragraph with DP, instead of greedily filling lines.
* **Embedded / real-time**: knapsack-style selection of which tasks or
  features fit a CPU or memory budget; optimal scheduling of DMA transfers;
  resource allocation in schedulability analysis.
* **Image processing**: seam carving (content-aware resize) is grid-path DP
  over pixel energy; stereo matching does DP along scanlines.

---

## 7. Common mistakes and pitfalls

1. **Vague state.** "dp[i] = the answer for i" is not a state. Say exactly
   what subproblem it is, including "ending at i" vs "among the first i".
2. **Wrong loop order for counting.** Coins outside = combinations; amounts
   outside = permutations. Decide which one the problem asks for.
3. **Wrong direction in 1-D knapsack.** 0/1 must go **down**; going up
   silently turns it into unbounded.
4. **Filling in the wrong order.** Interval DP by `i` then `j` reads
   unfilled cells. Fill by length, or go `i` from n-1 down to 0 and `j` up.
5. **INF overflow.** `INT_MAX + 1` is UB and wraps negative. Use
   `INT_MAX / 2` or check before adding.
6. **Off-by-one in prefix DPs.** `dp` has `n+1` rows; `dp[i]` talks about
   `a[i-1]`. Mixing the two indexings is the classic bug.
7. **Uninitialised or wrongly initialised base cases.** Min problems start at
   INF (except the base), max problems at 0 or -INF, counting at 0 with
   `dp[0] = 1` (the empty way).
8. **Memo keyed on the wrong thing.** If the result depends on a parameter
   that is not in the key, the cache returns wrong answers. The key must be
   the full state.
9. **Stack overflow in top-down.** Memoised recursion on n = 10⁶ (or a deep
   tree) blows the 8 MB stack. Go bottom-up or iterative.
10. **Huge tables on the stack.** `int dp[5000][5000]` is 100 MB; make it
    `static`, global, or heap-allocated.
11. **Treating `tails` in LIS as the sequence.** It is not (see 3.6).
12. **Using greedy where DP is needed.** Coin change with {1,3,4}, 0/1
    knapsack by value/weight ratio: greedy is wrong. Prove greedy or use DP.
13. **Forgetting it is pseudo-polynomial.** O(n·W) with W = 10⁹ is not
    "polynomial" in practice.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_fibonacci_three_ways.c](c/01_fibonacci_three_ways.c) | Naive vs memo vs table vs two variables (call counts, table trace); climbing stairs | `make run P=dsa/17-dynamic-programming/c/01_fibonacci_three_ways.c` |
| [c/02_coin_change.c](c/02_coin_change.c) | Min coins (traced), greedy counter-example, number of ways vs orderings | `make run P=dsa/17-dynamic-programming/c/02_coin_change.c` |
| [c/03_knapsack.c](c/03_knapsack.c) | 0/1 knapsack table row by row + reconstruction, 1-D downward, unbounded upward | `make run P=dsa/17-dynamic-programming/c/03_knapsack.c` |
| [c/04_lis.c](c/04_lis.c) | LIS O(n²) with parents and O(n log n) with tails trace, both reconstructed | `make run P=dsa/17-dynamic-programming/c/04_lis.c` |
| [c/05_lcs_edit_distance.c](c/05_lcs_edit_distance.c) | LCS and edit distance tables printed, LCS string and edit script recovered, two-row LCS | `make run P=dsa/17-dynamic-programming/c/05_lcs_edit_distance.c` |
| [c/06_interval_dp.c](c/06_interval_dp.c) | Matrix chain with parenthesisation, LPS table | `make run P=dsa/17-dynamic-programming/c/06_interval_dp.c` |
| [c/07_grid_and_partition.c](c/07_grid_and_partition.c) | Unique paths with obstacles, min path sum (one row), equal / min-diff partition, count subsets | `make run P=dsa/17-dynamic-programming/c/07_grid_and_partition.c` |
| [c/08_bitmask_dp.c](c/08_bitmask_dp.c) | Held–Karp TSP (n = 15 in ms) and the assignment problem | `make run P=dsa/17-dynamic-programming/c/08_bitmask_dp.c` |
| [c/09_tree_dp.c](c/09_tree_dp.c) | Tree MIS (take/skip), diameter, subtree sizes without recursion | `make run P=dsa/17-dynamic-programming/c/09_tree_dp.c` |
| [cpp/01_memo_vs_tabulation.cpp](cpp/01_memo_vs_tabulation.cpp) | Generic `Memo<Key,Value>` wrapper; house robber via the 5 steps; decode ways | `make run P=dsa/17-dynamic-programming/cpp/01_memo_vs_tabulation.cpp` |
| [cpp/02_knapsack_and_coins.cpp](cpp/02_knapsack_and_coins.cpp) | 0/1, unbounded, bounded (binary splitting) knapsack; coin change as one template; `std::optional` | `make run P=dsa/17-dynamic-programming/cpp/02_knapsack_and_coins.cpp` |
| [cpp/03_sequence_dp.cpp](cpp/03_sequence_dp.cpp) | LIS with `std::lower_bound`/`upper_bound`, generic LCS, a real line diff, O(min) edit distance | `make run P=dsa/17-dynamic-programming/cpp/03_sequence_dp.cpp` |
| [cpp/04_interval_dp.cpp](cpp/04_interval_dp.cpp) | Top-down matrix chain, LPS, min palindrome cuts, burst balloons | `make run P=dsa/17-dynamic-programming/cpp/04_interval_dp.cpp` |
| [cpp/05_bitmask_dp.cpp](cpp/05_bitmask_dp.cpp) | TSP with tour reconstruction, min bins via submask enumeration, k equal-sum subsets | `make run P=dsa/17-dynamic-programming/cpp/05_bitmask_dp.cpp` |
| [cpp/06_tree_dp_rerooting.cpp](cpp/06_tree_dp_rerooting.cpp) | Vertex cover on trees, sum of distances for every node by rerooting (200 000 nodes) | `make run P=dsa/17-dynamic-programming/cpp/06_tree_dp_rerooting.cpp` |
| [cpp/07_pattern_catalogue.cpp](cpp/07_pattern_catalogue.cpp) | Kadane, word break, Catalan, stock-with-cooldown state machine, digit DP | `make run P=dsa/17-dynamic-programming/cpp/07_pattern_catalogue.cpp` |

Every program checks its DP against a brute-force solver on hundreds of
random small inputs, so a passing run means the recurrence is right, not
just that it printed the expected example.

---

## 9. Exercises

**Easy**
1. Min cost climbing stairs: `cost[i]` to step on stair i, you may climb 1
   or 2. *Hint:* `dp[i] = cost[i] + min(dp[i-1], dp[i-2])`.
2. House robber II: houses in a **circle**. *Hint:* run the linear version
   twice: without the first house, and without the last.
3. Count paths in an R×C grid without obstacles and check against
   `C(R+C-2, R-1)`.

**Medium**
4. Print *all* the coins used for min coin change. *Hint:* store which coin
   achieved `dp[a]`, then follow it back.
5. Longest common **substring** (contiguous). *Hint:* on mismatch the cell is
   0, not `max(up, left)`; answer is the max cell.
6. Target sum: assign + or − to each number to reach T. *Hint:* reduce to
   "count subsets with sum (total + T) / 2".
7. Rod cutting: price `p[len]`, cut a rod of length n for max revenue.
   *Hint:* unbounded knapsack where weight = length.
8. Convert `c/06_interval_dp.c`'s LPS to O(n) space.

**Hard**
9. Wildcard matching with `?` and `*`. *Hint:* two-sequence DP; `*` reads
   `dp[i-1][j]` (match one more char) or `dp[i][j-1]` (match empty).
10. Egg dropping: k eggs, n floors, min worst-case drops. *Hint:* flip it:
    `f[m][k]` = max floors checkable with m moves and k eggs =
    `f[m-1][k-1] + f[m-1][k] + 1`.
11. Count Hamiltonian paths in a graph with n ≤ 18 using `dp[mask][v]`.
12. Tree: max path sum where the path may start and end anywhere (values can
    be negative). *Hint:* like diameter, but with the best *two* positive
    child branches.
13. Digit DP: count numbers in [L, R] with no two equal adjacent digits.
    *Hint:* `count(R) - count(L-1)`; state includes the previous digit and
    a "started" flag for leading zeros.

---

## 10. Remember forever

```
DP = recursion + "never solve the same subproblem twice".
Need: optimal substructure + overlapping subproblems.
5 steps: STATE (one sentence) -> CHOICE on the last decision -> BASE
         -> ORDER (dependencies first) -> ANSWER (+ shrink space, rebuild path).
Top-down = recursion + cache; bottom-up = loops in dependency order.
0/1 knapsack: 1-D loop DOWN.  Unbounded / coin change: loop UP.
Count ways: coins outside = combinations; amounts outside = orderings.
Two strings: dp[i][j] on prefixes; match -> diagonal, else up/left.
Intervals: fill by length.  Sets (n <= 20): bitmask.  Trees: post-order.
```

Review this card with the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What two properties must a problem have for DP to help? Give a problem
   that has only one of them.
2. Why does naive Fibonacci take exponential time, and exactly how many
   distinct subproblems are there?
3. When would you choose top-down over bottom-up? When is top-down
   dangerous?
4. Write the 0/1 knapsack recurrence. Why must the 1-D version iterate the
   capacity downward?
5. Coin change "number of ways": what changes if you swap the two loops?
6. In the O(n log n) LIS, why is `tails` sorted, and why is it not the LIS?
7. Write the edit-distance recurrence and say what each of the three
   neighbours means.
8. Why must interval DP be filled by increasing length?
9. What does `dp[mask][v]` mean in Held–Karp, and why is it smaller than
   trying all permutations?
10. In tree MIS, why does each node need two values?
11. How does rerooting turn n DFS runs into two?
12. Why is O(n·W) knapsack called pseudo-polynomial?
13. Give a coin system where greedy fails and show the DP answer.
