# 09 — Trees: binary trees, traversals, BST, AVL, red-black, std::map

> Prerequisites: [06-linked-lists](../06-linked-lists/) (nodes and pointers),
> [07-stacks-and-queues](../07-stacks-and-queues/) (the iterative traversals
> are a stack and a queue) and recursion.
> Next: [10-heaps](../10-heaps-priority-queues/README.md) (a tree stored in an
> array), [11-tries](../11-tries/README.md), [12-advanced-trees](../12-advanced-trees/README.md).

---

## 1. Why this exists

A sorted array finds things fast (binary search, O(log n)) but inserting in
the middle shifts everything (O(n)). A linked list inserts in O(1) once you are
at the spot, but finding the spot is O(n). A **tree** keeps the best of both:
it is a linked structure (cheap to insert and delete), shaped so that each step
of a search **throws away about half of what is left**, which is binary search
built out of pointers.

Trees also show up wherever data is already hierarchical: directories, the
device tree your BeagleBone boots with, the DOM, a compiler's syntax tree, a
GStreamer bin holding elements and other bins.

Once the tree is **balanced** (AVL, red-black), every operation is O(log n)
**guaranteed**, and it stays sorted, so it can answer "next bigger key",
"everything between a and b", or "the smallest". A hash table cannot answer
those. That is why the Linux kernel, `std::map` and every database index are
built on balanced trees.

---

## 2. Mental model

**Analogy: a company org chart.** One boss (the **root**). Each person has at
most two direct reports in a *binary* tree. People with no reports are
**leaves**. Your **depth** is how many bosses sit above you; the **height** of
the company is the longest chain of command from the top to an intern.

```
                 1            depth 0   <- root (no parent)
               /   \
              2     3         depth 1   <- 2 and 3 are siblings
             / \     \
            4   5     6       depth 2   <- 4 and 6 are leaves
               /
              7               depth 3   <- leaf; height of tree = 3 (edges)

   subtree rooted at 2 = {2, 4, 5, 7}       degree(2) = 2, degree(3) = 1
   ancestors of 7 = 5, 2, 1                  descendants of 2 = 4, 5, 7
```

**In memory**: each node is its own `malloc()` block holding a key and two
pointers. They are scattered across the heap, and the tree exists only
because of the pointers:

```
   root ──► ┌────────────────────────┐   struct Node {          // 64-bit:
            │ key=1 │ pad │ left │ right│     int key;            // off 0  (4 B + 4 pad)
            └─────────────────┬──────┬─┘     struct Node *left;  // off 8
                    ┌─────────┘      └──────┐ struct Node *right;// off 16
                    ▼                       ▼ };                  // 24 bytes
            ┌──────────────────┐   ┌──────────────────┐
            │ key=2 │ L  │  R  │   │ key=3 │NULL│  R  │ ...
            └──────────────────┘   └──────────────────┘
```

Every arrow is a **pointer chase**, which is probably a cache miss. That is why
a tree with 1 million keys (about 20 levels) is slower than O(log n) makes it
sound, and why B-trees (lesson 12) put hundreds of keys in one node.

### Terminology (learn these exact meanings)

| Term | Meaning | In the picture |
|---|---|---|
| root | the node with no parent | 1 |
| leaf | node with no children (degree 0) | 4, 7, 6 |
| internal node | node with at least one child | 1, 2, 3, 5 |
| degree | number of children (0, 1 or 2 in a binary tree) | deg(3) = 1 |
| depth(x) | edges from root down to x | depth(7) = 3 |
| height(x) | edges on the longest path from x down to a leaf | height(2) = 2 |
| height of tree | height(root); **empty = −1, one node = 0** in edges | 3 |
| level | all nodes at the same depth | level 1 = {2, 3} |
| subtree | a node plus all its descendants | |

> Height convention: some books count **nodes** (empty = 0, one node = 1).
> `c/01`–`c/05` count **edges**, while the AVL code (`c/06`, `cpp/03`) stores
> heights in **nodes** because then `h(NULL) = 0` with no special case.
> Both are fine. Just know which one you are using.

### Shapes

```
 FULL (0 or 2 kids)   COMPLETE (levels full,   PERFECT (full + all     DEGENERATE (each node
                      last filled from left)    leaves same depth)      ≤ 1 child = a list)
       1                     1                        1                  1
      / \                  /   \                    /   \                 \
     2   3                2     3                  2     3                 2
        / \              / \   /                  / \   / \                 \
       4   5            4   5 6                  4   5 6   7                 3

 BALANCED (height-balanced, AVL sense): at EVERY node |h(left) − h(right)| ≤ 1.
   The tree in the first picture is balanced (check each node!). Hang one more
   node under 7 and node 5 gets |1 − (−1)| = 2, so it is no longer balanced.
```

* perfect ⇒ complete and full; n = 2^(h+1) − 1 nodes, 2^h leaves.
* complete is exactly the shape a **heap** stores in an array (lesson 10).
* degenerate means height n − 1, so every O(h) operation becomes O(n).

---

## 3. How it works

### 3.1 Traversals: one walk, three moments

Walk around the tree with your left hand on it (the "Euler tour"). You
**pass every node three times**: on the way down, between its subtrees, and on
the way back up. The three depth-first orders differ only in **when you write
the node down**:

```
                 F                    pre-order  (1st pass, root-L-R):  F B A D C E G I H
               /   \                  in-order   (2nd pass, L-root-R):  A B C D E F G H I
              B     G                 post-order (3rd pass, L-R-root):  A C E D B H I G F
             / \     \                level-order (BFS, row by row):    F B G A D I C E H
            A   D     I
               / \   /
              C   E H
```

| Order | Use it for | Why |
|---|---|---|
| pre-order | copy / serialise a tree, print a directory listing | the parent is written before the children, so a reader can rebuild top-down |
| in-order | read a BST **sorted** | everything on the left is smaller and is written first |
| post-order | free a tree, compute sizes/heights, `du -s`, evaluate an expression tree | the children are finished before the parent |
| level-order | shortest path in edges, print by level, the "complete" check | a queue processes nodes nearest the root first |

**Recursive** versions are three lines each (`c/02`). Their hidden cost is the
**call stack**: depth = height. A degenerate tree of 10⁶ nodes overflows an
8 MB user stack, and a kernel stack is only 16 KB. So production code walks
trees **iteratively with an explicit stack**.

**Iterative in-order** ("dive left, pop, step right"), traced on a small BST:

```
        4          cur=4: push 4, push 2, push 1      stack [4 2 1]
       / \         pop 1 -> emit 1, cur = 1.right=∅   stack [4 2]
      2   6        pop 2 -> emit 2, cur = 3, push 3   stack [4 3]
     / \           pop 3 -> emit 3                    stack [4]
    1   3          pop 4 -> emit 4, cur = 6, push 6   stack [6]
                   pop 6 -> emit 6                    done: 1 2 3 4 6
```

* **Iterative pre-order**: pop, emit, push **right then left** (so left pops first).
* **Iterative post-order**: one stack plus a `prev` pointer: emit the top
  only when it has no right child or its right child was just emitted
  (`c/02`). Alternatively use two stacks: a root-R-L pre-order, reversed (`cpp/01`).
* **Level-order**: a queue. `width = queue size` at the start of a round is
  exactly one level.

**Morris traversal: O(1) extra space.** The stack is only needed to climb
back up. Morris borrows the unused `right == NULL` pointer of each node's
**in-order predecessor** as a temporary "go back up" link (a *thread*):

```
 step 1: at 4, pred(4) = 3          step 2: at 2, pred(2) = 1        step 3: at 1 (no left)
         make thread 3 -> 4                 make thread 1 -> 2               emit 1, follow thread
        4                                  4                                 to 2: thread found
       / \                                / \                                -> remove it, emit 2,
      2   6                              2   6                               go right to 3 ...
     / \                                / \
    1   3 ···> 4                       1   3 ···> 4
                                        ···> 2
```

The first visit to a node creates its thread and goes left. The second visit
comes back through the thread, deletes it (**the tree is restored**), emits
the node (in-order) and goes right. Pre-order emits at the first visit instead.
Every edge is walked at most 3 times, so it is O(n) time and O(1) space. The
catch: the tree is **modified while you walk it**, so no other thread may
read it at the same time.

### 3.2 Height, diameter, LCA: "ask your children, then combine"

Most tree problems are a post-order: get answers from left and right, then
combine them at the node.

```
 height(x)   = 1 + max(height(L), height(R)),  height(∅) = −1

 diameter    = longest path (in edges) between ANY two nodes. It goes up
               to one highest bend node X and down again:
                        X                 through X: (hL+1) + (hR+1)
                      /   \                         = hL + hR + 2
                    ...   ...             answer = max over all X.
               One pass returns height upward and keeps the best bend in a
               variable: O(n). (Recomputing height at every node: O(n²).)
               The path does NOT have to pass through the root.
```

**Lowest common ancestor** (deepest node that has both a and b below it,
where a node counts as its own ancestor):

```
              3          lca(t, a, b):
            /   \          if t is NULL, a or b  -> return t
           5     1         l = lca(left), r = lca(right)
          / \   / \        if l and r both non-NULL -> t is the split point: LCA
         6   2 0   8       else return whichever is non-NULL
            / \
           7   4         lca(6,4)=5   lca(5,1)=3   lca(5,4)=5   lca(7,8)=3
```

In a **BST** it is easier and O(h): walk down from the root. While a and b
are both smaller go left, while both are bigger go right; the first node
where they split (or that equals one of them) is the LCA.

### 3.3 Serialise / deserialise

Pointers mean nothing in another process. To save or send a tree you flatten
it. **One traversal alone does not determine the shape**: the trees `1 → left 2`
and `1 → right 2` both have pre-order "1 2". There are two fixes:

1. **Write the NULLs.** Pre-order with `#` for an empty slot:
   ```
          1
         / \          1 2 # # 3 4 # # 5 # #
        2   3         (n nodes always give exactly n+1 '#')
           / \
          4   5       reader: read token; '#' -> NULL, else node, then
                      read its left subtree, then its right. Same recursion,
                      so each token is consumed by the call that wrote it.
   ```
   The level-order form `[1,2,3,null,null,4,5]` (LeetCode style) works the same way with a queue.
2. **Two traversals, distinct keys:** pre-order + in-order (or post + in)
   determine the tree. Pre-order's first key is the root; its position in the
   in-order sequence splits left from right; recurse (`cpp/05`, O(n) with a hash
   map). Pre + post-order is **not** enough, because it cannot tell whether a
   single child is a left or a right child.

The Linux **device tree** is the same idea: the `.dts` tree is flattened into
a `.dtb` blob (FDT) that U-Boot hands to the kernel, which unflattens it back
into `struct device_node` pointers at boot.

### 3.4 Binary search tree (BST)

**Rule:** for every node, *everything* in its left subtree is smaller and
*everything* in its right subtree is bigger. So in-order = sorted.

**Search / insert**: compare, go left or right, stop at a match or a NULL
slot. In C, use a pointer-to-pointer `Node **link` so that inserting into an
empty tree needs no special case:

```
  insert 5:           8            link: &root -> &8.left -> &3.right -> &6.left
                     / \                 -> *link == NULL: *link = new node(5)
                    3   10
                   / \
                  1   6
                     /
                   (5)
```

**min / max**: keep going left / right. **Delete** has three cases:

```
 CASE 1 — leaf: just unlink it.
        8                       8
       / \      delete 4       / \
      3   10    ------->      3   10
       \                       \
        6                       6
       /
      4

 CASE 2 — one child: the child moves up into the hole.
        8                       8
       / \      delete 10      / \
      3   10    ------->      3   14
            \                     /
             14                 13
            /
          13

 CASE 3 — two children: you can't hang two orphans on one slot.
          Find the IN-ORDER SUCCESSOR s = min of the right subtree
          (it has NO left child — anything there would be smaller).
          Put s in the victim's place, then remove s from its old spot
          (which is case 1 or 2).

        8                          9                         9
       / \       delete 8         / \                       / \
      3   12     ------->        3   12     ------->       3   12
         /  \    succ(8) = 9        /  \    9's right          /  \
        9    14  (leftmost of      [9]   14  child 10 takes   10   14
         \        right subtree)     \       its old slot
          10                          10
```

The quick way in C is to **copy s's key** into the victim and delete s
(`c/05`). The proper way is to **relink the node s** into the victim's position
(`c/06`, `c/07`, `cpp/02`), so that pointers and references to other elements
stay valid. `std::map` guarantees this, and the kernel rbtree needs it because
the node is embedded in someone else's struct.

**Successor** (next bigger key):

```
  has a right subtree?   -> leftmost node of it.          succ(8) = 9  above
  no right subtree?      -> nearest ancestor you reach    succ(7) in 8/3/6/7:
                            from its LEFT side.              7 is right child of 6,
                                                             6 is right child of 3,
                                                             3 is LEFT child of 8 -> 8
  Without parent pointers: walk down from the root and remember the last node
  where you turned LEFT (c/05 bst_successor). With parent pointers: climb
  (c/07 rb_next). floor/ceil work the same way.
```

**Validate**: checking `left < node < right` only at each node is **wrong**:

```
        10
       /         5 < 12 locally fine, but 12 sits in 10's LEFT subtree.
      5          Correct check: pass the allowed open interval (lo, hi) down:
       \           root (−∞, +∞), go left -> (lo, node), go right -> (node, hi).
        12
```

**The BST's weak spot:** insert 1, 2, 3, …, n in order and every node becomes a
right child, so the tree has height n − 1 and is just a linked list. Random
insertion order gives about 3·log₂ n on average, but input is rarely random
(timestamps, sequence numbers and addresses all arrive sorted). That is why the
next two trees exist.

### 3.5 AVL tree: the strict self-balancer (Adelson-Velsky & Landis, 1962)

**Invariant:** at every node, `balance = h(left) − h(right)` is −1, 0 or +1.
Each node stores its height. After an insert or delete, walk back up the path
and recompute the heights. The **first** node that reaches ±2 is fixed with a
**rotation**.

**A rotation** changes the shape but **not the in-order sequence**, so the
BST rule still holds. Only three pointers move:

```
          y                                 x
         / \        rotate_right(y)        / \
        x   C       -------------->       A   y
       / \          <--------------          / \
      A   B         rotate_left(x)          B   C

   in-order before:  A x B y C        in-order after:  A x B y C   (same!)
   Only B changes parent. Update y's height first (it is now lower), then x's.
```

**The four cases.** Call the unbalanced node z, the child on its heavy side
y, and the grandchild on that path x. Heights are shown in brackets.

```
 ── LL: z is left-heavy, y is left-heavy (a straight line)  → rotate_right(z)

        z [3]                        y [2]
       /                            /     \
      y [2]       ----------->     x [1]   z [1]
     /
    x [1]                 insert order 3, 2, 1 creates this

 ── RR: mirror image (straight line to the right)           → rotate_left(z)

    z [3]                            y [2]
     \                              /     \
      y [2]       ----------->     z [1]   x [1]
       \
        x [1]                insert order 1, 2, 3

 ── LR: z is left-heavy, y is RIGHT-heavy (a zig-zag)
        → rotate_left(y) turns it into LL, then rotate_right(z)

        z                  z                    x
       /                  /                   /   \
      y        ---->     x        ---->      y     z
       \                /
        x              y
     (zig-zag)     rotate_left(y)          rotate_right(z)
                                     insert order 3, 1, 2

 ── RL: mirror      → rotate_right(y), then rotate_left(z)

    z                  z                        x
     \                  \                     /   \
      y      ---->       x        ---->      z     y
     /                    \
    x                      y
                                     insert order 1, 3, 2
```

How to remember which case you have: **read the path from z down to the new
node.** Same direction twice (LL / RR) means one rotation. A turn (LR / RL)
means two rotations, the first one straightens the zig-zag. A single rotation
on a zig-zag only flips it to the other side, still unbalanced. With subtrees
attached, an LR fix looks like this:

```
             z(+2)                          z                            x
            /    \                        /   \                       /     \
          y(−1)   D                      x     D                    y         z
         /    \        rotate_left(y)   / \       rotate_right(z)  / \       / \
        A      x       ------------>   y   C      ------------>   A   B     C   D
              / \                     / \
             B   C                   A   B
     in-order: A y B x C z D  — identical at every stage.
```

Facts that come up in interviews:
* **Insert** needs **at most one** (single or double) rotation. After it, the
  subtree is back to its old height, so nothing above changes.
* **Delete** may need a rotation at **every level** on the way up (O(log n)
  rotations), because a rotation can make the subtree shorter.
* Height ≤ 1.44 · log₂(n + 2). The worst case is a "Fibonacci tree", so AVL is
  the shorter of the two balanced trees here, which makes **lookups slightly faster**.

### 3.6 Red-black tree: the relaxed self-balancer

Each node is **red or black**, and five rules hold:

```
 1. Every node is red or black.
 2. The root is black.
 3. Every NULL leaf (NIL) counts as black.
 4. A red node has no red child          (no two reds in a row).
 5. Every path from a node down to its NIL leaves has the SAME number of
    black nodes (the "black-height").
```

**Why this means height ≤ 2 · log₂(n + 1):** by rule 5, every root-to-leaf
path has the same number b of black nodes, and by rule 4 reds can only be
slipped in between blacks. So the longest path is at most 2b while the
shortest is at least b. A tree whose shortest path is b has at least 2^b − 1
nodes, so b ≤ log₂(n + 1).

```
                 13(B)                    every path from 13 to NIL:
               /       \                  3 blacks (counting NIL)
           8(R)         17(R)
          /    \        /    \            red 8 has black kids 1, 11 ✓
       1(B)   11(B)  15(B)   25(B)
          \                  /   \
          6(R)            22(R)  27(R)
```

**Insert**: do a normal BST insert, colour the new node **red** (that cannot
break rule 5), then fix any red-red pair. With p = parent, g = grandparent and
u = uncle:

```
 Case 1 — uncle RED: recolour, move the problem up two levels.
          g(B)                    g(R)   <- may clash with its own parent:
         /    \                  /    \     repeat with z = g
       p(R)   u(R)    ---->    p(B)   u(B)
       /                       /
     z(R)                    z(R)

 Case 2 — uncle BLACK, z on the INSIDE (zig-zag): rotate p → becomes case 3.
          g(B)                    g(B)
         /    \                  /    \
       p(R)   u(B)    ---->    z(R)   u(B)
          \                    /
          z(R)               p(R)

 Case 3 — uncle BLACK, z on the OUTSIDE (line): rotate g, swap colours. Done.
          g(B)                    p(B)
         /    \                  /    \
       p(R)   u(B)    ---->    z(R)   g(R)
       /                                \
     z(R)                               u(B)
 Finally: paint the root black.
```

Insert does **at most 2 rotations**. Delete does **at most 3**. The rest is
O(log n) recolouring, which only flips one bit per node. **Delete** removes a
node like a BST does. If the removed node was black, one path is now a black
short. The fix-up treats that spot as carrying an "extra black" and has 4
cases (sibling red; sibling black with two black children, which pushes the
problem up; near nephew red; far nephew red, which finishes). They are drawn
in `c/07_rbtree.c`.

**AVL vs red-black: which one and why**

| | AVL | Red-black |
|---|---|---|
| height bound | 1.44 log₂ n | 2 log₂ n |
| lookup | slightly faster (shorter) | slightly slower |
| rotations per insert | ≤ 2 (one single or double) | ≤ 2 |
| rotations per delete | up to O(log n) | ≤ 3 |
| extra per node | height (a byte) | 1 colour bit (often hidden in a pointer's low bit) |
| typical use | read-heavy lookups, in-memory DB indexes | general-purpose maps, kernel, STL |

Red-black trees do a **bounded, small amount of restructuring** per update, so
the cost is predictable. This suits a kernel (updates with interrupts
disabled or spinlocks held), concurrent code and allocators. The kernel packs
the colour into the low bit of the parent pointer (`__rb_parent_color`),
because nodes are at least 4-byte aligned. That makes `struct rb_node` just 3
words.

### 3.7 Augmenting a tree

A node can store **extra data computed from its children**, provided every
rotation recomputes it bottom-up (lower node first). Examples:

* **subtree size** gives the k-th smallest and rank(x) in O(log n) (`cpp/03`). An
  `std::set` cannot do this: `std::distance` on its iterators is O(n).
* **max end-point of intervals in the subtree** gives an interval tree
  ([lesson 12](../12-advanced-trees/README.md)). The kernel's
  `rb_root_cached` + `interval_tree` work exactly this way (`include/linux/rbtree_augmented.h`).
* **cached leftmost node** gives O(1) min (`rb_root_cached`, used by the CFS
  scheduler).

### 3.8 std::map / std::set

`std::map`, `std::set`, `std::multimap` and `std::multiset` are red-black trees
(`_Rb_tree` in libstdc++'s `bits/stl_tree.h`). Things to know (`cpp/04`):

* sorted iteration; `lower_bound(k)` = first ≥ k, `upper_bound(k)` = first > k,
  `equal_range`. "floor" = `prev(upper_bound(k))`.
* `m[k]` **inserts** a default value if `k` is missing. Use `find`, `count`
  or `at` for lookups.
* `try_emplace` / `insert_or_assign` (C++17) say exactly what you mean.
* Iterators and references stay valid across inserts and erases of *other*
  elements, because nodes never move. That is unlike `vector` and
  `unordered_map`, which invalidate them on growth or rehash.
* Erase while iterating: `it = m.erase(it);`.
* Comparators must be a **strict weak ordering** (`<`, never `<=`). Two keys
  are "equal" when `!(a<b) && !(b<a)`.
* `std::less<>` (transparent) allows `find(string_view)` without building a temporary.
* `extract` / `merge` (C++17) move nodes between containers with no reallocation.

---

## 4. Complexity table

n = number of nodes, h = height.

| Operation | Plain BST avg | Plain BST worst | AVL / RB | Why |
|---|---|---|---|---|
| search / insert / delete | O(log n) | **O(n)** | **O(log n)** | one root-to-leaf path; balanced trees guarantee h = O(log n) |
| min / max | O(log n) | O(n) | O(log n), O(1) if cached | keep going left/right |
| successor / predecessor | O(log n) | O(n) | O(log n), amortised O(1) when iterating | right subtree's min, or climb |
| in-order walk (all n) | O(n) | O(n) | O(n) | every edge walked twice |
| k-th smallest / rank | O(n) | O(n) | O(log n) **if augmented with size** | skip whole subtrees by size |
| range query [a, b] with k results | O(log n + k) | O(n) | O(log n + k) | find a, then walk successors |
| build from sorted array | — | — | O(n) | middle element as root, recurse |

| Traversal | Time | Extra space |
|---|---|---|
| recursive pre/in/post | O(n) | O(h) call stack: O(n) for a degenerate tree |
| iterative with stack | O(n) | O(h) heap memory |
| level-order (queue) | O(n) | O(w), w = max width (up to n/2) |
| Morris | O(n) | **O(1)**, but it temporarily modifies the tree |

| Problem | Time | Note |
|---|---|---|
| height, size, leaves, balanced check | O(n) | one post-order; naive balanced check is O(n log n)–O(n²) |
| diameter | O(n) | return height, track hL + hR + 2 |
| LCA (binary tree / BST) | O(n) / O(h) | |
| serialise / deserialise | O(n) / O(n) | n + 1 null markers |
| pre + in-order → tree | O(n) | hash map of in-order positions |

Memory: one node per key, plus two pointers (three with a parent pointer).
On 64-bit that is 16–24 bytes of overhead per key, plus malloc headers.

---

## 5. C vs C++

| | C (`c/`) | C++ (`cpp/`) |
|---|---|---|
| node | `struct Node { int key; struct Node *left, *right; }` | `template<class T> struct Node { T value; std::unique_ptr<Node> left, right; }` |
| ownership | explicit `malloc`/`free`, post-order `tree_free` | `unique_ptr`: deleting the root frees the whole tree (RAII) |
| insert slot | `Node **link` | `std::unique_ptr<Node>* link` |
| rotation | 3 pointer assignments | 3 `std::move`s between `unique_ptr` slots |
| generic keys | `int`, or an intrusive node + `container_of` (kernel style) | templates + a `Compare` functor, like the STL |
| iteration | callback, or `rb_first`/`rb_next` | an iterator class enables range-for |
| ready-made | none in libc (`tsearch` from `<search.h>` exists but is clumsy); kernel `lib/rbtree.c` | `std::map`, `std::set`, `std::multimap`, `std::multiset` |

Two ownership styles appear in this lesson:

* **Owning nodes** (`cpp/02`, `cpp/03`): the tree allocates and frees nodes.
* **Intrusive nodes** (`c/07`, the Linux kernel, Boost.Intrusive): the user
  embeds `struct rb_node` inside their own struct and the tree only links
  them. There is no allocation inside the tree, an object can sit in several
  trees and lists at once, and you erase by pointer without searching.
  `container_of()` gets you from the node back to the user's struct.

C++ trap: the default `unique_ptr` destructor chain is **recursive**, so a
degenerate tree of 300,000 nodes can overflow the stack while it is being
destroyed. `cpp/01` and `cpp/02` show an iterative `clear()` that rotates left
children up and frees the root.

---

## 6. Where it is used in the real world

* **Linux kernel: `lib/rbtree.c`, `include/linux/rbtree.h`.** Red-black
  trees with intrusive nodes are everywhere:
  * **CFS scheduler** (`kernel/sched/fair.c`): runnable tasks sorted by
    `vruntime`, and the leftmost one (smallest virtual runtime) runs next. It
    uses `rb_root_cached`, so picking it is O(1) and enqueue/dequeue is O(log n).
    EEVDF, which replaced CFS in 6.6, still uses an augmented rbtree.
    `c/07` contains a mini version.
  * **hrtimers**: pending high-resolution timers sorted by expiry (a
    "timerqueue", which is an rbtree with a cached leftmost node).
  * **ext4 extents status tree**, epoll's set of watched fds, I/O schedulers
    (BFQ, mq-deadline sort requests by sector), and per-process VMAs until
    6.1 (now the **maple tree**, a B-tree variant).
  * **interval trees** on top of rbtrees for memory mappings (`i_mmap`).
* **C++ STL**: `std::map`, `std::set` and the multi versions.
  `std::unordered_map` is a hash table: faster point lookups but no order.
* **Java** `TreeMap`. **Databases**: on-disk indexes are B+ trees (lesson
  12). In-memory indexes and memtables use red-black trees, AVL trees or skip
  lists.
* **Device tree** (`.dts` → `.dtb`): a hardware tree serialised for the bootloader and unflattened by the kernel.
* **Filesystems**: directory hierarchies. btrfs is a B-tree filesystem.
* **Compilers**: abstract syntax trees, evaluated with post-order (children first).
* **GStreamer**: a pipeline is a tree of `GstBin`s and elements. State
  changes go through children in a defined order (sinks to sources), and
  `gst_bin_iterate_recurse()` walks it depth-first. GLib provides `GTree`, a
  balanced binary tree (AVL-based) with `g_tree_lower_bound` etc.
* **Networking**: routing tables (tries, lesson 11), and the Linux packet
  scheduler's HFSC class tree.
* **Games / graphics**: scene graphs, BSP trees, quadtrees and octrees.

---

## 7. Common mistakes and pitfalls

1. **Validating a BST locally** (`left < node < right` only). Pass (lo, hi)
   bounds down instead, and use `long long` or pointers for bounds so that
   `INT_MIN`/`INT_MAX` keys still work.
2. **Freeing a node before its children** (pre-order free). You lose the
   child pointers. Always free in post-order.
3. **Forgetting duplicates.** Decide on a policy: reject them (set), count
   them, or send them consistently to one side (multiset; ties go right,
   which keeps FIFO order, as CFS does).
4. **Off-by-one in height**: mixing "edges" and "nodes" conventions inside one
   program. `h(NULL)` must be −1 for edges and 0 for nodes.
5. **Recursing on data-dependent depth** in kernel/embedded code, or on
   untrusted input. A sorted input makes a plain BST into a list and blows
   the stack.
6. **Sorted input into a plain BST**: O(n²) total build time. Use a balanced
   tree, shuffle the input, or build from a sorted array in O(n).
7. **Rotations: updating heights in the wrong order.** The node that moved
   *down* must be updated first. Forgetting to update an augmented field
   (size, max) in a rotation is the same bug.
8. **AVL LR/RL handled with a single rotation**: the zig-zag just flips to
   the other side.
9. **Two-children delete by key copy** when other code holds pointers to
   nodes. Relink the node instead.
10. **Red-black delete: forgetting x may be NULL.** You need the parent
    tracked separately (`c/07` passes `xparent`), or a sentinel NIL node as
    in CLRS.
11. **Morris traversal and early exit**: if you `break` out of the loop, the
    temporary threads are left in place and the tree is corrupt.
12. **`std::map::operator[]` for lookups** silently inserts. A comparator
    using `<=` breaks the container.
13. **Iterator invalidation**: erasing the element an iterator points to
    invalidates *that* iterator. Use `it = m.erase(it)`.

---

## 8. Files in this lesson

| File | What it shows | Run |
|---|---|---|
| [c/01_binary_tree_basics.c](c/01_binary_tree_basics.c) | node layout (`sizeof`/`offsetof`), size/height/depth/leaves, full/complete/perfect/balanced/degenerate classifiers | `make run P=dsa/09-trees/c/01_binary_tree_basics.c` |
| [c/02_traversals.c](c/02_traversals.c) | pre/in/post recursive + iterative (explicit stack), level-order (queue), Morris in-order and pre-order; 2000 random trees cross-checked | `make run P=dsa/09-trees/c/02_traversals.c` |
| [c/03_height_diameter_lca.c](c/03_height_diameter_lca.c) | O(n) diameter vs O(n²) reference, LCA in binary tree vs path-based reference, BST LCA | `make run P=dsa/09-trees/c/03_height_diameter_lca.c` |
| [c/04_serialize.c](c/04_serialize.c) | pre-order with `#` and level-order `[1,null,...]` serialisation, round-tripped on random trees | `make run P=dsa/09-trees/c/04_serialize.c` |
| [c/05_bst.c](c/05_bst.c) | BST insert/search/delete (3 cases), min/max, successor/predecessor, floor/ceil, correct vs broken validation; 200000 ops vs presence array | `make run P=dsa/09-trees/c/05_bst.c` |
| [c/06_avl.c](c/06_avl.c) | AVL insert/delete, LL/RR/LR/RL rotations, invariants checked after every op | `make run P=dsa/09-trees/c/06_avl.c` |
| [c/07_rbtree.c](c/07_rbtree.c) | kernel-style intrusive red-black tree (`rb_link_node`, `rb_insert_color`, `rb_erase`, `rb_next`, `container_of`), 5 rules checked after every op, mini CFS scheduler | `make run P=dsa/09-trees/c/07_rbtree.c` |
| [cpp/01_traversals_iterators.cpp](cpp/01_traversals_iterators.cpp) | `unique_ptr` nodes, template visitors, `std::stack`/`std::queue`, an in-order **iterator** for range-for, iterative destruction of a 300000-node chain | `make run P=dsa/09-trees/cpp/01_traversals_iterators.cpp` |
| [cpp/02_bst_unique_ptr.cpp](cpp/02_bst_unique_ptr.cpp) | `BstMap<K,V,Compare>` with `unique_ptr`, delete by **relinking** (pointer stability), lower_bound/successor/range count; 200000 ops vs `std::map` | `make run P=dsa/09-trees/cpp/02_bst_unique_ptr.cpp` |
| [cpp/03_avl_order_statistics.cpp](cpp/03_avl_order_statistics.cpp) | `AvlSet<K,Compare>`: rotations as `unique_ptr` moves, subtree-size augmentation, `kth()` and `rank()` in O(log n), vs `std::set` | `make run P=dsa/09-trees/cpp/03_avl_order_statistics.cpp` |
| [cpp/04_std_map_set.cpp](cpp/04_std_map_set.cpp) | `std::map`/`set`/`multimap`/`multiset`: bounds, comparators, `operator[]` trap, erase-while-iterating, stability, `extract`/`merge`, physical-address → region lookup | `make run P=dsa/09-trees/cpp/04_std_map_set.cpp` |
| [cpp/05_tree_construction_problems.cpp](cpp/05_tree_construction_problems.cpp) | build from pre+in-order, stream serialisation, sorted array → balanced BST, max path sum (vs brute force), right-side view | `make run P=dsa/09-trees/cpp/05_tree_construction_problems.cpp` |

All programs check themselves (`assert`) against a simple reference on
random inputs and are clean under ASan/UBSan.

---

## 9. Exercises

**Easy**
1. Count nodes at depth k, and count nodes with exactly one child. *Hint:* pass depth down; post-order sum.
2. Mirror a tree (swap left/right everywhere) and check `inorder(mirror)` = reversed `inorder`.
3. Check whether two trees are identical, and whether a tree is symmetric. *Hint:* `sym(a, b) = a.key == b.key && sym(a.left, b.right) && sym(a.right, b.left)`.
4. In `c/05`, add `bst_count_range(lo, hi)` that skips subtrees outside the range.

**Medium**
5. Zig-zag level order (left-to-right, then right-to-left). *Hint:* two stacks, or a deque.
6. Kth smallest in a plain BST in O(h + k) using the iterative in-order and stopping early.
7. Convert a BST to a sorted **doubly linked list in place** (reuse left/right as prev/next). *Hint:* in-order with a `prev` pointer.
8. Add parent pointers to `c/05` and implement `successor(node)` without the root.
9. Implement iterative post-order with two stacks in C; then implement Morris **post-order** (harder: reverse the right edge of each left subtree).
10. Recover a BST where exactly two nodes' keys were swapped. *Hint:* in-order; find the one or two "drops".

**Hard**
11. Write `rb_erase` for a CLRS-style tree with a **sentinel NIL** node and compare the code with `c/07`'s NULL version.
12. Add subtree-size augmentation to `c/07` (update sizes in rotations and along the insert/erase paths) and implement `rb_kth()`.
13. Serialise a BST using **only pre-order without null markers** and rebuild it in O(n). *Hint:* the (lo, hi) bounds tell you where each key belongs.
14. A **treap** (BST by key + heap by random priority): implement split/merge and compare its height with AVL on sorted input.
15. Read `lib/rbtree.c` in the kernel tree you build for the BeagleBone and map each of its cases to the cases drawn in `c/07`.

---

## 10. Remember forever

Review this card on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

```
NODE       key + left + right. Height(empty) = -1 (edges). Depth = edges from root.
TRAVERSE   3 visits per node: pre = 1st, in = 2nd, post = 3rd. Level = queue.
           Iterative: stack. In-order = "dive left, pop, emit, go right".
           Morris: thread pred->right = cur, remove on 2nd visit. O(1) space.
COMBINE    most problems = post-order: ask children, combine (height, diameter hL+hR+2, LCA split).
BST        left < node < right for WHOLE subtrees. in-order = sorted. ops O(h).
DELETE     leaf: cut. one child: lift child. two: replace with successor (min of right).
VALIDATE   pass (lo, hi) down, never compare only with children.
AVL        |hL-hR|<=1. LL->R, RR->L, LR->L(child)+R, RL->R(child)+L. h<=1.44 log n.
RB         root black, no red-red, equal black count. h<=2 log n. <=2/3 rotations ins/del.
           Linux rbtree, std::map, CFS(vruntime, leftmost cached), hrtimers.
```

---

## 11. Self-test questions

1. Define height and depth. What are the height of an empty tree and of a single node in your convention?
2. Draw a tree that is complete but not full, and one that is full but not complete.
3. How many NULL child pointers does a binary tree with n nodes have? Why?
4. Which traversal frees a tree, which copies it, and which prints a BST sorted? Why each?
5. Write the iterative in-order traversal from memory. What does the stack hold at any moment?
6. How does Morris traversal get back up without a stack, and why does it leave the tree unchanged?
7. Why is the diameter `hL + hR + 2`, and why can't you assume the path goes through the root?
8. Why is one traversal not enough to rebuild a tree? Which pairs of traversals are enough?
9. Walk through deleting a node with two children. Why does the successor never have a left child?
10. Why is "left child < node < right child at every node" not a valid BST check?
11. Insert 1..7 into a plain BST and into an AVL tree. Draw both.
12. Name the four AVL cases, how you recognise each, and the rotations each needs. Why does LR need two?
13. Why does an AVL insert need at most one rebalancing, while a delete may need O(log n)?
14. State the five red-black rules and derive the 2·log₂(n+1) height bound.
15. Why does the Linux kernel use red-black trees rather than AVL? What is `rb_root_cached` for?
16. What does `container_of` do, and what does an intrusive tree buy you?
17. Why do `std::map` iterators survive inserts while `std::vector` iterators don't?
18. When would you choose `std::map` over `std::unordered_map`, and the other way round?
