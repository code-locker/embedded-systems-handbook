# 13 — Graphs

> Things connected to other things. Roads, routers, packages, threads and
> locks, device-tree nodes, GStreamer elements. Learn BFS and DFS deeply and
> every other algorithm in this chapter becomes "BFS or DFS plus one idea".

This topic is big, so this README is the **chapter overview** and each part
has a long-form note with full step-by-step traces:

| Part | Note | Programs (C and C++) |
|---|---|---|
| 1. What a graph is, every type | [notes/01-graph-types.md](notes/01-graph-types.md) | — |
| 2. Representations | [notes/02-representations.md](notes/02-representations.md) | 01 |
| 3. Traversals: BFS, DFS, edge classes | [notes/03-traversals.md](notes/03-traversals.md) | 02, 03 |
| 4. Connectivity: components, DSU, cycles, bipartite, bridges, SCC | [notes/04-connectivity.md](notes/04-connectivity.md) | 04, 05, 07, 08 |
| 5. Ordering: topological sort | [notes/05-ordering.md](notes/05-ordering.md) | 06 |
| 6. Shortest paths | [notes/06-shortest-paths.md](notes/06-shortest-paths.md) | 09, 10, 11, 12 |
| 7. Minimum spanning tree | [notes/07-mst.md](notes/07-mst.md) | 13 |
| 8. Max flow, matching, Euler, TSP | [notes/08-advanced.md](notes/08-advanced.md) | 14, 15, 16 |
| 9. Where graphs are used | [notes/09-real-world.md](notes/09-real-world.md) | — |

Suggested path: read this page top to bottom once, then one note per
session, running its programs while you read the trace tables.

---

## 1. Why this exists

Arrays, lists and trees store things that line up or branch out. Many real
problems are about **relationships with no fixed shape**: which router is
connected to which, which package needs which, which thread waits for which
lock, which element feeds which in a pipeline. A graph is the data structure
for "anything connected to anything".

Once a problem is a graph, decades of algorithms apply directly:

* "fastest route", "lowest-latency path" -> shortest paths
* "in what order do I build / install / start these?" -> topological sort
* "will these threads deadlock?", "circular dependency?" -> cycle detection
* "what single failure splits my network?" -> bridges / articulation points
* "cheapest way to connect everything" -> minimum spanning tree
* "how much can this network carry?", "who does which job?" -> max flow / matching

The hard part is usually not the algorithm; it is **recognising the graph**
in the problem. This chapter trains both.

## 2. Mental model

**Analogy: a city map.** Junctions are **vertices**, roads are **edges**.
Two-way roads are undirected edges, one-way streets are directed edges
(arcs), and the travel time printed on a road is its **weight**.

```
        0 ----- 1 ----- 2          G1: 8 vertices, 9 edges, undirected,
        |       |       |          unweighted. Used by programs 01-05.
        3 ----- 4       5
                |       |          deg(4) = 3, path 0-1-4-6, cycle 0-1-4-3-0
                6 ----- 7
```

Two traversals are the engines of almost everything:

```
   BFS: ripples in a pond (queue)          DFS: explorer with chalk (stack)

   level 0:  0                              0 -> 1 -> 2 -> 5 -> 7 -> 6 -> 4 -> 3
   level 1:  1 3                            then stuck: backtrack all the way up
   level 2:  2 4                            goes deep first, backtracks when stuck
   level 3:  5 6
   level 4:  7                              finds structure: cycles, order,
   = shortest paths in edges                bridges, SCCs
```

And one operation underlies all shortest-path algorithms:

```
   RELAX(u -> v, w):   if dist[u] + w < dist[v]:  dist[v] = dist[u] + w
                       "going through u is a shortcut to v"
```

## 3. How it works — the chapter in one page

### 3.1 Types (details: [notes/01](notes/01-graph-types.md))

Ask five questions about any graph; the answers pick the algorithm:

```
   directed?      one-way relationships (depends-on, links, pad -> pad)
   weighted?      none / 0-1 / non-negative / may be negative
   cyclic?        a DAG (no directed cycle) unlocks topological order and linear-time paths
   connected?     components (undirected) / SCCs (directed)
   sparse/dense?  real graphs are sparse: m ~ n, not n^2
```

Also: complete graphs (TSP inputs), bipartite graphs (2-colourable, no odd
cycle: assignment problems), trees and forests (connected+acyclic, n-1 edges),
multigraphs and self-loops (parallel links: identify edges by id!), and
implicit graphs (grids, puzzle state spaces: neighbours computed, never stored).

### 3.2 Representations (details: [notes/02](notes/02-representations.md))

```
   edge list   [0-1, 0-3, 1-2, ...]                 Kruskal, Bellman-Ford, file input
   matrix      mat[u][v] = 1                        O(1) edge test, O(n^2) memory, Floyd-Warshall
   adj list    head[1] -> 0 -> 2 -> 4 -> NULL      the default; O(n + m)
               vector<vector<int>> adj
   CSR         row_ptr = [0,2,5,7,9,12,14,16,18]    two flat arrays: smallest, cache/GPU
               col     = [1,3,0,2,4,1,5,0,4,...]    friendly, static graphs
```

### 3.3 Traversals (details: [notes/03](notes/03-traversals.md))

* **BFS** (FIFO queue, mark on enqueue) gives levels and shortest paths in
  edges; multi-source BFS gives distance to the nearest of many sources.
* **DFS** (stack/recursion) with colours WHITE/GRAY/BLACK and timestamps
  d/f. Edge u->v is **tree** (v white), **back** (v gray: a cycle!),
  **forward** or **cross** (v black). Reverse finish order = topological order.
* Deep graphs need iterative DFS with an explicit stack of
  (vertex, next-neighbour index).

### 3.4 Connectivity (details: [notes/04](notes/04-connectivity.md))

* Components: BFS/DFS from every unvisited vertex.
* **Union-find (DSU):** forest of parent pointers; union by rank + path
  compression = α(n) <= 4 amortised per operation. Edge with both ends in
  the same set = cycle.
* **Cycles:** undirected — revisit through a non-arrival edge, or DSU;
  directed — an edge into a GRAY vertex.
* **Bipartite:** BFS 2-colouring; same-colour edge = odd cycle.
* **Bridges / articulation points:** Tarjan low-link, `low[v] > disc[u]` /
  `low[v] >= disc[u]`.
* **SCC:** Kosaraju (finish order + transpose) or Tarjan (low-link + stack);
  condensation is a DAG.

### 3.5 Ordering (details: [notes/05](notes/05-ordering.md))

* **Kahn:** output in-degree-0 vertices, decrement successors; leftover = cycle.
* **DFS:** reverse postorder.
* make, apt, systemd, Yocto, CI pipelines, and GStreamer (data flows in
  topological order; state changes go sink -> source, the reverse).

### 3.6 Shortest paths (details: [notes/06](notes/06-shortest-paths.md))

* BFS (unweighted), 0-1 BFS (deque), Dijkstra (heap, weights >= 0, fails
  with negative edges), Bellman-Ford (V-1 rounds, V-th round = negative cycle
  detector), Floyd-Warshall (all pairs, k outermost), A* (Dijkstra + an
  admissible heuristic), DAG shortest/longest path (topological order, any
  weights; longest = critical path).

### 3.7 Minimum spanning tree (details: [notes/07](notes/07-mst.md))

* **Cut property:** the lightest edge across any cut is safe.
* **Kruskal:** sort edges, keep those joining different DSU sets.
* **Prim:** grow one tree with a heap of edges leaving it.

### 3.8 Advanced (details: [notes/08](notes/08-advanced.md))

* **Max flow (Edmonds-Karp):** BFS augmenting paths in the residual graph
  with "undo" edges; **max flow = min cut**.
* **Bipartite matching:** unit-capacity flow, or Kuhn's augmenting paths.
* **Euler path/circuit (Hierholzer):** 0 or 2 odd vertices; walk, splice
  sub-tours; O(E).
* **TSP:** NP-hard; Held-Karp bitmask DP O(2^n n^2) for n <= ~20.

### 3.9 Which graph algorithm should I use?

```
                              What do you need?
                                     |
   +-------------+--------------+----+---------+---------------+----------------+
   |             |              |              |               |                |
 visit /       shortest       an order      connect all     structure        assignment /
 reach         path           (dependencies) cheaply        (cycles, cuts,   capacity
   |             |              |              |             groups)            |
 BFS or DFS      |          DAG? -> topo sort  MST:            |            max flow
 (components:    |          (Kahn or DFS)      Kruskal         |            (Edmonds-Karp)
  BFS/DFS;       |          cycle? -> report   (edge list,     |            bipartite pairs:
  online: DSU)   |                             sparse) or      |            matching
                 |                             Prim (dense)    |            (Kuhn / flow)
                 |                                             |
   one source or all pairs?                     +--------------+---------------+
     |                 |                        |              |               |
   one source       all pairs              directed cycle?  single points   groups that
     |              V <= ~2000?            DFS colours      of failure?     reach each
     |              -> Floyd-Warshall      undirected:      bridges / APs   other? SCC
     |              else Dijkstra from     DSU or DFS       (Tarjan low)    (Kosaraju /
     |              each vertex                                              Tarjan)
     |              (Johnson if negatives)
     |
   is it a DAG? --yes--> DAG shortest/longest path (topological order, any weights)
     | no
   edge weights?
     +-- none (all equal) ----------------> BFS
     +-- only 0 and 1 --------------------> 0-1 BFS (deque)
     +-- non-negative, one target, and a good distance estimate --> A*
     +-- non-negative --------------------> Dijkstra (binary heap)
     +-- some negative -------------------> Bellman-Ford (detects negative cycles)

   visit every EDGE once?   -> Euler path (Hierholzer), check degrees first
   visit every VERTEX once, shortest tour? -> TSP: NP-hard; n <= 20 Held-Karp,
                                              else heuristics (2-opt, Christofides)
   2-colour / two groups with no conflict inside? -> bipartite check (BFS colouring)
```

## 4. Complexity table

n = |V|, m = |E|. All with adjacency lists unless noted.

| Algorithm | Time | Extra space | Why |
|---|---|---|---|
| BFS / DFS | O(n + m) | O(n) | each vertex enqueued/visited once, each list scanned once |
| BFS with adjacency matrix | O(n^2) | O(n) | finding neighbours costs a full row |
| Connected components | O(n + m) | O(n) | one traversal per component, total covers everything once |
| DSU, m operations | O(m α(n)) | O(n) | rank keeps trees log-height, compression flattens them |
| Undirected / directed cycle check | O(n + m) | O(n) | one DFS (or DSU) |
| Bipartite check | O(n + m) | O(n) | one BFS |
| Bridges & articulation points | O(n + m) | O(n) | one DFS with low-link |
| SCC (Kosaraju / Tarjan) | O(n + m) | O(n + m) / O(n) | two DFS + transpose / one DFS + stack |
| Topological sort (Kahn / DFS) | O(n + m) | O(n) | each vertex output once, each edge decrements once |
| Kahn with min-heap | O(n log n + m) | O(n) | heap ops on the ready set |
| Dijkstra (binary heap) | O((n + m) log n) | O(n + m) | <= m pushes, each O(log m) = O(log n) |
| Dijkstra (array, dense) | O(n^2) | O(n) | n times "scan for the minimum" |
| Bellman-Ford | O(n m) | O(n) | n-1 passes over all edges |
| Floyd-Warshall | O(n^3) | O(n^2) | n "via k" rounds over n^2 pairs |
| 0-1 BFS | O(n + m) | O(n + m) | deque replaces the heap |
| DAG shortest / longest path | O(n + m) | O(n) | one topological pass |
| A* | O(m log n) worst, far less typical | O(n) | Dijkstra with a better pop order |
| Kruskal | O(m log m) | O(n) | sort dominates; DSU near-constant |
| Prim (binary heap) | O(m log n) | O(n + m) | lazy heap of edges |
| Edmonds-Karp | O(n m^2) | O(n + m) | O(nm) augmentations x O(m) BFS |
| Kuhn bipartite matching | O(n m) | O(n) | one augmenting DFS per left vertex |
| Hierholzer (Euler) | O(n + m) | O(m) | each edge walked once |
| Held-Karp TSP | O(2^n n^2) | O(2^n n) | 2^n subsets x n endpoints x n transitions |

## 5. C vs C++

| Concept | C (this chapter) | C++ (this chapter) |
|---|---|---|
| adjacency list | array of linked lists (`01`), or `int **adj` + `len[]`/`cap[]` growable arrays, freed by hand | `std::vector<std::vector<int>>`, `vector<vector<Arc>>` |
| matrix | one flat `calloc(n*n)`, index `u*n+v` | `std::vector<unsigned char>(n*n)` in a class |
| CSR | two `malloc`'d int arrays built by counting sort | two `std::vector<int>` + `std::partial_sum` |
| queue (BFS) | int array with head/tail indices | `std::queue<int>` |
| deque (0-1 BFS) | ring buffer | `std::deque<int>` |
| heap (Dijkstra, Prim, A*) | hand-written binary heap (sift up/down) | `std::priority_queue<pair<…>, vector<…>, std::greater<>>` |
| DSU | struct of two arrays + functions | `class Dsu` with `find`/`unite`/`same` |
| recursion helper | static function with explicit parameters | lambda via `std::function`, or a class method |
| "no answer" | return -1 / a flag | `std::optional<…>` |
| random tests | small xorshift PRNG | `std::mt19937` |
| brute-force permutations | recursion with a `used` bitmask | `std::next_permutation` |

The C programs make every allocation and free visible (and run clean under
ASan/LSan); the C++ programs show how you would actually write it, plus a
few extras: multi-source BFS, Kahn with a min-heap and name-based
dependency resolution, DSU-based cycle detection, -infinity propagation in
Bellman-Ford, Floyd-Warshall next-hop tables, weighted (inadmissible) A*,
MST clustering, paired-edge residual graphs, directed Euler and de Bruijn
sequences.

**Libraries you would use in production:** Boost.Graph (C++), LEMON (C++,
flows and matchings), igraph (C, with Python/R bindings), NetworkX (Python,
prototyping), cuGraph/Gunrock (GPU), OR-Tools (routing/TSP/flows).

## 6. Where it is used in the real world

Full tour: [notes/09-real-world.md](notes/09-real-world.md). Highlights:

* **Maps / GPS:** road graphs in CSR, Dijkstra + A* + contraction hierarchies.
* **Network routing:** OSPF/IS-IS run **Dijkstra** on the link-state graph;
  RIP is a distributed **Bellman-Ford**; Ethernet STP builds a spanning tree.
* **Social networks:** BFS degrees of separation, communities, centrality.
* **Dependency resolution:** apt, pip, cargo, make, ninja, Bazel, Yocto
  BitBake, systemd — topological sort + cycle detection.
* **Compilers:** control-flow graphs (loops, dead code, dominators), call
  graph SCCs, instruction scheduling DAGs, **register allocation as graph
  colouring**.
* **Deadlock detection:** cycles in wait-for graphs (databases, OS).
* **Linux kernel:** the **device tree** (a tree plus phandle links;
  fw_devlink orders probing by supplier/consumer DAG; `of_graph`
  ports/endpoints describe camera pipelines), **lockdep** (cycle detection
  in the lock-order graph), Kconfig dependencies, module dependencies.
* **GStreamer:** a pipeline is a **DAG of elements**; data flows in
  topological order, state changes go sink -> source,
  `GST_DEBUG_DUMP_DOT_DIR` dumps the graph as Graphviz. TI's TIOVX/OpenVX on
  the TDA4VM is also an explicit node graph.
* **Recommendation systems:** bipartite user-item graphs, random walks.
* **PageRank:** a random walk on the web graph, computed by sparse
  matrix-vector products over CSR.

## 7. Common mistakes and pitfalls

1. **Undirected edge stored once.** Add both `u->v` and `v->u`.
2. **BFS marks on dequeue** -> vertices enqueued many times.
3. **Forgetting disconnected graphs:** wrap BFS/DFS in `for each unvisited s`.
4. **Recursion depth:** DFS on 10^6 vertices overflows the stack (and a
   kernel/RTOS stack is a few KB). Use an explicit stack.
5. **Two colours for directed cycle detection** -> false cycles on diamonds.
   You need GRAY (on the current path) vs BLACK (finished).
6. **Skipping the parent vertex instead of the arrival edge** in undirected
   DFS -> parallel edges mishandled (missed cycles, wrong bridges).
7. **Dijkstra with negative edges** -> silently wrong. Bellman-Ford instead.
8. **Stopping Dijkstra when the target is pushed** rather than popped.
9. **`INF + w` overflow** in C is undefined behaviour: use `INT_MAX / 4`,
   test `dist[u] != INF`, or 64-bit sums.
10. **Floyd-Warshall loop order:** k must be the outermost loop.
11. **Bellman-Ford without the V-th pass** -> negative cycles go unnoticed.
12. **A\* with an overestimating heuristic** -> non-optimal paths
    (`cpp/12` returns 22 instead of 12).
13. **Topological sort of a cyclic graph** without checking `count == n`.
14. **Assuming a unique answer** (topological orders, MST edge sets, shortest
    paths with ties): test validity/weight, not exact equality.
15. **Adjacency matrix for a big sparse graph:** compute `n*n*sizeof(cell)`
    first — 10^5 vertices x 4 bytes = 40 GB.
16. **MST on a directed graph** or treating the MST as shortest paths.
17. **Confusing Euler (every edge) with Hamiltonian (every vertex).** One is
    O(E), the other NP-hard.

## 8. Files in this lesson

Every program builds a graph that matches a diagram in the notes, prints its
step-by-step trace, asserts the known answer, and (where cheap) cross-checks
against a brute force on hundreds of random graphs.

### C (`c/`)

| File | What it shows | Run |
|---|---|---|
| [01_representations.c](c/01_representations.c) | G1 as matrix, linked-list adjacency, edge list, CSR; all cross-checked; memory comparison | `make run P=dsa/13-graphs/c/01_representations.c` |
| [02_bfs.c](c/02_bfs.c) | BFS trace table (queue, dist), levels, path rebuild; vs brute force | `make run P=dsa/13-graphs/c/02_bfs.c` |
| [03_dfs.c](c/03_dfs.c) | recursive + iterative DFS, d/f times, edge classification, parenthesis theorem | `make run P=dsa/13-graphs/c/03_dfs.c` |
| [04_union_find.c](c/04_union_find.c) | DSU with rank + path compression, parent[] trace, naive chain vs flat tree; vs BFS components | `make run P=dsa/13-graphs/c/04_union_find.c` |
| [05_cycles_and_bipartite.c](c/05_cycles_and_bipartite.c) | undirected cycle (edge ids), directed cycle (3 colours), bipartite colouring; vs brute force | `make run P=dsa/13-graphs/c/05_cycles_and_bipartite.c` |
| [06_topological_sort.c](c/06_topological_sort.c) | Kahn trace + DFS order on a build DAG, cycle detection, GStreamer pipeline order | `make run P=dsa/13-graphs/c/06_topological_sort.c` |
| [07_bridges_articulation.c](c/07_bridges_articulation.c) | Tarjan low-link trace, disc/low table, parallel-edge case; vs delete-and-count | `make run P=dsa/13-graphs/c/07_bridges_articulation.c` |
| [08_scc.c](c/08_scc.c) | Kosaraju and Tarjan traces, condensation DAG; vs mutual reachability | `make run P=dsa/13-graphs/c/08_scc.c` |
| [09_dijkstra.c](c/09_dijkstra.c) | hand-written heap, lazy deletion trace, negative-edge failure; vs Bellman-Ford | `make run P=dsa/13-graphs/c/09_dijkstra.c` |
| [10_bellman_ford_floyd_warshall.c](c/10_bellman_ford_floyd_warshall.c) | BF passes, negative cycle extraction, Floyd-Warshall matrices per k; cross-checked | `make run P=dsa/13-graphs/c/10_bellman_ford_floyd_warshall.c` |
| [11_zero_one_bfs_dag_paths.c](c/11_zero_one_bfs_dag_paths.c) | 0-1 BFS deque trace (street reversals), DAG shortest paths, critical path | `make run P=dsa/13-graphs/c/11_zero_one_bfs_dag_paths.c` |
| [12_astar_grid.c](c/12_astar_grid.c) | A* vs Dijkstra on a U-trap grid, expanded cells drawn; vs BFS on random mazes | `make run P=dsa/13-graphs/c/12_astar_grid.c` |
| [13_mst_kruskal_prim.c](c/13_mst_kruskal_prim.c) | Kruskal and Prim traces (weight 39); vs brute force over edge subsets | `make run P=dsa/13-graphs/c/13_mst_kruskal_prim.c` |
| [14_max_flow_matching.c](c/14_max_flow_matching.c) | Edmonds-Karp augmenting paths, min cut, Kuhn matching trace; vs brute force | `make run P=dsa/13-graphs/c/14_max_flow_matching.c` |
| [15_euler_hierholzer.c](c/15_euler_hierholzer.c) | Koenigsberg, house path with sub-tour splice, circuit; vs brute force | `make run P=dsa/13-graphs/c/15_euler_hierholzer.c` |
| [16_tsp_bitmask.c](c/16_tsp_bitmask.c) | Held-Karp DP table, greedy trap, growth table; vs brute force | `make run P=dsa/13-graphs/c/16_tsp_bitmask.c` |

### C++ (`cpp/`)

| File | What it shows | Run |
|---|---|---|
| [01_representations.cpp](cpp/01_representations.cpp) | `vector<vector<int>>`, matrix class, CSR with `partial_sum` | `make run P=dsa/13-graphs/cpp/01_representations.cpp` |
| [02_bfs.cpp](cpp/02_bfs.cpp) | `std::queue` BFS, path rebuild, multi-source BFS | `make run P=dsa/13-graphs/cpp/02_bfs.cpp` |
| [03_dfs.cpp](cpp/03_dfs.cpp) | lambda recursion vs explicit stack; 200,000-deep path without overflow | `make run P=dsa/13-graphs/cpp/03_dfs.cpp` |
| [04_union_find.cpp](cpp/04_union_find.cpp) | `class Dsu`, online connectivity, 10^6 operations | `make run P=dsa/13-graphs/cpp/04_union_find.cpp` |
| [05_cycles_and_bipartite.cpp](cpp/05_cycles_and_bipartite.cpp) | cycle via DSU, directed cycle returned as a vector, `std::optional` colouring | `make run P=dsa/13-graphs/cpp/05_cycles_and_bipartite.cpp` |
| [06_topological_sort.cpp](cpp/06_topological_sort.cpp) | Kahn templated on FIFO / min-heap, DFS order, install order by package name | `make run P=dsa/13-graphs/cpp/06_topological_sort.cpp` |
| [07_bridges_articulation.cpp](cpp/07_bridges_articulation.cpp) | `CutFinder` class (Tarjan low-link) | `make run P=dsa/13-graphs/cpp/07_bridges_articulation.cpp` |
| [08_scc.cpp](cpp/08_scc.cpp) | Kosaraju + Tarjan, condensation as `std::set` of edges | `make run P=dsa/13-graphs/cpp/08_scc.cpp` |
| [09_dijkstra.cpp](cpp/09_dijkstra.cpp) | `std::priority_queue` + `std::greater`, early exit, negative-edge failure | `make run P=dsa/13-graphs/cpp/09_dijkstra.cpp` |
| [10_bellman_ford_floyd_warshall.cpp](cpp/10_bellman_ford_floyd_warshall.cpp) | -infinity propagation, Floyd-Warshall next-hop routing table | `make run P=dsa/13-graphs/cpp/10_bellman_ford_floyd_warshall.cpp` |
| [11_zero_one_bfs_dag_paths.cpp](cpp/11_zero_one_bfs_dag_paths.cpp) | `std::deque` 0-1 BFS on an arrow grid, DAG shortest + longest | `make run P=dsa/13-graphs/cpp/11_zero_one_bfs_dag_paths.cpp` |
| [12_astar_grid.cpp](cpp/12_astar_grid.cpp) | A* templated on the heuristic: 0, Manhattan, 5x Manhattan (suboptimal!) | `make run P=dsa/13-graphs/cpp/12_astar_grid.cpp` |
| [13_mst_kruskal_prim.cpp](cpp/13_mst_kruskal_prim.cpp) | Kruskal/Prim with STL, MST clustering | `make run P=dsa/13-graphs/cpp/13_mst_kruskal_prim.cpp` |
| [14_max_flow_matching.cpp](cpp/14_max_flow_matching.cpp) | `MaxFlow` class with paired edges (`i ^ 1`), per-edge flows, matching by flow | `make run P=dsa/13-graphs/cpp/14_max_flow_matching.cpp` |
| [15_euler_hierholzer.cpp](cpp/15_euler_hierholzer.cpp) | directed Hierholzer O(E), de Bruijn sequences | `make run P=dsa/13-graphs/cpp/15_euler_hierholzer.cpp` |
| [16_tsp_bitmask.cpp](cpp/16_tsp_bitmask.cpp) | Held-Karp vs `std::next_permutation`, n = 16 timed | `make run P=dsa/13-graphs/cpp/16_tsp_bitmask.cpp` |

## 9. Exercises

Easy

1. Change G1 in `c/02_bfs.c` by adding edge 3-6. Predict the new dist[] and
   BFS tree **before** running it. *Hint: 6 is now reachable at level 2.*
2. Count the edges of a graph from its CSR arrays only. *Hint: `row_ptr[n]`.*
3. Write `is_tree(n, edges)` using DSU. *Hint: n-1 edges and no union fails.*
4. Grid flood fill: count islands of `#` in a character grid (4-neighbour).
   *Hint: components on an implicit graph.*

Medium

5. Modify `c/09_dijkstra.c` to also count the **number of shortest paths**
   to each vertex. *Hint: when `nd == dist[v]`, add `count[u]` to
   `count[v]`; when `nd < dist[v]`, reset it.*
6. Make Kahn's algorithm print the build in **parallel waves** (all vertices
   whose in-degree hits 0 in the same round). How many waves does the build
   DAG need? *Hint: process the queue level by level; answer 3.*
7. Print the actual negative cycle in `cpp/10` (not just -inf marks).
   *Hint: walk parent[] n times from a vertex that relaxed in pass n.*
8. Find all bridges in a 1000 x 1000 grid graph with a few random holes
   without recursion. *Hint: iterative DFS with (vertex, edge-index) frames,
   update `low` of the parent when a frame pops.*
9. Detect a deadlock: given "thread T holds lock L" and "thread T waits for
   lock L" lists, build the wait-for graph and print the cycle.

Hard

10. Implement **Dinic's** max-flow (BFS level graph + blocking flow DFS) on
    top of the paired-edge `MaxFlow` class and compare speed with
    Edmonds-Karp on a 2,000-vertex random network.
11. **2-SAT**: build the implication graph of `(a or b) and (not a or c) ...`
    and decide satisfiability with SCCs. *Hint: unsatisfiable iff x and
    not-x are in the same SCC.*
12. Write a tiny route planner: load a road graph from a CSV edge list into
    CSR, answer 1,000 random queries with Dijkstra and with bidirectional
    Dijkstra; compare settled-vertex counts. (Ties in with the "Route
    planner" project in the roadmap.)
13. Parse the output of `gst-launch-1.0 ... ` with `GST_DEBUG_DUMP_DOT_DIR`
    set: read the `.dot` file, build the element graph, and print a
    topological order and the reverse (state change) order.
14. Register allocation toy: given live ranges as intervals, build the
    interference graph and colour it greedily with k registers; spill when
    stuck.

## 10. Remember forever

```
BFS  = queue, mark when ENQUEUED, first discovery = fewest edges.      O(V+E)
DFS  = stack/recursion; WHITE/GRAY/BLACK; edge to GRAY = cycle;
       reverse finish order = topological order.                      O(V+E)
DSU  = parent forest; union by rank + path compression = ~O(1).
Kahn = output in-degree 0, decrement successors; < n output = cycle.
Relax: if d[u]+w < d[v] then d[v] = d[u]+w.
  BFS (no weights) | 0-1 BFS (deque) | Dijkstra (heap, w>=0)
  Bellman-Ford (V-1 rounds; round V improves = negative cycle)
  Floyd-Warshall (k outermost) | DAG: relax in topo order (any w)
MST: lightest edge across any cut is safe. Kruskal = sort + DSU; Prim = heap.
Low-link: low[v] > disc[u] bridge; low[v] >= disc[u] articulation point.
Max flow = min cut; residual "undo" edges. Euler: 0 or 2 odd degrees.
```

Review on the schedule in [LEARNING_METHOD.md](../../docs/LEARNING_METHOD.md).

## 11. Self-test questions

Answer out loud, without looking:

1. Why does BFS find shortest paths in an unweighted graph but DFS does not?
2. Why must BFS mark a vertex when it is enqueued, not when it is dequeued?
3. In a directed DFS, what is the difference between a back edge and a cross
   edge, and which one means a cycle? Why are two colours not enough?
4. What does CSR stand for, what are its two arrays, and why do GPUs and
   route planners like it?
5. Explain union by rank and path compression. What is α(n) for any n you
   will ever see?
6. Kahn's algorithm leaves 4 vertices unprocessed. What do you know about the
   graph?
7. Why does Dijkstra fail with a negative edge? Draw a 3-4 vertex example.
8. How does Bellman-Ford detect a negative cycle, and how do you find the
   cycle's vertices?
9. Why must k be the outer loop in Floyd-Warshall?
10. When is 0-1 BFS valid, and why does push_front for 0-edges keep it correct?
11. What makes an A* heuristic admissible? What happens if it is not?
12. Why can a DAG have shortest paths with negative edges in O(V + E)? How do
    you get the longest path?
13. State the cut property and use it to justify Kruskal.
14. Define `low[u]`. When is u-v a bridge? When is u an articulation point?
    What is special about the DFS root?
15. Why does Kosaraju process the transpose in decreasing finish time?
16. What is the residual graph, and why are reverse edges needed?
17. State max-flow min-cut and say how to read the min cut off the final
    residual graph.
18. When does an undirected graph have an Euler circuit? An Euler path?
19. Why is TSP hard while Euler path is easy? What does Held-Karp's
    `dp[mask][v]` mean?
20. In GStreamer, in which order do elements change state, and why is that
    the reverse of a topological order of the pipeline DAG?
21. OSPF and RIP: which shortest-path algorithm does each one implement,
    and what is "count to infinity"?
22. How does lockdep use a graph to report a deadlock that has not happened?
