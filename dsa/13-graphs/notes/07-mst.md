# 7. Minimum spanning tree: Kruskal and Prim

[Back to the chapter](../README.md) · Programs:
[c/13_mst_kruskal_prim.c](../c/13_mst_kruskal_prim.c),
[cpp/13_mst_kruskal_prim.cpp](../cpp/13_mst_kruskal_prim.cpp)

## The problem

Connect all n vertices of a weighted undirected graph using a subset of
edges with **minimum total weight**. The result has no cycles (a cycle's most
expensive edge could be dropped) and connects everything, so it is a tree
with exactly n-1 edges: a **minimum spanning tree**.

Wire up buildings with the least cable, connect sensors with the cheapest
links, design a power grid or a PCB ground net, approximate TSP, cluster
data. Note what MST is **not**: it does not give shortest paths between
vertices (that is Dijkstra's job; the two trees usually differ).

```
   G9 (A..G = 0..6)                          MST (weight 39)

            7         8                              7
       0 ------ 1 ------ 2                     0 ------ 1        2
        \      / \      /                       \        \      /
        5\   9/   \7   /5                       5\        \7   /5
          \  /     \  /                           \        \  /
           3 ------ 4                              3        4
            \  15  / \                              \        \
            6\   8/   \9                            6\        \9
              \  /     \                              \        \
               5 ------ 6                              5        6
                   11
```

## The cut property — the one fact both algorithms rely on

Split the vertices into any two groups S and V-S (a **cut**). The lightest
edge crossing the cut belongs to some MST.

Why: take any spanning tree T without that edge e = (u, v). T has a path
from u to v; that path must cross the cut somewhere, through some edge e'
that is at least as heavy as e. Swap e' for e: still a spanning tree, not
heavier. So an MST containing e exists.

```
     S = {0, 1, 3}                    V - S = {2, 4, 5, 6}
     +------------+                   +------------+
     |  0 ---- 1  | ----- 1-2 : 8 --- |  2      4  |
     |   \        | ----- 1-4 : 7 --- |            |
     |    3       | ----- 3-4 : 15 -- |  5      6  |
     +------------+ ----- 3-5 : 6 --- +------------+
                          ^
                          lightest edge crossing this cut: safe to take
```

(The *cycle property* is the mirror image: the heaviest edge on any cycle is
not needed.) Kruskal and Prim are just two strategies for picking cuts.

## Kruskal: cheapest edges first, skip cycles

```
   sort edges by weight
   for each edge (u, v) in that order:
       if find(u) != find(v):   # different components (DSU)
           take it; union(u, v)
       # else: u and v already connected -> this edge would close a cycle
   stop after n-1 edges
```

The cut used: "the component containing u" vs everything else; the edge is
the lightest one leaving that component (all lighter ones were already
processed).

Trace (exact output of `c/13`):

| weight | edge | decision |
|---|---|---|
| 5 | 0-3 | TAKE |
| 5 | 2-4 | TAKE |
| 6 | 3-5 | TAKE |
| 7 | 0-1 | TAKE |
| 7 | 1-4 | TAKE (joins {0,1,3,5} with {2,4}) |
| 8 | 1-2 | skip: 1 and 2 already connected (1-4-2) |
| 8 | 4-5 | skip: already connected |
| 9 | 1-3 | skip: already connected |
| 9 | 4-6 | TAKE — 6 edges, done |

Total 5+5+6+7+7+9 = **39**. O(E log E) for the sort + O(E α(V)) for DSU.
Kruskal shines on sparse graphs and when edges are already sorted or come
from an edge list.

**Bonus — clustering:** stop Kruskal when k components remain. The result
is single-linkage clustering: the k groups whose closest members are
farthest apart. `cpp/13` splits 8 sensors into {0,1,2} {3,4,5} {6,7}.

## Prim: grow one tree from a start vertex

```
   in_tree = {0}; heap = edges leaving 0
   while tree has < n vertices:
       (w, u->v) = pop lightest
       if v already in tree: skip (stale)
       add v (and that edge) to the tree
       push all edges from v to vertices not yet in the tree
```

The cut used: tree vs non-tree. Trace (exact output of `c/13`):

| pop | edge | result |
|---|---|---|
| 5 | 0-3 | TAKE, tree {0,3} |
| 6 | 3-5 | TAKE, tree {0,3,5} |
| 7 | 0-1 | TAKE |
| 7 | 1-4 | TAKE |
| 5 | 4-2 | TAKE (pushed when 4 joined) |
| 8 | 1-2 | stale: 2 in tree |
| 8 | 5-4 | stale |
| 9 | 3-1 | stale |
| 9 | 4-6 | TAKE, all 7 vertices |

Same weight 39. O(E log V) with a binary heap; O(V^2) with a plain array
(better for dense graphs). Prim looks almost exactly like Dijkstra — the
only difference is the key: Prim uses **the edge weight w**, Dijkstra uses
**dist[u] + w**.

## Kruskal vs Prim

| | Kruskal | Prim |
|---|---|---|
| data structure | sorted edge list + DSU | adjacency list + heap |
| grows | a forest that merges | one tree |
| best for | sparse graphs, edge-list input | dense graphs (array version O(V^2)) |
| works on disconnected graphs | yes, gives a minimum spanning **forest** | only spans the start's component |
| time | O(E log E) | O(E log V) |

Uniqueness: if all weights are distinct, the MST is unique. With ties (G9
has two 5s, two 7s...) there can be several MSTs with the same weight; tests
should compare total weight, not edge sets. The programs cross-check
Kruskal == Prim == brute force (try every subset of n-1 edges) on 300
random graphs.

## Real-world uses

* Network design: cable, fibre, pipes, power lines between sites.
* Clustering (single linkage), image segmentation (Felzenszwalb's algorithm
  is Kruskal-like on pixel graphs).
* Approximation: the MST is a 2-approximation building block for metric TSP
  (walk the tree, shortcut repeats) and Christofides' 1.5-approximation.
* Ethernet **Spanning Tree Protocol** (STP) builds a loop-free spanning tree
  of the switch network (rooted at a bridge, by path cost — closer to a
  shortest-path tree, but the same "keep it a tree, cut the loops" goal).

## Pitfalls

* Running MST on a directed graph: undefined; the directed version
  ("minimum arborescence", Chu-Liu/Edmonds) is a different algorithm.
* Forgetting the graph may be disconnected (Prim silently returns a partial
  tree; check you got n-1 edges).
* Treating the MST as a shortest-path tree: the 0 -> 2 path in the MST
  above costs 7+7+5 = 19, but 0 -> 1 -> 2 costs 15.

[Previous: shortest paths](06-shortest-paths.md) · [Next: advanced](08-advanced.md)
