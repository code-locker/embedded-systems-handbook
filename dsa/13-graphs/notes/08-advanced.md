# 8. Advanced: max flow, min cut, matching, Euler, TSP

[Back to the chapter](../README.md) · Programs:
[14_max_flow_matching](../c/14_max_flow_matching.c), [15_euler_hierholzer](../c/15_euler_hierholzer.c),
[16_tsp_bitmask](../c/16_tsp_bitmask.c) (each also in [cpp/](../cpp/))

---

## Max flow (Edmonds-Karp)

### The problem

A directed network of pipes; each arc has a **capacity**. Push as much as
possible from source s to sink t. Flow on an arc may not exceed its
capacity, and at every other vertex flow in = flow out (conservation).

```
   G10 (CLRS fig. 26.1), s = 0, t = 5

            16             12
     0 ----------> 1 -------------> 3
     |             ^            _.' ^ \
     |13          4|     9  _.-'    |7 \ 20
     |             |   _.-'         |   v
     |             |<-'             |    5
     +-----------> 2 -------------> 4 ---^
                           14          4

   arcs: 0->1:16 0->2:13 1->3:12 2->1:4 2->4:14 3->2:9 3->5:20 4->3:7 4->5:4
```

### The residual graph and the "undo" edge

Greedy "find a path, push the bottleneck, repeat" can get stuck in a bad
choice. The fix: pushing f units along u->v also creates f units of capacity
on the **reverse** arc v->u. A later path may use that reverse arc, which
means "cancel f units of the earlier flow and send them elsewhere".

```
   before pushing 5 on u->v (cap 8):     after:
        u ---8---> v                       u ---3---> v      (8 - 5 left)
                                           u <---5--- v      (5 can be undone)
```

**Ford-Fulkerson:** while the residual graph has an s->t path with spare
capacity (an *augmenting path*), push its bottleneck along it.
**Edmonds-Karp:** always pick the *shortest* such path (BFS). That bounds
the number of rounds by O(VE), total O(V E^2), independent of capacity
values.

Trace (exact output of `c/14`):

| round | augmenting path (BFS, fewest edges) | bottleneck | total |
|---|---|---|---|
| 1 | 0 -> 1 -> 3 -> 5 | 12 (1->3) | 12 |
| 2 | 0 -> 2 -> 4 -> 5 | 4 (4->5) | 16 |
| 3 | 0 -> 2 -> 4 -> 3 -> 5 | 7 (4->3) | 23 |
| 4 | no path left | | **23** |

Final flow per arc (`cpp/14`): 0->1 12/16, 0->2 11/13, 1->3 12/12,
2->4 11/14, 3->5 19/20, 4->3 7/7, 4->5 4/4 (2->1 and 3->2 unused).

### Max-flow = min-cut

A **cut** (S, T) splits vertices with s in S and t in T; its capacity is the
sum of capacities of arcs from S to T. Every unit of flow must cross every
cut, so flow <= any cut. When Ford-Fulkerson stops, let S = vertices still
reachable from s in the residual graph. Every S->T arc is full (else the
target would be reachable) and every T->S arc carries no flow — so the flow
**equals** this cut's capacity. Hence max flow = min cut.

```
   S = {0, 1, 2, 4}     T = {3, 5}
   cut arcs: 1->3 (12) + 4->3 (7) + 4->5 (4) = 23 = max flow
```

The min cut is the **bottleneck** of the network — which links to upgrade, or
which links an attacker must cut to disconnect s from t. Uses: network
capacity planning, image segmentation (graph cuts: foreground vs
background), project selection, baseball elimination, airline scheduling.

### Implementation styles

* `c/14`: capacity **matrix** — the reverse arc is simply `cap[v][u]`.
  O(V^2) memory, O(V) neighbour scan; fine for V up to a few hundred.
* `cpp/14`: edge vector with **paired edges** — edge `i` and edge `i ^ 1` are
  each other's reverse, adjacency lists hold edge indices. O(V + E) memory;
  this is how production code (and Dinic's algorithm, the faster successor)
  is written.

---

## Bipartite matching

Pair left vertices with right vertices along edges, each vertex used at most
once, as many pairs as possible.

```
   4 camera streams (L) -> 4 hardware blocks (R); x = "this stream can use it"

              R0    R1    R2    R3          as a graph: an edge for every x
        L0    x     x                       L0-R0  L0-R1
        L1    x                             L1-R0
        L2          x     x                 L2-R1  L2-R2
        L3                x     x           L3-R2  L3-R3
```

**As max flow:** s -> every L (cap 1), L -> R for allowed pairs (cap 1),
every R -> t (cap 1). Integral max flow = maximum matching.

**Kuhn's augmenting-path algorithm** (the short version of the same idea):
for each left vertex, try to find a free right vertex; if the one you want
is taken, ask its owner to move to another of *its* options, recursively.

Trace (exact output of `c/14`):

```
 augment from L0:
    L0 tries R0 (free: take it)
 augment from L1:
    L1 tries R0 (taken: ask its owner to move)
      L0 tries R1 (free: take it)              <- L0 moves R0 -> R1, L1 gets R0
 augment from L2:
    L2 tries R1 (taken: ask its owner to move)
      L0 tries R0 (taken: ask its owner to move)
    L2 tries R2 (free: take it)
 augment from L3:
    L3 tries R2 (taken: ask its owner to move)
      L2 tries R1 (taken: ask its owner to move)
        L0 tries R0 (taken: ask its owner to move)
    L3 tries R3 (free: take it)
 matching size 4: L1-R0 L0-R1 L2-R2 L3-R3
```

O(V E). (Hopcroft-Karp does it in O(E sqrt V).) **König's theorem:** in a
bipartite graph, max matching = minimum vertex cover. Uses: assigning jobs
to workers/cores/accelerators, ride-hailing driver-rider pairing, stable
pairing problems (with preferences: Gale-Shapley), register/resource
assignment, scheduling exams to rooms.

---

## Euler path and circuit (Hierholzer)

An **Euler path** uses every **edge** exactly once (a Hamiltonian path uses
every **vertex** once — that one is NP-hard; Euler is easy).

### Existence (undirected)

Every time a walk passes *through* a vertex it uses two of its edges. So:

* **Circuit** (start = end): all edges in one connected piece and **every
  degree is even**.
* **Path** (start != end): connected and **exactly two odd vertices** — start
  at one, finish at the other.
* More than two odd vertices: impossible. (Handshake lemma: the count of odd
  vertices is always even, so it's 0, 2, 4, ...)

Directed: circuit iff in-degree = out-degree everywhere; path iff one vertex
has out = in + 1 (start) and one has in = out + 1 (end).

```
   Koenigsberg (1736) as a multigraph       the "house": draw it in one stroke
                C                                 4
              // \\                              / \\
             A---D        deg A=5, B=3,         3---2
              \\ /        C=3, D=3: four       | X |     deg: 0:3 1:3 2:4 3:4 4:2
                B         odd -> impossible     0---1    two odd (0, 1): a PATH 0 ... 1
```

(Euler's proof that the Koenigsberg walk is impossible is usually called the
birth of graph theory.)

### Hierholzer's algorithm

Walk unused edges from the start until you are stuck. You can only get stuck
at the end vertex (every other vertex had an even number of unused edges).
If some vertex on the walk still has unused edges, start a sub-tour there
and splice it in. With a stack: push when you walk an edge, pop to the output
when the top has no unused edges; the output comes out reversed. O(E).

Trace on the house (edge order chosen so a splice is needed; exact output
of `c/15`):

```
   first walk: 0 -> 1 -> 3 -> 0 -> 2 -> 1   stuck at 1 (all of 1's edges used)
     stuck at 1 -> emit.   stack: 0 1 3 0 2         output(reversed): 1
   back at 2: it still has unused edges -> sub-tour 2 -> 3 -> 4 -> 2
     stuck at 2 -> emit.   stack: 0 1 3 0 2 3 4     output(reversed): 1 2
     stuck at 4 -> emit.   stack: 0 1 3 0 2 3       output(reversed): 1 2 4
     ... everything else pops
   trail: 0 1 3 0 2 3 4 2 1     (8 edges, each once; sub-tour spliced in at 2)
```

Uses: drawing/plotting/CNC/laser paths without lifting the tool, snow-plough
and street-sweeper routes (Chinese postman = Euler after duplicating
edges), **DNA sequence assembly** (Euler paths in a de Bruijn graph of k-mers),
**de Bruijn sequences** (`cpp/15` builds `0100110101111000`, which contains
every 4-bit pattern exactly once, cyclically — used in absolute rotary
encoders and structured-light 3D scanning patterns).

---

## TSP, NP-hardness, and the bitmask DP

**Travelling salesman:** shortest tour that visits every city exactly once
and returns. It looks like the Euler problem but is about vertices, and that
changes everything.

### What NP-hard means here (intuition)

* Nobody knows an algorithm that solves every TSP instance in time
  polynomial in n (like n^3). Finding one would prove P = NP and break a
  lot of cryptography's assumptions; most experts believe it is impossible.
* "NP-hard" = at least as hard as every problem in NP: if you could solve
  TSP fast, you could solve SAT, scheduling, register allocation (graph
  colouring), Hamiltonian path... fast.
* Practical consequence: **exact** algorithms are exponential, so you either
  keep n small, or accept **approximate** answers (heuristics: nearest
  neighbour, 2-opt, Christofides for metric TSP, LKH, OR-tools).

### Held-Karp: O(2^n · n^2) instead of O(n!)

Key observation: when you are halfway through a tour, the cheapest way to
finish depends only on **which cities are done** and **where you are now**
— not on the order you visited them in. So:

```
   dp[mask][v] = cheapest path that starts at 0, visits exactly the cities
                 in the bit set `mask`, and ends at v

   dp[{0}][0] = 0
   dp[mask | {w}][w] = min over v in mask of  dp[mask][v] + d[v][w]
   answer = min over v of  dp[all][v] + d[v][0]
```

The 4-city example (exact output of `c/16`):

```
          0 ---10---- 1         d =  0 10 15 20
          | \       / |             10  0 35 25
         15  20   25  35            15 35  0 30
          |     X     |             20 25 30  0
          2 ---30---- 3

   dp[mask][end]:
     {01} end 1 :  10        {013} end 1 :  45 (from 3)    {0123} end 1 :  70 (from 3)
     {02} end 2 :  15        {013} end 3 :  35 (from 1)    {0123} end 2 :  65 (from 3)
     {03} end 3 :  20        {023} end 2 :  50 (from 3)    {0123} end 3 :  75 (from 1)
     {012} end 1 :  50       {023} end 3 :  45 (from 2)
     {012} end 2 :  45

   close the tour: 70+10 = 80, 65+15 = 80, 75+20 = 95  ->  optimal 80
   tour 0 -> 2 -> 3 -> 1 -> 0  (15 + 30 + 25 + 10)
```

| n | brute force (n-1)! | Held-Karp 2^n · n^2 |
|---|---|---|
| 5 | 24 | 800 |
| 10 | 3.6·10^5 | 1.0·10^5 |
| 15 | 8.7·10^10 | 7.4·10^6 |
| 20 | 1.2·10^17 | 4.2·10^8 |

n = 16 takes ~80 ms in `cpp/16` (with sanitizers on); n = 20-25 is the
practical ceiling (memory 2^n · n entries). The same "set of done things +
current position" bitmask DP solves many small assignment/ordering problems.

**Greedy is not optimal:** `c/16` builds a 5-city instance where "always go
to the nearest unvisited city" walks a cheap chain and then must pay 50 to
get home: greedy 54, optimal 10.

[Previous: MST](07-mst.md) · [Next: graphs in the real world](09-real-world.md)
