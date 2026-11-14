# 1. What a graph is, and every type you will meet

[Back to the chapter](../README.md)

## The one-sentence definition

A **graph** `G = (V, E)` is a set of **vertices** (also called nodes) `V` and a
set of **edges** `E`, where each edge connects two vertices. That's the whole
definition. Everything else in this chapter adds rules on top of it.

```
   vertices: the things           edges: the relationships
   (cities, CPUs, packages,       (roads, buses, "depends on",
    GStreamer elements, people)    pad links, "follows")
```

Words you need from day one:

| Word | Meaning | In G1 below |
|---|---|---|
| `n = |V|` | number of vertices | 8 |
| `m = |E|` | number of edges | 9 |
| adjacent / neighbour | joined by an edge | 1 and 4 |
| degree `deg(v)` | number of edges touching v | deg(4) = 3 |
| path | a sequence of vertices, each joined to the next | 0-1-4-6 |
| simple path | a path that repeats no vertex | 0-1-4-6 yes; 0-1-0 no |
| cycle | a path that comes back to its start | 0-1-4-3-0 |
| reachable | there is a path from u to v | 7 from 0: yes |

```
   G1 — the graph most programs in this chapter start from

       0 ----- 1 ----- 2
       |       |       |
       3 ----- 4       5
               |       |
               6 ----- 7
```

**Handshake lemma:** the sum of all degrees is `2m`, because every edge has two
ends. (G1: 2+3+2+2+3+2+2+2 = 18 = 2·9.) Consequence: the number of odd-degree
vertices is always even. You will use this in the Euler section.

---

## Directed vs undirected

```
   undirected (a two-way road)      directed (a one-way street, an "arc")
       A ------ B                        A ------> B
   A-B means you can go both ways   A->B does NOT give you B->A
```

* **Undirected:** friendships, Ethernet cables, road with traffic both ways,
  "these two GPIOs are shorted together".
* **Directed:** "follows" on social media, web links, function calls (a call
  graph), `#include`, "package X depends on Y", a GStreamer **src pad -> sink
  pad** link (data flows one way), a one-way street.
* For directed graphs, degree splits into **in-degree** (arrows in) and
  **out-degree** (arrows out). Topological sort is built on in-degree.
* An undirected graph is stored as a directed one with both arcs `u->v` and
  `v->u`. That is why adjacency lists of undirected graphs have `2m` entries.

## Weighted vs unweighted

```
   unweighted: every edge "costs 1"     weighted: each edge carries a number
       A ------ B ------ C                  A --5-- B --2-- C
                                              \_____9______/
```

Weights can mean distance, time, latency, bandwidth, cost, probability,
capacity. **The choice of algorithm depends on the weights**:
no weights -> BFS; non-negative -> Dijkstra; negative -> Bellman-Ford;
only 0/1 -> 0-1 BFS. (See the decision chart in the main README.)

## Cyclic vs acyclic, and the DAG

```
   cyclic (has a cycle)          acyclic undirected = a forest/tree
     A ---> B                       A       D
     ^      |                      / \      |
     |      v                     B   C     E
     D <--- C
```

A **DAG** (directed acyclic graph) is a directed graph with no directed cycle.
It is *the* most useful special graph in systems work because a DAG is exactly
"a set of things with a valid order": build targets (make, ninja, Bazel),
package installs (apt, pip, cargo), task schedulers (Airflow, systemd's
`After=`/`Requires=` ordering), spreadsheet cells, compiler instruction
scheduling, neural-network computation graphs, and a GStreamer pipeline of
elements (data flows source -> sink and never loops back).

```
   a DAG: diamonds are fine (config.h reaches app two ways), loops are not

     config.h --+--> main.o --+
                |      ^      |
     main.c ----|------+      +--> app
                v             |
     util.c --> util.o -------+
```

A directed graph can be cyclic while its *undirected* version is not a forest,
and a DAG can have undirected cycles (the diamond above). Acyclic in the
directed sense = no way to follow arrows back to where you started.

## Connected, disconnected, strongly and weakly connected

**Undirected:**

```
   connected                 disconnected: 3 connected components
   0 --- 1 --- 2              0 --- 1     3 --- 4     5
         |                    |
         3                    2
```

A **connected component** is a maximal set of mutually reachable vertices.
"How many separate networks are there?" = "how many components?"

**Directed** graphs have two notions:

```
   strongly connected:              weakly connected (but NOT strongly):
   every vertex reaches every       connected if you ignore arrow directions,
   other one following arrows       but C cannot get back to A

      A ---> B                          A ---> B ---> C
      ^      |
      +----- C
```

* **Strongly connected component (SCC):** a maximal set where every vertex
  reaches every other along arrows. Collapsing each SCC into one node always
  gives a DAG (the *condensation*). Used for: finding groups of mutually
  recursive functions, circular module dependencies, deadlocked thread groups.
* **Weakly connected:** connected after replacing every arc by an undirected
  edge.

## Sparse vs dense

```
   m can be anything from 0 to n(n-1)/2 (undirected, simple)

   sparse: m ~ n          dense: m ~ n^2
   (roads, the web,       (distance tables between all cities,
    social networks,       a fully connected neural layer,
    dependency graphs)     small complete graphs)
```

Almost every real graph is **sparse**: a road junction joins ~4 roads whether
the map has 100 or 100 million junctions. That is why adjacency lists / CSR
(memory O(n + m)) beat adjacency matrices (memory O(n^2)) in practice, and why
`O((n + m) log n)` Dijkstra beats `O(n^2)` Dijkstra on real maps.

## Complete graph `K_n`

Every pair of vertices is joined: `m = n(n-1)/2`.

```
   K4:   0 ------- 1
         | \     / |
         |   \ /   |
         |   / \   |
         | /     \ |
         2 ------- 3
```

TSP inputs are usually complete graphs (you can drive from any city to any
other). Complete graphs are the worst case for edge-heavy algorithms.

## Bipartite

The vertices split into two sides L and R so that **every edge goes between
the sides**, never inside one.

```
   L (workers)      R (jobs)          equivalent test:
     w0 ------------- j0              "can I 2-colour the vertices so no
     w0 ------------- j1               edge joins two equal colours?"
     w1 ------------- j0              and also:
     w2 ------------- j1              "the graph has NO odd-length cycle"
```

Bipartite graphs model *assignments*: workers to jobs, video streams to
hardware encoders, students to projects, ads to slots. G1 above is bipartite
(sides {0,2,4,7} and {1,3,5,6}); adding edge 0-4 creates the triangle 0-1-4
and breaks it.

## Trees and forests are graphs

```
   tree: connected + acyclic             forest: acyclic, maybe disconnected
           0                                 0         5
          / \                               / \        |
         1   2      n vertices,            1   2       6
        / \          exactly n-1 edges
       3   4
```

Equivalent definitions of a tree on n vertices (any two imply the third):
connected; acyclic; exactly n-1 edges. Also: exactly one simple path between
any two vertices. Every BFS or DFS of a connected graph builds a **spanning
tree** (the tree edges), and the MST section finds the cheapest one. The
binary trees and heaps of earlier chapters are rooted trees with extra rules.

## Multigraphs and self-loops

```
   multigraph: parallel edges            self-loop: an edge u-u
      A ====== B   (two cables)              +--+
      A ------ C                             |  |
                                             A--+
```

* **Parallel edges** appear for real: two network links between the same
  routers, two bridges between the same banks (Koenigsberg!), two flights
  A->B at different prices. Algorithms that identify edges by `(u, v)` break
  on them; identify edges by an **edge id** instead (see the bridges program:
  a doubled edge is never a bridge).
* **Self-loops**: a state machine that stays in its state; a page linking to
  itself. A self-loop adds 2 to the degree in an undirected graph and is a
  cycle of length 1 in a directed one.
* A **simple graph** has neither. Most textbook algorithms assume simple
  graphs; check before you reuse one.

## Implicit graphs: grids and state spaces

Sometimes the graph is never stored. You only have a function
`neighbours(state)`.

```
   grid (each cell a vertex,             state space (each state a vertex)
   4 neighbours each, walls removed)
                                          sliding puzzle board  --move-->  board'
   . . # .                                Rubik's cube          --turn-->  cube'
   . # . .   neighbours(r,c) =            chess position        --move-->  position'
   . . . #   {(r±1,c),(r,c±1)} \ walls    camera pipeline config --change-> config'
```

A 1000 x 1000 image is a graph with a million vertices and you never build an
adjacency list: you compute neighbours on the fly (flood fill = BFS on
pixels; A* path-finding in games = search on a grid). A Rubik's cube has
4.3·10^19 states: you could never store that graph, but BFS / A* still work on
it because they only touch the states they reach.

---

## Recall

* A graph is vertices + edges. Directed? Weighted? Cyclic? Connected? Sparse?
  Answer those five questions first: they pick the algorithm for you.
* DAG = "things with a valid order". SCCs collapse any digraph into a DAG.
* Bipartite = 2-colourable = no odd cycle.
* Tree = connected + acyclic = n-1 edges.
* Real graphs are sparse -> adjacency lists / CSR.
* Implicit graphs: neighbours computed, not stored.

[Next: representations](02-representations.md)
