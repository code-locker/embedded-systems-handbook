# 4. Connectivity: components, union-find, cycles, bipartite, bridges, SCC

[Back to the chapter](../README.md) · Programs:
[04_union_find](../c/04_union_find.c), [05_cycles_and_bipartite](../c/05_cycles_and_bipartite.c),
[07_bridges_articulation](../c/07_bridges_articulation.c), [08_scc](../c/08_scc.c)
(each also in [cpp/](../cpp/))

---

## Connected components

"How many separate pieces does this graph have, and which piece is each
vertex in?" Run BFS or DFS from every vertex that is still unvisited; each
run labels one component. O(n + m).

```
   G3:  0 --- 1 --- 2      3 --- 4      5 --- 6      8      9
                                         \   /
                                           7
   comp = [0 0 0 1 1 2 2 2 3 4]   -> 5 components
```

BFS labelling is perfect when the graph is given all at once. When edges
**arrive one by one** and you keep asking "are a and b connected *now*?",
use union-find.

---

## Union-find (DSU, disjoint-set union)

### Mental model

Every set is a little tree; each element points to a parent; the **root
names the set**. Two elements are in the same set iff they have the same root.

```
   find(x): follow parent pointers to the root
   union(a, b): find both roots; hang one root under the other

   parent = [0 0 0 3 3 5 5 5 8 9]
                0        3       5       8   9
               / \       |      / \
              1   2      4     6   7
```

### Trace on G3 (exact output of `c/04_union_find.c`)

| operation | parent[0..9] | rank[0..9] | sets | note |
|---|---|---|---|---|
| start | 0 1 2 3 4 5 6 7 8 9 | 0 0 0 0 0 0 0 0 0 0 | 10 | |
| union(0,1) | **0** 0 2 3 4 5 6 7 8 9 | **1** 0 0 0 0 0 0 0 0 0 | 9 | equal ranks: 1 under 0, rank[0]++ |
| union(1,2) | 0 0 **0** 3 4 5 6 7 8 9 | 1 0 0 0 0 0 0 0 0 0 | 8 | find(1)=0 (rank 1) beats 2 (rank 0) |
| union(3,4) | 0 0 0 3 **3** 5 6 7 8 9 | 1 0 0 1 0 0 0 0 0 0 | 7 | |
| union(5,6) | 0 0 0 3 3 5 **5** 7 8 9 | 1 0 0 1 0 1 0 0 0 0 | 6 | |
| union(6,7) | 0 0 0 3 3 5 5 **5** 8 9 | 1 0 0 1 0 1 0 0 0 0 | 5 | |
| union(7,5) | unchanged | unchanged | 5 | **same root 5: this edge closes a cycle** |

That last line is the **DSU cycle test for undirected graphs**: an edge whose
ends already share a root is redundant — it closes a cycle. Kruskal's MST
uses exactly this to skip edges.

### The two tricks

**Union by rank (or size):** hang the *shorter* tree under the *taller* one.
Height only grows when two equal-height trees merge, and then the result has
at least twice the nodes. So height <= log2(n). (Without it: `union(i, i+1)`
for i = 0..1022 builds a 1023-long chain — the program prints exactly that.)

**Path compression:** after `find(x)` reaches the root, make every node on the
path point directly at the root.

```
   find(4) on a chain                after
        0                               0
        |                         / /  |  \ \
        1                        1 2   3   4
        |
        2       next find on any of them is 1 hop
        |
        3
        |
        4
```

### Why "nearly constant": the inverse Ackermann function, intuitively

With both tricks, any sequence of m operations on n elements costs
O(m · α(n)), where α is the inverse Ackermann function.

Intuition without the proof:

* Rank alone gives log n. Compression flattens paths *every time you look*,
  so a long path is only paid for once; afterwards those nodes are one hop
  from the root.
* Group nodes by rank into "towers": tower 1 = ranks up to 2, tower 2 = up to
  2^2 = 4, tower 3 = up to 2^2^2 = 16, tower 4 = up to 2^16 = 65536,
  tower 5 = up to 2^65536. A find either moves a node's parent pointer to a
  much higher tower (which can happen only a few times per node) or is one
  of a handful of steps per tower. The number of towers needed for n nodes is
  log*(n) ("how many times must I take log2 before I get <= 1"), and log*(n)
  <= 5 for any n below 2^65536. That is the older Hopcroft-Ullman bound.
* Tarjan proved the tighter α(n), which grows even more slowly than log*.

```
   n                                   α(n)
   3                                   1
   4 .. 7                              2
   8 .. 2047                           3
   2048 .. (a number vastly bigger     4
            than 10^80, the atoms in
            the observable universe)
```

So for every input you will ever have, each DSU operation is **at most ~4
pointer hops** amortised. Treat it as O(1). (The C++ program does a million
random unions and finds in a blink.)

### DSU vs BFS components

| | BFS/DFS labelling | DSU |
|---|---|---|
| graph given all at once | O(n + m), simplest | O(m α(n)) |
| edges arrive online, interleaved queries | rerun each time: O(q (n + m)) | **O(α(n)) per op** |
| deletions | fine (rebuild) | not supported (DSU only merges) |
| also gives paths / distances | yes | no — only "same set?" |

Uses: Kruskal's MST, "are these two network nodes connected yet" as links
come up, image segmentation (merge neighbouring pixels of similar colour),
percolation, equivalence of variables in compilers/type inference
(unification), grouping duplicate records.

---

## Cycle detection

### Undirected graphs

Two equivalent tests:

1. **DFS:** reaching an already-visited vertex through an edge other than the
   one you arrived by means a cycle. (Skip the arrival **edge id**, not the
   parent vertex — otherwise two parallel edges u=v, which do form a cycle,
   are missed. `c/05` checks this.)
2. **DSU:** an edge whose ends are already connected closes a cycle.

Quick sanity rule: a forest has exactly `m = n - components` edges. More
edges = at least one cycle.

```
   c/05 on G1: DFS goes 0,1,2,5,7,6,4 and from 4 sees 1 (visited, not its
   arrival edge) -> cycle 4 - 6 - 7 - 5 - 2 - 1 - 4
```

### Directed graphs: why two colours are not enough

```
   0 ----> 1 ----> 3        visited-only DFS from 0: 0, 1, 3, back to 0,
   |               ^        then 0 -> 3 finds 3 "visited" ... cycle?? NO.
   +---------------+        3 is BLACK (finished in another branch). This is
                            a DAG: two paths to 3, no loop.
```

Only an edge into a **GRAY** vertex (still on the recursion stack) proves a
cycle: you are standing below v and can reach v again.

```
   G2 with 3 -> 0:  stack 0 -> 1 -> 3, then 3 -> 0 and 0 is GRAY
   cycle: 0 -> 1 -> 3 -> 0   (walk parents from 3 back up to 0)
```

Alternative: Kahn's algorithm (topological sort) — if it cannot output all
vertices, there is a cycle.

Real uses: deadlock detection on a wait-for graph (thread T1 waits for a lock
held by T2 ...: a cycle = deadlock), Linux **lockdep** (a cycle in the lock
ordering graph = potential deadlock, reported before it happens), circular
`#include` or package dependencies, `make: Circular dependency dropped`.

---

## Bipartite check (2-colouring)

Colour the start A; colour every neighbour the opposite colour (BFS level
parity). If an edge ever joins two vertices of the same colour, there is an
**odd cycle** and the graph is not bipartite.

```
   G1 from 0 (trace from c/05):
     colour 1 := B (neighbour of 0)      A = {0, 2, 4, 7}
     colour 3 := B (neighbour of 0)      B = {1, 3, 5, 6}
     colour 2 := A (neighbour of 1)
     colour 4 := A (neighbour of 1)      every edge goes A-B: bipartite
     colour 5 := B (neighbour of 2)
     colour 6 := B (neighbour of 4)
     colour 7 := A (neighbour of 5)

   G1 + edge 0-4:  0=A, then 1,3,4 = B, then edge 1-4 is B-B -> conflict
                   odd cycle 0 - 1 - 4 - 0 (length 3)
```

Why odd cycles are exactly the obstacle: walking around a cycle flips colour
on every step; you return to the start with the same colour only after an
even number of steps.

Uses: matching problems (only bipartite graphs have the simple
augmenting-path matching), scheduling with two shifts, "can these items be
split into two groups with no conflict inside a group", checking a graph is
a valid two-sided assignment before running matching.

---

## Bridges and articulation points (Tarjan's low-link)

* **Articulation point (cut vertex):** removing it disconnects the graph.
* **Bridge (cut edge):** removing it disconnects the graph.

They are the **single points of failure** of a network: a router whose loss
splits the network, the only cable between two buildings, the one CAN
gateway between two buses.

```
   G4
     0               5
     | \           / |
     |   1 - 3 - 4   |      bridges: 1-3, 3-4
     | /           \ |      articulation points: 1, 3, 4
     2               6
```

### The low-link idea

During DFS, record `disc[u]` (discovery time). Define

```
   low[u] = the smallest disc[] reachable from u's DFS subtree using
            tree edges downward and at most ONE back edge upward
```

"How high can my subtree climb without using the edge to my parent?"

For a tree edge `u -> v` (v is a child):

* `low[v] >  disc[u]`: v's subtree cannot even reach u by another route ->
  **u-v is a bridge**.
* `low[v] >= disc[u]`: v's subtree cannot get above u -> removing u cuts it
  off -> **u is an articulation point** (if u is not the root).
* Root rule: the DFS root is an articulation point iff it has >= 2 DFS
  children (the children's subtrees can only connect through the root).

### Trace on G4 from 0 (exact output of `c/07_bridges_articulation.c`)

DFS order 0, 1, 2 (2 sees 0: back edge), then 1 -> 3 -> 4 -> 5 -> 6 (6 sees 4: back edge).

| event | check | result |
|---|---|---|
| back at 1 from child 2 | low[2]=0 vs disc[1]=1 | 0 < 1: not AP, not bridge |
| back at 5 from child 6 | low[6]=4 vs disc[5]=5 | not AP |
| back at 4 from child 5 | low[5]=4 vs disc[4]=4 | 4 >= 4: **4 is AP**; 4 > 4 false: 4-5 not a bridge |
| back at 3 from child 4 | low[4]=4 vs disc[3]=3 | **3 is AP**, 4 > 3: **3-4 is a bridge** |
| back at 1 from child 3 | low[3]=3 vs disc[1]=1 | **1 is AP**, **1-3 is a bridge** |
| back at 0 (root) from child 1 | root has only 1 child | 0 is not AP |

```
   vertex : 0  1  2  3  4  5  6
   disc   : 0  1  2  3  4  5  6
   low    : 0  0  0  3  4  4  4
```

Read the `low` row as "the earliest ancestor I can escape to":
the left triangle escapes to 0, the right triangle only to 4, vertex 3 only
to itself.

Pitfall checked by the programs: add a second (parallel) 3-4 edge — a
redundant cable — and 3-4 stops being a bridge. That only works if DFS skips
the **arrival edge id**, not "the parent vertex".

O(n + m), one DFS. Cross-checked against "delete the vertex/edge and count
components" on 500 random multigraphs.

---

## Strongly connected components (SCC)

In a directed graph, an SCC is a maximal set in which every vertex reaches
every other. Condensing each SCC to one node always yields a **DAG**.

```
   G5:
     +----------+          +----------+          +--------+
     | 0 -> 1   |          | 3 -> 4   |          | 6 <-> 7|
     |  ^   |   | --1->3-> |  ^   |   | --5->6-> |        |
     |   \  v   |          |   \  v   |          +--------+
     |     2    |          |     5    |
     +----------+          +----------+
        SCC A                 SCC B                 SCC C

   condensation:  A -> B -> C
```

### Kosaraju (two DFS passes)

1. DFS the graph, record vertices in order of **finish time**.
2. Reverse every edge (transpose).
3. DFS the transpose, starting from vertices in **decreasing** finish time.
   Each DFS tree is one SCC.

Why it works: the vertex that finishes last lies in a *source* SCC of the
condensation (nothing outside points into it, else that outside vertex would
have finished later). In the transposed graph that SCC becomes a *sink*:
a DFS started there cannot leak out, so it collects exactly that SCC. Remove
it, repeat.

Trace (exact output of `c/08_scc.c`):

```
   pass 1 finish order : 2 7 6 5 4 3 1 0
   pass 2 on transpose, in DECREASING finish time:
     start 0 -> SCC 0 = { 0 1 2 }        (transpose: 0->2->1->0, cannot reach 3..7)
     start 3 -> SCC 1 = { 3 4 5 }        (1 already taken)
     start 6 -> SCC 2 = { 6 7 }
```

### Tarjan (one DFS + a stack)

Same `disc`/`low` as bridges, but only counting vertices **still on the
stack** (a vertex leaves the stack when its SCC is complete). When a vertex
finishes with `low[u] == disc[u]`, it is the root of an SCC: pop the stack
down to u.

```
   disc order 0..7 = 0 1 2 3 4 5 6 7
     low[6]==disc[6]==6 -> pop SCC 0 = { 7 6 }
     low[3]==disc[3]==3 -> pop SCC 1 = { 5 4 3 }
     low[0]==disc[0]==0 -> pop SCC 2 = { 2 1 0 }
```

Tarjan emits SCCs in **reverse topological order** of the condensation
(sinks first); Kosaraju in topological order.

| | Kosaraju | Tarjan |
|---|---|---|
| passes | 2 DFS + build transpose | 1 DFS |
| extra memory | the transposed graph | a stack + low[] |
| output order | topological | reverse topological |
| ease of remembering | easiest | slightly trickier (on-stack rule) |

Both O(n + m). Uses: 2-SAT solvers, finding mutually recursive function
groups (compilers), circular module / package dependencies, groups of
threads deadlocked on each other, "which web pages form a closed cluster"
(PageRank spider traps), simplifying a state machine.

[Previous: traversals](03-traversals.md) · [Next: ordering](05-ordering.md)
