# 15 — Recursion and Backtracking

> A recursive function solves a big problem by **trusting itself** to solve a
> smaller copy of the same problem. Backtracking is recursion used for
> **searching**: make a choice, explore what follows, then **undo the choice**
> and try the next one.

---

## 1. Why this exists

Some problems are naturally defined in terms of smaller copies of themselves:

- a directory contains files **and directories** (which contain files and directories...)
- a tree node has children that are **trees**
- "all orderings of 5 items" = for each item first, "all orderings of the other 4"
- parsing `(1 + (2 * 3))`: an expression contains **expressions**

Writing these with loops alone means managing your own stack of "where was I"
by hand. Recursion lets the CPU's call stack do that bookkeeping.

**Backtracking** is the general tool for problems of the form "find all / any
arrangement that satisfies the rules": placing queens, filling a Sudoku,
choosing items that sum to a target, matching a word in a grid. Brute force
would generate *every* arrangement and test each one at the end. Backtracking
builds arrangements **one decision at a time** and abandons a partial
arrangement the moment it breaks a rule, skipping the whole subtree of
arrangements that would have started that way. That's often the difference
between 10^10 steps and 10^4.

---

## 2. Mental model

### Recursion = a stack of sticky notes

Picture a desk with a stack of sticky notes. Each time a function is called,
you write a new note: *"computing fact(3), waiting for fact(2), then multiply by 3"*
and put it **on top**. You only ever work on the top note. When it is done,
you peel it off and hand its answer to the note underneath.

That stack of notes **is** the call stack. Each note is a **stack frame**.

```
 call fact(4)                              memory (stack grows DOWN)
                                           high addr
 ┌───────────────────────────────┐        ┌──────────────────────────┐
 │ main                          │        │ main's frame              │
 ├───────────────────────────────┤        ├──────────────────────────┤
 │ fact n=4  waiting: 4 * ?      │        │ ret addr | saved fp | n=4 │
 ├───────────────────────────────┤        ├──────────────────────────┤
 │ fact n=3  waiting: 3 * ?      │        │ ret addr | saved fp | n=3 │
 ├───────────────────────────────┤        ├──────────────────────────┤
 │ fact n=2  waiting: 2 * ?      │        │ ret addr | saved fp | n=2 │
 ├───────────────────────────────┤        ├──────────────────────────┤
 │ fact n=1  BASE CASE -> 1      │  <- sp │ ret addr | saved fp | n=1 │
 └───────────────────────────────┘        └──────────────────────────┘
                                           low addr
 then unwinding: 1 -> 2*1=2 -> 3*2=6 -> 4*6=24
```

`c/01_recursion_basics.c` prints real frame addresses so you can see each
call land a fixed number of bytes (144 in the sanitizer build)
**below** its caller.

### Backtracking = walking a maze with chalk

You're in a maze. At each junction you pick a corridor and chalk an arrow
(**choose**). You keep walking (**explore**). At a dead end you walk back to
the last junction and **rub out the arrow** (**un-choose**), then try the
next corridor. You never explore a corridor twice from the same junction,
and you never carry stale chalk marks into a new attempt.

```
                    start
              ┌───────┼───────┐
           choose   choose   choose          each level = one decision
              A        B        C
            ┌─┴─┐    ┌─┴─┐     ✗   <- rule broken: PRUNE, never go below
            D   E    D   E
            ✓   ✗    ✗   ✓        <- leaves = complete candidates
```

---

## 3. How it works

### 3.1 Base case and recursive case

Every recursive function has two parts:

| Part | Job | Forget it and... |
|---|---|---|
| **Base case** | the smallest input, answered directly | infinite recursion → stack overflow (SIGSEGV) |
| **Recursive case** | shrink the problem, call yourself, combine | no progress → infinite recursion too |

The recursive case must move **strictly toward** the base case
(`n-1`, `n/2`, "one fewer element", "next row").

```c
uint64_t fact(unsigned n) {
    if (n <= 1) return 1;          /* base case      */
    return n * fact(n - 1);        /* recursive case */
}
```

**The leap of faith.** To write or check a recursive function you do **not**
trace every call. You check:

1. the base case is correct;
2. *assuming* the recursive call returns the right answer for the smaller
   input, the current call combines it correctly.

That's mathematical induction. Tower of Hanoi is the classic example. You
can't trace 2^20 moves in your head, but you can check: "move n-1 disks out
of the way (trust it), move the big one, move n-1 back on top (trust it)".

### 3.2 How recursion uses the stack: what a frame holds

On x86-64 / AArch64 one frame typically holds:

```
 ┌─────────────────────┐ higher address (caller)
 │ arguments (if spilled)│
 │ return address       │  <- where to continue in the caller (LR on arm64)
 │ saved frame pointer  │  <- links frames into a chain (what gdb's `bt` walks)
 │ saved registers      │
 │ local variables      │
 └─────────────────────┘ lower address  <- stack pointer (sp)
```

Consequences you must remember:

- **Space = depth × frame size.** `fact(n)` uses O(n) stack even though it
  allocates nothing.
- **Stack size is small and fixed:** 8 MB default for a Linux user thread
  (`ulimit -s`, also glibc's default for new pthreads), whatever you pass to
  `pthread_attr_setstacksize` for others, **16 KB for a Linux kernel stack**
  (arm64), a few KB on a microcontroller RTOS task. Recursion
  depth 100,000 is fine as a loop and fatal as recursion.
- Each frame is private, so two calls of the same function never see each
  other's locals. That's why recursion "just works".

### 3.3 Recursion tree

When a function calls itself more than once, the calls form a **tree**.
Total work = sum of the work over all nodes.

```
                          fib(5)                         calls(n) = 1 + calls(n-1) + calls(n-2)
                 /                    \                  calls(5) = 15
             fib(4)                   fib(3)*            grows like 1.618^n
           /        \                /      \
       fib(3)*      fib(2)*      fib(2)*    fib(1)       * = recomputed subtree
       /    \       /    \       /    \
    fib(2)* fib(1) fib(1) fib(0) fib(1) fib(0)
    /    \
 fib(1) fib(0)
```

Reading complexity off the tree:

| Shape | Example | Nodes | Depth (= stack) |
|---|---|---|---|
| one call, n-1 | factorial, list sum | n | n |
| one call, n/2 | binary search, fast power | log n | log n |
| two calls, n/2 + O(n) work | merge sort | n log n work | log n |
| two calls, n-1 | Hanoi | 2^n | n |
| two calls, n-1 and n-2 | naive Fibonacci | ~1.618^n | n |
| n calls, then n-1, ... | permutations | n! leaves | n |

### 3.4 Memoisation preview

Look at the fib tree: `fib(3)` is computed twice, `fib(2)` three times. If
we **cache** each answer the first time (memoisation), every repeated subtree
becomes a single lookup:

```
 fib(5) -> fib(4) -> fib(3) -> fib(2) -> fib(1)
             \         \         \_ fib(0)
              \         \_ fib(1)          every right child = cache HIT
               \_ fib(2) (hit)
               ...  ~2n calls instead of ~1.618^n
```

From `c/02_recursion_tree_memo.c`:

```
  n   fib(n)        naive calls   memo calls
  20  6765          21891         39
  30  832040        2692537       59
```

"Recursion + cache" is **top-down dynamic programming**, chapter 17. The
signal to look for: **the same arguments come back again** (overlapping
subproblems). Backtracking problems usually do **not** have that, because
each path carries different state (which cells are used, which items are
chosen), so they stay exponential and you speed them up by **pruning**
instead.

### 3.5 Tail recursion

A call is a **tail call** when nothing is left to do after it returns:

```c
return n * fact(n - 1);          /* NOT a tail call: multiply happens after  */
return fact_tail(n - 1, acc * n);/* tail call: result is returned untouched  */
```

In a tail call the current frame is dead weight, so the compiler can **reuse**
it, turning the recursion into a jump (tail-call optimisation, TCO).

```
 fact_tail(4, 1) -> fact_tail(3, 4) -> fact_tail(2, 12) -> fact_tail(1, 24) -> 24
 one frame, rewritten in place each time   ==   a while loop
```

Important for C/C++:

- **C and C++ do not guarantee TCO.** gcc/clang do it at `-O2`, **not** at `-O0`
  (this repo's debug build), and not if any local's address escapes or a
  destructor must run after the call.
- So in C, use tail recursion as a **stepping stone to a loop**, not as a
  promise. (Scheme guarantees TCO. C does not.)

### 3.6 Recursion → iteration

Three recipes, from easiest to most general (all in `c/03_*.c` and `cpp/07_*.cpp`):

**Recipe 1: tail recursion → loop.** Parameters become variables; the call
becomes "reassign and go to the top".

```c
int bs(a, lo, hi, key) {                      while (lo < hi) {
  if (lo >= hi) return -1;                        mid = ...;
  mid = ...;                                      if (a[mid] == key) return mid;
  if (a[mid] == key) return mid;       ==>        if (a[mid] < key) lo = mid + 1;
  if (a[mid] < key) return bs(a,mid+1,hi,key);    else              hi = mid;
  return bs(a, lo, mid, key);                 }
}                                             return -1;
```

**Recipe 2: work list.** When the order of the calls doesn't matter much
(flood fill, DFS, pre-order), push "calls" onto your own stack and pop them in
a loop. Push children in **reverse** order if you want the same visiting order.

**Recipe 3: explicit frames with a resume point.** For general recursion where
work happens **between** calls (in-order, post-order, Hanoi), a frame must
remember *where to continue*. That's the return address, made explicit:

```
 struct Frame { int n, from, to, via; int stage; };   stage 0: before 1st call
                                                       stage 1: between calls
                                                       stage 2: done -> pop
 stack (top right), hanoi(2):
 [ {2,s0} ]
 [ {2,s1} {1,s0} ]
 [ {2,s1} {1,s1} {0} ]          n==0 -> pop
 [ {2,s1} {1,s1} ]  move A->B, stage 2, push {0}
 ...
```

Your explicit stack lives on the heap (or a static array), so its size limit
is RAM, not the 8 MB / 16 KB thread stack.

### 3.7 The backtracking template

```c
void backtrack(State *s) {
    if (is_solution(s)) { record(s); return; }
    for (each candidate c in candidates(s)) {
        if (!valid(s, c)) continue;        /* PRUNE: skip doomed branches  */
        apply(s, c);                       /* CHOOSE                       */
        backtrack(s);                      /* EXPLORE                      */
        undo(s, c);                        /* UN-CHOOSE: restore the state */
    }
}
```

Four questions decide every backtracking problem:

| Question | Subsets | Permutations | N-Queens | Sudoku |
|---|---|---|---|---|
| What's one decision? | take/skip item i | which value goes in slot k | which column in row r | which digit in cell (r,c) |
| Depth of the tree | n | n | n | number of empty cells |
| What state to undo? | pop path | pop path, clear used[i] | clear col/diag flags | clear the cell + masks |
| When to prune? | (never: all are answers) | value already used | square attacked | digit already in row/col/box |

**Un-choose must restore EXACTLY what choose changed.** If choose sets three
flags, un-choose clears the same three. If you mutate in place (word search
overwrites a letter with `#`), you must write back the original.

Two ways to avoid explicit undo:

- **pass state by value**: the bitmask N-Queens passes `cols, ld, rd` as
  arguments, so the caller's copies are never touched. Cheap for a few words,
  expensive for big arrays.
- **overwrite by position**: generate-parentheses writes `buf[len]`; the next
  attempt at the same depth simply overwrites it.

### 3.8 Subsets

Decision at depth i: **take** or **skip** element i. Every leaf is a subset.

```
                         []                          {a, b, c}
              take a /        \ skip a
                [a]              []
            /       \         /      \
        [a,b]       [a]     [b]       []
        /  \        / \     / \       / \
 [a,b,c][a,b] [a,c] [a] [b,c] [b]  [c]  []          2^3 = 8 leaves
```

Alternative ("start index") form, where **every node** is a subset. Used for
combinations and combination sum:

```
                 {}
          a/     b|     \c          loop i = start..n-1, recurse with start = i+1
        {a}     {b}     {c}         -> {b,a} can never appear (only increasing indices)
       b/ \c     |c
    {a,b} {a,c} {b,c}
      |c
   {a,b,c}
```

Cost: O(2^n · n) to output all subsets (2^n of them, each up to n long).

### 3.9 Permutations

Decision at depth k: **which unused element** goes into slot k.

```
                       _ _ _
             1/          2|          \3
          1 _ _        2 _ _        3 _ _
          2/ \3        1/ \3        1/ \2
        12_   13_    21_   23_    31_   32_
         |     |      |     |      |     |
        123   132    213   231    312   321          3! = 6 leaves
```

Two implementations (both in `c/04_*.c`):

- `used[]` array + `path`: emits in **lexicographic** order (same as
  `std::next_permutation`, which the program checks).
- **swap**: put element i into slot k by `swap(a[k], a[i])`, recurse on
  k+1, swap back. No extra memory, but the order isn't lexicographic.

**Duplicates** (`{1,1,2}` has 3 distinct permutations, not 6): sort, then skip
`i` when `a[i] == a[i-1]` and `i-1` is **not** used. That forces equal values
to be placed in index order, so each arrangement is produced once
(`cpp/02_backtracking_template.cpp`).

### 3.10 Combinations C(n, k)

Subsets of a fixed size k, generated with increasing indices. A nice
**pruning bound**: if you still need `k - len` numbers, the loop can stop at
`n - (k - len) + 1`, because starting later leaves too few numbers to finish.

```
 C(4,2):   start=1: 1 -> {1,2} {1,3} {1,4}
           start=2: 2 -> {2,3} {2,4}
           start=3: 3 -> {3,4}
           start=4: STOP (only 1 number left, need 2)   <- pruned by the bound
```

### 3.11 N-Queens

Place N queens on an N×N board so that no two share a row, column or diagonal.

- **Depth = row**, so "one per row" is built in.
- **Choice = column.** Check column and both diagonals in O(1):
  `col[c]`, `diag[r+c]` (/ direction), `anti[r-c+n-1]` (\ direction).

```
 r+c is constant on "/"           r-c is constant on "\"
   c: 0 1 2 3                       c: 0  1  2  3
 r0   0 1 2 3                     r0   0 -1 -2 -3
 r1   1 2 3 4                     r1   1  0 -1 -2
 r2   2 3 4 5                     r2   2  1  0 -1
 r3   3 4 5 6                     r3   3  2  1  0
```

Trace for N=4 (the first solution):

```
 row0 c0       row1 c2        row2: c0 col✗ c1 diag✗   back to row1, try c3
 Q . . .       Q . . .              c2 col✗ c3 diag✗
 . . . .       . . Q .        -> DEAD END
 . . . .       . . . .
 . . . .       . . . .

 row1 c3       row2 c1        row3: every column attacked -> back up ... row0 c1
 Q . . .       Q . . .
 . . . Q       . . . Q
 . . . .       . Q . .
 . . . .       . . . .

 row0 c1 -> row1 c3 -> row2 c0 -> row3 c2:   . Q . .
                                             . . . Q
                                             Q . . .
                                             . . Q .     solution 1 of 2
```

Pruning in numbers (from `c/05_n_queens.c`): for N=8, backtracking visits
**2,057** nodes; testing all 8! permutations (already one-per-row and
one-per-column!) visits 109,601; naive "any square for each queen" is
C(64,8) ≈ 4.4 × 10^9.

**Bitmask version** (`cpp/03_n_queens_bitmask.cpp`): keep the attacked columns
for the *current* row in three integers. Moving down one row shifts the
diagonal attacks one column:

```
 queen placed at col 2 in row r (N=5):
            cols     ld (/)    rd (\)
 row r    : 00100    00100     00100
 row r+1  : 00100    01000     00010      ld <<= 1, rd >>= 1
 free     = ~(cols | ld | rd) & 11111 = 10001  -> try col 0 and col 4
 pick     = free & -free  (lowest set bit)
```

### 3.12 Sudoku solver

```
 find an empty cell
 for d in 1..9:
     if d is not in its row, column, or 3x3 box:
         place d; if solve() succeeds -> done
         remove d
 return failure (no digit fits -> caller must try its next digit)
```

Two big improvements (`c/06_sudoku_solver.c`, `cpp/04_sudoku.cpp`):

1. **Bitmasks:** `rows[r]`, `cols[c]`, `boxes[b]` each a 9-bit set of used
   digits. Candidates = `~(rows[r] | cols[c] | boxes[b]) & 0x3FE`: one
   expression instead of 27 comparisons.
2. **Most-constrained cell first (MRV):** fill the cell with the **fewest**
   candidates. A cell with 1 candidate is forced (no branching); a cell with
   0 candidates proves the current partial grid is dead, so prune
   immediately.

```
 puzzle 0: plain = 4209 nodes, bitmask+MRV = 52 nodes
 puzzle 1: plain = 13529 nodes, bitmask+MRV = 104 nodes
```

**Counting solutions** with a limit (stop at 2) proves a puzzle is unique.
That's how puzzle generators work.

### 3.13 Word search

Find a word in a letter grid, moving up/down/left/right, using each cell at
most once per path.

```
  A B C E        search "SEE" from S at (1,3):
  S F C S        mark (1,3)='#'   -> up (0,3)='E' ✓ mark; next 'E'? neighbours C,#,- ✗
  A D E E                           UNMARK (0,3)  <- forget this and later searches break
                                  -> down (2,3)='E' ✓ mark; next 'E'? left (2,2)='E' ✓ FOUND
```

The "visited" state is undone by writing the letter back. **Word Search II**
(many words) puts the words in a **trie** (chapter 11) and walks the grid once
per start cell; a path is pruned as soon as it isn't a prefix of any remaining
word (`cpp/05_word_search_trie.cpp`).

### 3.14 Generate parentheses

All balanced strings of n pairs. Two rules make every partial string valid,
so there are **no dead ends at all**:

```
 add '(' if open  < n
 add ')' if close < open

 n = 2:            ""
                   |(
                  "("
            ( /        \ )
          "(("          "()"
            |)            |(
          "(()"         "()("
            |)            |)
          "(())"        "()()"          Catalan(2) = 2
```

The count is the Catalan number C(n) = (2n)! / ((n+1)! n!): 1, 1, 2, 5, 14,
42, 132... For n = 10: 16,796 results from 82,499 nodes, versus 2^20 =
1,048,576 strings for generate-and-test.

### 3.15 Pruning

Pruning = **don't enter a subtree that can't contain an answer**. The
earlier you cut, the more you save, because you skip everything below.

| Kind | Example | Code |
|---|---|---|
| Feasibility | queen square attacked | `if (col[c] \|\| diag[r+c] ...) continue;` |
| Bound | sorted candidates, sum overshoots | `if (a[i] > remaining) break;` (break, not continue!) |
| Counting | need k-len more items but fewer remain | loop bound `i <= n-(k-len)+1` |
| Forced moves / fail-first | Sudoku MRV, cell with 0 candidates | pick the most constrained cell |
| Symmetry | dedupe equal siblings | `if (i > start && a[i] == a[i-1]) continue;` |
| Prefix | word search with a trie | `if (!node->next[ch]) return;` |
| Global precheck | word needs 2 B's, grid has 1 | count letters before searching |

`c/08_parentheses_and_pruning.c` shows combination sum: target 60 → 30,775
nodes without the `break`, 16,625 with it, same answers.

---

## 4. Complexity table

| Algorithm | Time | Extra space (stack depth) | Why |
|---|---|---|---|
| factorial / list sum | O(n) | O(n) | one call per element |
| tail-recursive version at -O2 | O(n) | O(1) | frame reused (not guaranteed) |
| fast power | O(log n) | O(log n) | halves n each call |
| naive Fibonacci | O(φ^n) ≈ O(1.618^n) | O(n) | tree with ~2·fib(n+1) nodes, depth n |
| memoised Fibonacci | O(n) | O(n) | each n solved once |
| Tower of Hanoi | O(2^n) | O(n) | 2^n − 1 moves, all needed |
| all subsets | O(n · 2^n) | O(n) | 2^n outputs × length n |
| all permutations | O(n · n!) | O(n) | n! outputs × length n |
| combinations C(n,k) | O(k · C(n,k)) | O(k) | outputs × length |
| N-Queens | O(n!) upper bound, far less in practice | O(n) | ≤ n choices row 0, ≤ n-1 row 1... |
| Sudoku | O(9^m), m = empty cells, worst case | O(m) | pruning makes typical puzzles tiny |
| word search | O(R·C · 4 · 3^(L-1)) | O(L) | first step 4 ways, then ≤ 3 (no going back) |
| generate parentheses | O(4^n / √n) (Catalan × n) | O(n) | every node leads to a valid result |
| explicit-stack iteration | same time as recursion | O(depth) on the **heap** | frames moved from stack to heap |

Space note: the **output** (all subsets, all permutations) is often far bigger
than the stack. If you only need to *process* each solution, process it in the
leaf instead of storing it.

---

## 5. C vs C++

| Concept | C | C++ |
|---|---|---|
| Recursive function | plain function | same; also `constexpr` → compiler runs it, `static_assert` checks it |
| Recursive lambda | n/a | pass `self`: `auto f = [](auto&& self, int n) {...}; f(f, 5);` or `std::function` (type-erased, slower) |
| Compile-time recursion | macros only | variadic templates; C++17 **fold expressions** `(xs + ...)` replace most of it |
| Path / partial solution | array + length | `std::vector::push_back` / `pop_back`, `std::string` |
| Used-flags | `char used[N]` / bitmask | `std::vector<bool>`, `std::bitset<N>`, integer masks |
| Results | fixed arrays or caller buffer | `std::vector<std::vector<int>>`, `std::set` for dedupe checks |
| Explicit stack | `malloc`ed array of structs | `std::stack<Frame>` / `std::vector<Frame>` |
| Memo table | array, or a hand hash map | `std::unordered_map`, `std::map` |
| STL equivalents | — | `std::next_permutation` / `prev_permutation` (permutations, handles duplicates), bitmask loops for subsets |

C++ gotchas seen in this lesson:

- `std::set<std::vector<T>>::insert({})` picks the `initializer_list`
  overload and inserts **nothing**. Write `insert(std::vector<T>{})`.
- A `std::unique_ptr` chain (linked list or degenerate tree) destroys itself
  **recursively**: `~Node` → `~unique_ptr` → `~Node`... 200,000 nodes deep
  can overflow the stack. Destroy iteratively (`cpp/07_*.cpp`).
- `constexpr` recursion has an implementation step limit (gcc
  `-fconstexpr-ops-limit`), so naive `fib(60)` won't compile.

---

## 6. Where it is used in the real world

- **Linux kernel avoids recursion.** Kernel stacks are 16 KB on arm64
  (`THREAD_SIZE`) and there's no guard against deep recursion in most paths, so
  kernel code converts recursion to iteration. The rbtree code
  (`lib/rbtree.c`) is iterative. Path lookup (`fs/namei.c`) follows symlinks
  with an explicit **stack of 40 levels max** (`MAXSYMLINKS`) instead of
  recursing. `checkpatch` and reviewers push back on recursive kernel code.
- **Device-tree parsing** (`libfdt`, used by U-Boot and the kernel on your
  BeagleBone AI-64) walks a tree of nodes. `fdt_next_node()` is **iterative**
  with a depth counter because U-Boot SPL runs on the R5 with a tiny stack.
- **Filesystem walks:** `nftw(3)`, `find`, `rm -r`, `du` recurse over
  directories. Glibc's `fts` uses an explicit structure so it doesn't blow the
  stack on deep trees.
- **Compilers and parsers:** recursive-descent parsers (GCC's C++ parser,
  Clang, most JSON parsers) are recursion. JSON libraries cap nesting depth
  (e.g. 512–1000) to stop malicious input crashing them with stack overflow.
- **Regex engines:** PCRE and backtracking regex are literally backtracking:
  try one alternative, undo on mismatch. Catastrophic backtracking
  (`(a+)+$`) is the exponential tree in action. RE2 and Rust's regex use
  automata to avoid it.
- **Constraint solvers:** SAT solvers (DPLL/CDCL, used in package managers
  like `apt`'s solver alternatives, `libsolv` in dnf/zypper, hardware
  verification) are backtracking with heavy pruning: unit propagation =
  "forced moves", conflict learning = remembering why a subtree failed.
- **Games:** chess engines use minimax / alpha-beta, which is recursion with
  bound pruning. Sudoku and crossword generators use backtracking with
  uniqueness counting.
- **Make / build systems:** dependency resolution is a recursive DFS over the
  dependency graph (with cycle detection).
- **GStreamer:** `gst_bin_iterate_recurse()` walks nested bins (bins contain
  elements and other bins). Caps negotiation and `decodebin`/autoplugging try
  candidate elements, and if linking fails they **back out** and try the next
  factory by rank. That's backtracking over plugin choices.
- **Embedded / MISRA C:** MISRA C:2012 Rule 17.2 forbids recursion in
  safety-critical code because stack use must be statically boundable. You
  convert to iteration with a bounded explicit stack.

---

## 7. Common mistakes and pitfalls

1. **Missing or unreachable base case** → infinite recursion → SIGSEGV
   (stack overflow). ASan reports it as `stack-overflow`.
2. **Not shrinking the problem** on some path (`f(n)` calling `f(n)`, or a
   halving step that gets stuck: `lo + (hi-lo)/2` with the wrong bound update
   can leave the range unchanged). Always test the smallest inputs: 0, 1, 2.
3. **Forgetting to un-choose.** The most common backtracking bug: a `used[i]`
   left set, a cell left marked `#`, a path element not popped. The program
   silently misses solutions. Tests: check that the shared state is
   **identical** before and after the top-level call (the C word-search
   program does this with `memcmp`).
4. **Un-choosing only part of the state** (clearing `used[i]` but not popping
   `path`, or one diagonal flag of three).
5. **Storing a pointer/reference to the path** instead of a **copy** in the
   result list. Every stored "solution" then shows the final (empty) state.
   In C, `memcpy` into the result; in C++, `out.push_back(path)` copies.
6. **Duplicate answers** when the input has duplicates. Sort + skip equal
   siblings (`i > start && a[i] == a[i-1]`). Note it's `i > start`, not
   `i > 0`: equal values may still appear together **down** one path.
7. **`continue` vs `break` in sorted pruning.** If candidates are sorted and
   `a[i] > remaining`, every later one is too: `break`. Using `continue` is
   correct but wastes time. Using `break` on **unsorted** input is a bug.
8. **Calling the recursive function twice when you meant once:**
   `power(x, n/2) * power(x, n/2)` turns O(log n) into O(n). Store it in a
   variable.
9. **Deep recursion on user-controlled input** (a 1,000,000-node linked list,
   a degenerate tree, a nested JSON document) → crash. Use an explicit stack
   or a depth limit.
10. **Expecting tail-call optimisation in C at -O0.** It's not guaranteed and
    not done in debug builds. Also impossible if a local's address escapes.
11. **Mutating a global used by an outer frame** (e.g. a single global `i` loop
    counter) instead of a local. Each frame needs its own loop variable.
12. **Exponential blow-up without noticing.** Before coding, estimate the
    tree: branching factor ^ depth. 2^30 = 10^9 is too slow; 2^20 is fine.

---

## 8. Files in this lesson

### C (`c/`)

| File | What it shows | Run |
|---|---|---|
| [01_recursion_basics.c](c/01_recursion_basics.c) | factorial, real stack-frame addresses, tail recursion → loop, halving recursion, fast power, palindrome, reverse, gcd | `make run P=dsa/15-recursion-and-backtracking/c/01_recursion_basics.c` |
| [02_recursion_tree_memo.c](c/02_recursion_tree_memo.c) | Fibonacci recursion tree, call counting, memoisation (O(φ^n) → O(n)), Tower of Hanoi with move legality checks | `make run P=dsa/15-recursion-and-backtracking/c/02_recursion_tree_memo.c` |
| [03_recursion_to_iteration.c](c/03_recursion_to_iteration.c) | flood fill with explicit stack, Hanoi with explicit frames + resume points, binary search tail recursion → loop | `make run P=dsa/15-recursion-and-backtracking/c/03_recursion_to_iteration.c` |
| [04_subsets_permutations_combinations.c](c/04_subsets_permutations_combinations.c) | the template on subsets, permutations (used[] and swap), combinations with pruning bound; checked vs bitmasks and next_permutation | `make run P=dsa/15-recursion-and-backtracking/c/04_subsets_permutations_combinations.c` |
| [05_n_queens.c](c/05_n_queens.c) | N-Queens with O(1) column/diagonal checks, node counts vs brute force, counts vs OEIS for N=1..12 | `make run P=dsa/15-recursion-and-backtracking/c/05_n_queens.c` |
| [06_sudoku_solver.c](c/06_sudoku_solver.c) | plain Sudoku backtracking vs bitmask + most-constrained-cell, validator, unsolvable detection | `make run P=dsa/15-recursion-and-backtracking/c/06_sudoku_solver.c` |
| [07_word_search.c](c/07_word_search.c) | grid word search with mark/unmark in place, path validation, 2000 random words | `make run P=dsa/15-recursion-and-backtracking/c/07_word_search.c` |
| [08_parentheses_and_pruning.c](c/08_parentheses_and_pruning.c) | generate parentheses (Catalan, vs 2^(2n) brute force), combination sum plain vs pruned node counts | `make run P=dsa/15-recursion-and-backtracking/c/08_parentheses_and_pruning.c` |

### C++ (`cpp/`)

| File | What it shows | Run |
|---|---|---|
| [01_recursion_modern.cpp](cpp/01_recursion_modern.cpp) | `constexpr` + `static_assert` recursion, recursive lambdas (self-param vs `std::function`), variadic templates vs fold, memo with `unordered_map` | `make run P=dsa/15-recursion-and-backtracking/cpp/01_recursion_modern.cpp` |
| [02_backtracking_template.cpp](cpp/02_backtracking_template.cpp) | one generic `backtrack<Problem>()` reused for subsets, permutations, unique permutations of a multiset, combinations; checked vs STL | `make run P=dsa/15-recursion-and-backtracking/cpp/02_backtracking_template.cpp` |
| [03_n_queens_bitmask.cpp](cpp/03_n_queens_bitmask.cpp) | readable N-Queens class returning boards vs bitmask counter, timings, N up to 13 | `make run P=dsa/15-recursion-and-backtracking/cpp/03_n_queens_bitmask.cpp` |
| [04_sudoku.cpp](cpp/04_sudoku.cpp) | `Sudoku` class with `std::optional` parse, MRV solve, solution counting with a limit (uniqueness proof) | `make run P=dsa/15-recursion-and-backtracking/cpp/04_sudoku.cpp` |
| [05_word_search_trie.cpp](cpp/05_word_search_trie.cpp) | Word Search I and II; trie-guided DFS with prefix pruning, cross-checked on random boards | `make run P=dsa/15-recursion-and-backtracking/cpp/05_word_search_trie.cpp` |
| [06_partition_and_combination_sum.cpp](cpp/06_partition_and_combination_sum.cpp) | palindrome partitioning, combination sum II (duplicates, dedupe rule), generate parentheses; all vs brute force | `make run P=dsa/15-recursion-and-backtracking/cpp/06_partition_and_combination_sum.cpp` |
| [07_recursion_to_iteration.cpp](cpp/07_recursion_to_iteration.cpp) | pre/in/post-order recursive vs `std::stack`, 200,000-deep tree, iterative destruction of a `unique_ptr` chain | `make run P=dsa/15-recursion-and-backtracking/cpp/07_recursion_to_iteration.cpp` |

Tip: to see real speed (the timings in `cpp/03` are with -O0 + sanitizers),
delete the old binary first, because make does not rebuild when only flags change:
`rm build/dsa/15-recursion-and-backtracking/cpp/03_n_queens_bitmask && make run P=dsa/15-recursion-and-backtracking/cpp/03_n_queens_bitmask.cpp OPT=1 SAN=0`.

---

## 9. Exercises

**Easy**

1. Write `int count_digits(unsigned n)` and `int sum_digits(unsigned n)` recursively. *Hint: base case n < 10.*
2. Print a string backwards using recursion without modifying it. *Hint: recurse first, print after.*
3. Convert `sum_array` in `c/01` into a tail-recursive version with an accumulator, then into a loop.
4. Count the calls made by naive `fib(n)` and prove `calls(n) = 2·fib(n+1) − 1` by induction. *Hint: calls(n) = 1 + calls(n−1) + calls(n−2).*

**Medium**

5. Letter combinations of a phone number ("23" → ad, ae, af, bd, ...). *Hint: depth = digit index, branches = letters of that digit.*
6. Subsets II: all distinct subsets of a multiset like {1,2,2}. *Hint: sort + skip equal siblings, as in combination sum II.*
7. Make `c/05_n_queens.c` exploit symmetry: only try columns `0..n/2-1` in row 0, double the count, handle the middle column for odd n. Check against the table.
8. Rewrite in-order traversal iteratively **without** a stack (Morris traversal). *Hint: temporarily thread the predecessor's right pointer back to the current node.*
9. Flood fill in `c/03` uses up to 4·R·C stack entries. Change it to mark cells **when pushed** instead of when popped and show the stack never exceeds R·C.

**Hard**

10. Knight's tour on a 6×6 board, with Warnsdorff's rule (move to the square with the fewest onward moves: MRV again) as the ordering heuristic. Compare node counts with and without it.
11. Sudoku generator: start from a random full grid, remove clues one by one while `count_solutions(2) == 1`. How few clues can you reach?
12. Expression add operators: given "123" and target 6, insert + - * between digits ("1+2+3", "1*2*3"). *Hint: carry the value of the last multiplicative term so you can undo it for `*`.*
13. Implement a tiny regex matcher supporting `.` and `*` with backtracking, then find an input that makes it exponential. Add memoisation on (i, j) and watch it become polynomial. That's chapter 17.

---

## 10. Remember forever

```
RECURSION  = base case (answer directly) + recursive case (shrink, call, combine).
             Trust the call (induction). Stack space = depth x frame size.
TREE       = branching^depth nodes. Same args repeated? -> memoise (DP).
TAIL CALL  = nothing after the call -> it is a loop; C does NOT guarantee TCO.
TO LOOP    = tail -> reassign params; otherwise explicit stack of frames
             {args, stage} where stage = the return address.
BACKTRACK  = for c in choices: if !valid continue; CHOOSE; EXPLORE; UN-CHOOSE.
             Undo exactly what you did. Copy the path when you record it.
PRUNE      = cut early: feasibility, bounds (sorted -> break), fewest-options-first,
             skip equal siblings (i > start && a[i]==a[i-1]).
N-QUEENS   = depth=row, flags col[c], diag[r+c], anti[r-c+n-1].
```

Review this card on the schedule in
[LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

---

## 11. Self-test questions

1. What exactly is stored in a stack frame, and why does `fact(100000)` crash while a loop doesn't?
2. Why is the Linux kernel so reluctant to use recursion? How big is a kernel stack on arm64?
3. Draw the recursion tree of `fib(5)`. How many calls? Which subtrees repeat?
4. What makes a call a tail call? Is `return 1 + f(n-1);` one? Will gcc remove it at `-O0`?
5. Convert in-order traversal to iteration. Why does post-order need an extra flag per frame?
6. State the backtracking template from memory. What goes wrong if you skip the un-choose step?
7. Subsets: why are there 2^n of them, and what are the two ways to draw the decision tree?
8. Why does permutation-by-swap not produce lexicographic order, while the `used[]` version does?
9. In N-Queens, why do `r+c` and `r-c` identify diagonals? How does the bitmask version shift them?
10. What is the MRV heuristic, and why does a cell with zero candidates let you prune immediately?
11. In combination sum II, why is the dedupe test `i > start` and not `i > 0`?
12. When does backtracking turn into dynamic programming, and why do most backtracking problems *not* benefit from memoisation?
