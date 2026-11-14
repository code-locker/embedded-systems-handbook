# 16 — Greedy algorithms

> Make the choice that looks best **right now**, never look back, and prove
> that this never costs you anything.

---

## 1. Why this exists

Many optimisation problems ("fit the most", "pay with the fewest", "finish
everything as early as possible") have an exponential number of candidate
solutions. Trying them all (brute force) or remembering every sub-answer
(dynamic programming, lesson 17) works, but costs time and memory.

For a special class of problems one **locally best** choice is always part of
some globally best solution. Then you can:

1. make that choice,
2. throw away everything it rules out,
3. repeat on the smaller problem that is left,

and you are done, usually in **O(n log n)** (one sort) or **O(n)** (one pass).
No backtracking, no table.

The hard part is not the code (it is often 10 lines). It is **knowing when the
greedy choice is safe**. The intuitive greedy is often wrong, and a wrong
greedy still gives an answer that looks reasonable. So this lesson is half
algorithms and half "how to prove it, or break it".

---

## 2. Mental model

**The hiker in fog.** You can only see one step around you. A greedy hiker
always steps uphill. On a hill with one peak (a "convex" landscape) this
reaches the top. On a landscape with several peaks it can get stuck on a
small one.

```
   one peak: greedy wins              two peaks: greedy can get stuck

          /\                                /\
         /  \                       /\     /  \
        /    \                     /  \   /    \
   ____/  ^   \____           ____/ ^  \_/      \____
          |                         |
     every uphill step         greedy stops here: every step down looks
     gets closer to the top    worse, but the real top is to the right
```

Greedy problems are the "one peak" problems. Two properties tell you that you
are on one:

```
  +-------------------------------------------------------------------+
  | GREEDY-CHOICE PROPERTY                                            |
  |   Some optimal solution contains the greedy choice.               |
  |   ("Taking it never hurts.")                                      |
  |                                                                   |
  | OPTIMAL SUBSTRUCTURE                                              |
  |   After taking it, an optimal solution of what is LEFT, plus the  |
  |   choice, is optimal for the whole.                               |
  |   ("The rest is the same problem, just smaller.")                 |
  +-------------------------------------------------------------------+
```

DP needs only optimal substructure: it tries **every** choice and keeps the
best. Greedy also needs the greedy-choice property, so it tries **one**.

```
         DP at each step                Greedy at each step
            (state)                          (state)
          /   |    \                            |
        c1   c2    c3   try all, keep best      c*   take the one that
        ...  ...   ...                          ...  provably never hurts
```

---

## 3. How it works

### 3.0 The universal recipe

```
  sort the input by a well-chosen key     <- the whole intelligence is here
  for each element in that order:
      if it is compatible with what was already chosen:
          take it (update a single running value)
```

or, when the "best" element changes as you go:

```
  put the elements in a priority queue (heap)
  while the problem is not solved:
      take the best element out, use it, maybe push something back
```

### 3.1 How to PROVE a greedy: the exchange argument

This is the one proof technique to remember. It has three moves:

1. Take **any** optimal solution OPT.
2. If OPT does not agree with the greedy choice, **exchange** one of its
   elements for the greedy one.
3. Show the result is still valid and **no worse**. Repeat: you turn OPT into
   the greedy solution without ever losing value, so greedy is optimal.

Example for activity selection (below): let `g` be the activity with the
earliest finish time, and `o` the first activity in OPT.

```
  OPT:     [====o====)   [===)    [=====)
  greedy:  [==g==)
                 ^ g finishes no later than o (g has the earliest finish)

  swap o for g:
  OPT':    [==g==)       [===)    [=====)
  Everything after o started after o finished, so it also starts after g
  finishes. OPT' is valid and has the same size. So some optimal solution
  starts with g.
```

The other proof style is **"greedy stays ahead"**: show by induction that after
k steps the greedy solution is at least as good as any other solution after
k steps (used for the jump game: the greedy window always reaches at least as
far as any k-jump path).

### 3.2 Activity selection / interval scheduling

**Problem.** Activities with `[start, finish)`. One room. Maximise the number
of activities held.

**Which greedy?** There are 4 obvious candidates and only one is right:

| sort by              | correct? | counter-example                                     |
|----------------------|----------|-----------------------------------------------------|
| earliest start       | no       | one long activity that starts first covers 3 short |
| shortest duration    | no       | a short one that overlaps two long compatible ones |
| fewest conflicts     | no       | (subtle; a 11-interval example exists)              |
| **earliest finish**  | **yes**  | exchange argument above                             |

```
  earliest start fails              shortest first fails
  [===========================)     [=======)     [=======)
    [==)  [==)  [==)                      [===)
  greedy 1, optimum 3               greedy 1 (the short one), optimum 2
```

**Trace** (half-open intervals, sorted by finish):

```
  time     0 1 2 3 4 5 6 7 8 9
  A [0,2)  [===)                    finish 2   take        last_finish = 2
  B [1,4)    [=====)                finish 4   1 < 2  skip
  C [3,5)        [===)              finish 5   3 >= 2 take last_finish = 5
  D [4,7)          [=====)          finish 7   4 < 5  skip
  E [6,8)              [===)        finish 8   6 >= 5 take last_finish = 8
  F [2,9)      [=============)      finish 9   2 < 8  skip
                                    answer: A C E  (3)
```

Intuition: finishing early **leaves the most room for everyone else**.

Same sweep, different questions:
- *minimum intervals to remove so the rest do not overlap* = `n - max kept`;
- *minimum arrows to burst balloons* (closed intervals, a point hits every
  interval containing it): shoot at the finish of the earliest-finishing
  balloon; every balloon starting before that point is burst too.

### 3.3 Fractional knapsack

**Problem.** Capacity W. Items have value and weight; you may take any
**fraction** of an item (gold dust, not gold bars).

**Greedy.** Sort by value density `v/w`, pour in the densest first, cut the
last one.

```
  W = 50
  item  value weight  v/w
  A       60    10    6.0   take all    room 40   value  60
  B      100    20    5.0   take all    room 20   value 160
  C      120    30    4.0   take 20/30  room  0   value 240
```

Why it works: every kilogram in the bag is a slot; if a slot holds a less dense
kilogram while a denser one is left out, swapping them increases value
(exchange argument at the level of single kilograms).

Why it **fails for 0/1** (no cutting): same items, the ratio greedy takes A+B =
160 and C no longer fits; B+C = 220 is optimal. Indivisibility breaks the
"swap one kilogram" argument. 0/1 knapsack is a DP problem (lesson 17).

### 3.4 Huffman coding

**Problem.** Give each symbol a binary code so that (a) no code is a prefix of
another (so the bitstream needs no separators) and (b) the total length
`sum(freq * len)` is minimal.

Prefix-free codes = leaves of a binary tree (0 = left, 1 = right). The total
length is `sum(freq(leaf) * depth(leaf))`. So: put rare symbols deep and common
symbols shallow.

**Greedy.** Put every symbol in a min-heap by frequency. Repeatedly pop the two
lightest, make them siblings under a new node whose weight is their sum, push it
back. When one node is left it is the root.

```
  "abracadabra"   a:5  b:2  r:2  c:1  d:1

  heap                         action
  c1 d1 b2 r2 a5               merge c1+d1   -> (cd)2
  b2 r2 (cd)2 a5               merge b2+r2   -> (br)4
  (cd)2 (br)4 a5               merge 2+4     -> (cdbr)6
  a5 (cdbr)6                   merge 5+6     -> root 11

              (11)
            0/    \1
           a:5    (6)
                0/    \1
              (2)      (4)
             0/ \1    0/ \1
            c    d    b    r

  a = 0     c = 100    d = 101    b = 110    r = 111

  encode "abracadabra":
  a  b   r   a c   a d   a b   r   a
  0 110 111 0 100 0 101 0 110 111 0     = 23 bits   (ASCII: 88 bits)

  decode: walk from the root, each bit picks a child; at a leaf output the
  symbol and jump back to the root. No separators needed because symbols
  live ONLY at leaves (that is what prefix-free means).
```

Why merge the two lightest? Exchange argument: in some optimal tree the two
least frequent symbols are siblings at the deepest level (if a heavier symbol
sat deeper, swapping it with a light one would reduce the cost). Merging them
into one "super-symbol" of weight `f1+f2` gives a smaller problem with the same
structure (every merge adds its weight to the total: cost = sum of internal node
weights).

**Canonical Huffman** (what DEFLATE and JPEG actually store). The decoder does
not need the tree, only each symbol's **code length**. Sort symbols by
(length, symbol), give the first all zeros, then `code = (code + 1) << (len -
prev_len)`. The file header stores lengths only, which compresses well.

```
  lengths: a=1 b=3 c=3 d=3 r=3     canonical: a=0 b=100 c=101 d=110 r=111
```

### 3.5 Jump game

**Problem.** `a[i]` = how far you may jump from index i. (I) Can you reach the
last index? (II) With the fewest jumps?

**Greedy (I).** Carry `far` = the farthest index reachable so far. If you ever
stand at `i > far` you are stuck.

```
  a = [3, 2, 1, 0, 4]
  i:   0  1  2  3  4
  far: 3  3  3  3  -> i=4 > far=3: stuck (everything funnels into the 0)
```

**Greedy (II): BFS without a queue.** Indices reachable with exactly k jumps form
a contiguous **window**. Scan the current window, record the farthest reach,
that becomes the next window.

```
  a = [2, 3, 1, 1, 4]
       |  |  |  |  |
  k=0 [0]                 far from window = 0+2 = 2
  k=1    [1  2]           far from window = 1+3 = 4
  k=2          [3  4]     contains the last index -> answer 2
```

### 3.6 Gas station

**Problem.** Circular road; station i gives `gas[i]`, driving to i+1 costs
`cost[i]`. Find a start from which you complete the loop, or -1.

Two facts:
1. If `sum(gas) >= sum(cost)` a start exists (and if not, none does).
2. If starting at `s` the tank first goes negative at `i`, no start in
   `s..i` works: each of them reaches `i` with *less* fuel (they skip a prefix
   whose balance was >= 0). So jump straight to `i + 1`.

```
  gas  = [1, 2, 3, 4, 5]
  cost = [3, 4, 5, 1, 2]
  diff = [-2,-2,-2, 3, 3]      total = 0 >= 0

  i  diff  tank  start
  0   -2    -2    -> reset, start = 1
  1   -2    -2    -> reset, start = 2
  2   -2    -2    -> reset, start = 3
  3    3     3
  4    3     6    answer 3
```

One pass, O(n), O(1) memory, versus O(n^2) for "simulate from every start".

### 3.7 Minimum platforms (meeting rooms, max concurrency)

**Problem.** Trains with arrival/departure times. Minimum platforms so no train
waits = the maximum number of trains present at the same instant.

**Greedy sweep.** Sort arrivals and departures separately; walk both with two
pointers; arrival → +1, departure → -1; answer = peak.

```
  arr: 900  940  950 1100 1500 1800
  dep: 910 1120 1130 1200 1900 2000

  time   900  910  940  950 1100 1120 1130 1200 1500 1800 1900 2000
  event   A    D    A    A    A    D    D    D    A    A    D    D
  count   1    0    1    2   [3]   2    1    0    1    2    1    0
```

Tie rule: if a train arrives exactly when another departs, do you need two
platforms? Decide (closed vs half-open intervals) and sort ties accordingly.
That one line is the most common bug.

To also know **which** platform each train gets: process trains by arrival and
keep a **min-heap of departure times**. If the earliest-freeing platform is free
before this train arrives, reuse it; otherwise open a new one. The heap greedy
uses exactly `peak` platforms, which is a lower bound, so it is optimal.

### 3.8 Scheduling with deadlines (EDF)

One CPU, jobs with duration and deadline; minimise the **maximum lateness**.
Run jobs in order of deadline — **Earliest Deadline First**. Exchange
argument: two adjacent jobs in the wrong deadline order can be swapped without
increasing max lateness; repeat until sorted.

```
  before swap |--B (dl 5)--|--A (dl 3)--|    A late by T-3
  after  swap |--A (dl 3)--|--B (dl 5)--|    B late by T-5 <= T-3, A earlier
```

Related: unit-time jobs with deadlines and profits — take jobs by profit, put
each in the **latest** free slot not after its deadline.

### 3.9 When greedy fails

```
  coins {1, 3, 4}, amount 6
    greedy (largest first): 4 + 1 + 1   = 3 coins
    optimal:                3 + 3       = 2 coins
```

Taking a 4 is locally best but destroys the 3+3 option. The euro/rupee/US
systems are **canonical** (greedy is always optimal); arbitrary systems are not.
Kozen and Zaks showed: if greedy fails at all, it fails for some amount below
`c_max + c_second_max`, so you can test a coin system quickly (program `c/07`).

Other tempting-but-wrong greedies (program `cpp/06`):

| problem                       | tempting greedy           | why it fails                      | fix          |
|-------------------------------|---------------------------|-----------------------------------|--------------|
| coin change (arbitrary coins) | largest coin first        | blocks a better combination       | DP           |
| 0/1 knapsack                  | best value/weight first   | items cannot be split             | DP           |
| weighted interval scheduling  | earliest finish           | ignores weights                   | DP + binary search |
| travelling salesman           | nearest neighbour         | can be arbitrarily bad            | DP / heuristics |
| longest path in a graph       | take the longest edge     | no optimal substructure           | NP-hard      |

**Rule of thumb:** if you cannot find a 3-line exchange argument, try to break
your greedy with a tiny brute force on random inputs. Every program in this
lesson does exactly that.

---

## 4. Complexity table

| algorithm                         | time        | extra space | why                                              |
|-----------------------------------|-------------|-------------|--------------------------------------------------|
| activity selection                | O(n log n)  | O(1)–O(n)   | sort by finish, one O(n) scan                    |
| min removals / min arrows         | O(n log n)  | O(1)        | same sort, same scan                             |
| fractional knapsack               | O(n log n)  | O(n)        | sort by density; (O(n) possible with selection)  |
| Huffman build (k symbols)         | O(k log k)  | O(k)        | 2k-1 heap pops/pushes, each O(log k)             |
| Huffman encode / decode           | O(total bits) | O(1)      | one table lookup / one tree step per bit         |
| jump game I and II                | O(n)        | O(1)        | one pass with a running maximum                  |
| gas station                       | O(n)        | O(1)        | one pass, the skip argument                      |
| min platforms (sweep)             | O(n log n)  | O(1)        | two sorts, two-pointer merge                     |
| platform assignment (heap)        | O(n log n)  | O(n)        | one heap op per train                            |
| EDF                               | O(n log n)  | O(n)        | sort by deadline                                 |
| job sequencing (simple)           | O(n · D)    | O(D)        | scan back for a free slot; DSU makes it ~O(n α)  |
| greedy coin change                | O(#coins)   | O(1)        | one division per coin                            |

The pattern: greedy cost is almost always **the sort** plus a linear pass.

---

## 5. C vs C++

| need                    | C                                         | C++                                              |
|-------------------------|-------------------------------------------|--------------------------------------------------|
| sort by a key           | `qsort(a, n, sizeof *a, cmp)`, cmp returns `<0/0/>0` | `std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.end < b.end; })` |
| stable tie order        | write insertion/merge sort                | `std::stable_sort`                               |
| min-heap                | hand-written array heap (see `c/03`)      | `std::priority_queue<T, vector<T>, std::greater<T>>` |
| heap of `unique_ptr`    | —                                         | `std::make_heap/pop_heap` on a `vector` (can move out of `back()`; `priority_queue::top()` is const) |
| sort indices not items  | array of ints + comparator using a global or a struct | `std::iota` + lambda capturing the data |
| "maybe no answer"       | return `-1`                               | `std::optional<int>`                             |
| sum                     | loop                                      | `std::accumulate(..., 0L)` (note `0L`: avoid int overflow) |

Comparator traps: in C never write `return a - b;` (it overflows for large
ints); use `(a > b) - (a < b)`. In C++ the comparator must be a **strict weak
ordering** (`<`, never `<=`), or `std::sort` has undefined behaviour and can
crash.

---

## 6. Where it is used in the real world

- **Compression.** Huffman coding is the entropy coder of DEFLATE (gzip, zlib,
  PNG, ZIP), baseline JPEG, MP3 and AAC. DEFLATE stores **canonical** code
  lengths per block. H.264's CAVLC uses fixed, pre-computed variable-length
  code tables — the Huffman idea, chosen per context. (CABAC and modern codecs
  moved to arithmetic coding, which beats Huffman's whole-bit limit.)
- **Linux scheduler.** `SCHED_DEADLINE` is EDF + CBS (constant bandwidth
  server). The real-time classes always pick the highest-priority runnable task —
  a greedy choice made at every tick. CFS/EEVDF picks the task with the smallest
  virtual (eligible) deadline from a red-black tree — a greedy over a heap-like
  structure.
- **Memory allocators.** First-fit / best-fit free-list allocators are greedy;
  the buddy allocator greedily splits the smallest block that fits.
- **Networking.** Dijkstra (lesson 13) is greedy: OSPF and IS-IS compute
  shortest paths with it. Prim and Kruskal (minimum spanning tree) are greedy;
  the spanning tree protocol builds a tree of shortest paths to the root bridge.
- **Interval problems everywhere.** Video players and GStreamer schedule
  buffers by timestamp; the "min platforms" sweep answers "how many decoder
  buffers / DMA descriptors are in flight at once", which sizes a buffer pool.
  Calendar apps (meeting rooms), CPU register allocation (linear-scan register
  allocation in JITs sweeps live intervals sorted by start).
- **Embedded.** A TDA4VM/TI RTOS task graph with deadlines on the R5F cores is
  scheduled with rate-monotonic or EDF reasoning. Choosing which DMA channel or
  hardware queue to hand out next is the heap-of-free-times greedy.
- **Money.** ATMs and vending machines give change greedily — which is why
  currencies are designed to be canonical.

---

## 7. Common mistakes and pitfalls

1. **Using a greedy without a proof.** "It works on my 3 examples" means
   nothing. Prove it (exchange / stays ahead) or test against brute force on
   thousands of random small inputs.
2. **Wrong sort key.** Activity selection sorts by *finish*, not start or
   length. EDF sorts by *deadline*, not duration.
3. **Tie handling and interval ends.** `[s, e)` vs `[s, e]` changes `>` to `>=`
   in the compatibility test and the event order in a sweep. Decide once, write
   it in a comment.
4. **Comparator overflow** (`a - b` in C) and non-strict comparators (`<=` in
   C++). Comparing ratios with division loses precision; compare
   `v1*w2 > v2*w1` (watch for overflow there too: use 64-bit).
5. **Fractional knapsack logic applied to 0/1 knapsack.** Wrong answer, no crash.
6. **Huffman with one distinct symbol.** The tree is a single leaf of depth 0:
   give it a 1-bit code or the encoder outputs nothing.
7. **Huffman determinism.** Encoder and decoder must build the same tree; break
   frequency ties by a fixed rule — or use canonical codes, which only need the
   lengths.
8. **Gas station: returning a start without checking the total.** The pass
   always ends with *some* start index; it is valid only if total >= 0.
9. **Coin change with greedy on non-canonical coins.** Use DP unless the coin
   system is known canonical.

---

## 8. Files in this lesson

| file | what it shows | run |
|------|---------------|-----|
| [c/01_activity_selection.c](c/01_activity_selection.c) | earliest-finish greedy; counter-examples for earliest-start and shortest-first; checked vs 2^n brute force | `make run P=dsa/16-greedy/c/01_activity_selection.c` |
| [c/02_fractional_knapsack.c](c/02_fractional_knapsack.c) | sort by density with a division-free comparator; why the same greedy fails for 0/1 | `make run P=dsa/16-greedy/c/02_fractional_knapsack.c` |
| [c/03_huffman.c](c/03_huffman.c) | hand-written min-heap, tree in a node pool, encode/decode "abracadabra", prefix-free and optimal-cost checks | `make run P=dsa/16-greedy/c/03_huffman.c` |
| [c/04_jump_game.c](c/04_jump_game.c) | reachability with a running max; min jumps as an implicit BFS; checked vs O(n^2) DP | `make run P=dsa/16-greedy/c/04_jump_game.c` |
| [c/05_gas_station.c](c/05_gas_station.c) | one-pass start finding with the skip argument; checked vs simulating every start | `make run P=dsa/16-greedy/c/05_gas_station.c` |
| [c/06_min_platforms.c](c/06_min_platforms.c) | two sorted arrays + two pointers, closed-interval tie rule; checked vs brute force | `make run P=dsa/16-greedy/c/06_min_platforms.c` |
| [c/07_coin_change_greedy_fails.c](c/07_coin_change_greedy_fails.c) | finds the smallest amount where greedy coin change fails ({1,3,4}, old UK coins), Kozen–Zaks bound | `make run P=dsa/16-greedy/c/07_coin_change_greedy_fails.c` |
| [cpp/01_interval_scheduling.cpp](cpp/01_interval_scheduling.cpp) | one sweep, three problems: max kept, min removed, min arrows; `std::sort` + lambdas | `make run P=dsa/16-greedy/cpp/01_interval_scheduling.cpp` |
| [cpp/02_fractional_knapsack.cpp](cpp/02_fractional_knapsack.cpp) | returns the plan (item, fraction) by sorting indices; checked vs an exact LP-style search | `make run P=dsa/16-greedy/cpp/02_fractional_knapsack.cpp` |
| [cpp/03_huffman_canonical.cpp](cpp/03_huffman_canonical.cpp) | `unique_ptr` tree, heap of owning pointers, canonical codes as in DEFLATE, real bit packing, puff-style decoder, Kraft check | `make run P=dsa/16-greedy/cpp/03_huffman_canonical.cpp` |
| [cpp/04_jump_and_gas.cpp](cpp/04_jump_and_gas.cpp) | jump game II with `std::optional`, gas station with `std::accumulate`; checked vs BFS / brute force | `make run P=dsa/16-greedy/cpp/04_jump_and_gas.cpp` |
| [cpp/05_min_platforms_rooms.cpp](cpp/05_min_platforms_rooms.cpp) | event sweep and min-heap room assignment (which platform each train gets) | `make run P=dsa/16-greedy/cpp/05_min_platforms_rooms.cpp` |
| [cpp/06_greedy_vs_dp.cpp](cpp/06_greedy_vs_dp.cpp) | three tempting greedies that fail (coins, weighted intervals, 0/1 knapsack) and the DP that fixes each | `make run P=dsa/16-greedy/cpp/06_greedy_vs_dp.cpp` |
| [cpp/07_deadline_scheduling.cpp](cpp/07_deadline_scheduling.cpp) | EDF minimises max lateness (checked vs all permutations); job sequencing with deadlines and profits | `make run P=dsa/16-greedy/cpp/07_deadline_scheduling.cpp` |

---

## 9. Exercises

1. **(easy)** Change `c/01` to closed intervals `[s, f]`. Which single
   character changes? Add a test where two activities touch.
2. **(easy)** Minimum number of coins for the euro system by greedy; verify
   against the DP in `c/07` for all amounts up to 1000.
3. **(easy)** *Assign cookies*: children with greed factors, cookies with
   sizes; maximise content children. Hint: sort both, two pointers, give each
   child the smallest cookie that satisfies them.
4. **(medium)** *Merge intervals* and *insert interval*. Hint: sort by start
   (not finish!) — different problem, different key. Explain why.
5. **(medium)** Print the Huffman codes of the bytes of a real file
   (`/etc/services`) and compute the compressed size in bytes. Compare with
   `gzip -9` (which also uses LZ77, so it should win).
6. **(medium)** *Partition labels*: split a string into as many parts as
   possible so each letter appears in only one part. Hint: last occurrence of
   each letter = an interval; this is a jump-game-like running max.
7. **(medium)** Job sequencing in O(n log n) using a DSU where `find(d)`
   returns the latest free slot <= d (lesson on union-find in 13-graphs).
8. **(hard)** *Candy*: children in a row with ratings; each gets >= 1 candy and
   more than a neighbour with a higher rating; minimise candies. Hint: two
   greedy passes (left to right, right to left), take the max.
9. **(hard)** *Minimum number of refuelling stops* (car with a tank, stations
   on a line). Hint: drive as far as possible; when stuck, retroactively refuel
   at the largest station passed (max-heap).
10. **(hard)** Write a length-limited Huffman (max code length 15 as in
    DEFLATE). Hint: look up the package-merge algorithm; or the simpler heuristic
    zlib uses (`gen_bitlen` overflow fixing).

---

## 10. Remember forever

```
GREEDY = sort by the right key (or use a heap) + one pass, never undo.
Valid only with PROOF: greedy-choice property + optimal substructure.
Proof tool: EXCHANGE ARGUMENT — swap OPT's choice for greedy's, show no loss.
Intervals: sort by FINISH, take if start >= last_finish.
Fractional knapsack: by value/weight; 0/1 knapsack -> DP, not greedy.
Huffman: merge the two LIGHTEST (min-heap); left=0 right=1; prefix-free.
Jump/gas: one running number (farthest reach / tank since start).
Platforms: +1 arrive, -1 depart, answer = peak; mind ties.
Coins {1,3,4}, 6: greedy 3 coins, optimum 2 — greedy can fail.
No proof? Test against brute force on random small inputs.
```

Review on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. State the greedy-choice property and optimal substructure in one sentence
   each. Which of the two does DP *not* need?
2. Walk through the exchange argument for "earliest finish first".
3. Give a counter-example for "earliest start first" and for "shortest first".
4. Why does the ratio greedy solve fractional knapsack but not 0/1 knapsack?
5. Build the Huffman tree for `a:5 b:2 r:2 c:1 d:1` on paper and give the
   total bit count. Why is the result prefix-free?
6. Why can DEFLATE store only code *lengths* instead of the tree?
7. In the gas station problem, why may we skip every start between the old
   candidate and the station where the tank went negative?
8. Jump game II: why does each "level" of BFS form a contiguous window?
9. In the min-platforms sweep, when should an arrival at time t be processed
   before a departure at time t?
10. Find the smallest amount where greedy fails for coins `{1, 5, 6, 9}`. (Try
    it in `c/07`.)
11. Which Linux scheduling class is EDF, and what does EDF minimise?
