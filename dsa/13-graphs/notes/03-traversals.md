# 3. Traversals: BFS and DFS

[Back to the chapter](../README.md) · Programs:
[c/02_bfs.c](../c/02_bfs.c), [c/03_dfs.c](../c/03_dfs.c),
[cpp/02_bfs.cpp](../cpp/02_bfs.cpp), [cpp/03_dfs.cpp](../cpp/03_dfs.cpp)

Every graph algorithm in this chapter is BFS or DFS with extra bookkeeping.
Learn these two until you can write them half asleep.

```
   BFS = ripples in a pond             DFS = a maze explorer with chalk
   (queue: oldest first)               (stack: newest first)

        3 3 3                          go down one corridor until a dead end,
      3 2 2 2 3                        then walk back to the last junction
    3 2 1 1 1 2 3                      that still has an unexplored corridor
      3 2 S 2 3
        3 3 3
```

---

## BFS — breadth-first search

### Intuition

Drop a stone at the source. The first ring is everything 1 edge away, then
everything 2 edges away, and so on. A **FIFO queue** produces exactly this
order: vertices at distance d are all dequeued before any vertex at distance
d+1, because they were all enqueued earlier.

**Consequence (the reason BFS matters):** the first time BFS discovers a
vertex, it has found a **shortest path in number of edges**. In an
unweighted graph, BFS *is* the shortest-path algorithm.

### Mechanics

```
   dist[*] = -1 (unseen); dist[s] = 0; queue = [s]
   while queue not empty:
       u = pop front
       for each neighbour v of u:
           if dist[v] == -1:            # first discovery = final distance
               dist[v] = dist[u] + 1
               parent[v] = u             # remember how we got here
               push v at the back
```

Mark a vertex **when you enqueue it**, not when you dequeue it. Otherwise the
same vertex can be enqueued once per neighbour (still correct, but the queue
can grow to O(m) entries).

### Trace on G1 from 0 (exact output of `c/02_bfs.c`)

```
       0 ----- 1 ----- 2
       |       |       |
       3 ----- 4       5
               |       |
               6 ----- 7
```

| step | pop | newly discovered | queue after | dist[0..7] |
|---|---|---|---|---|
| 1 | 0 | 1 (d=1), 3 (d=1) | 1 3 | 0 1 . 1 . . . . |
| 2 | 1 | 2 (d=2), 4 (d=2) | 3 2 4 | 0 1 2 1 2 . . . |
| 3 | 3 | — (0, 4 already seen) | 2 4 | 0 1 2 1 2 . . . |
| 4 | 2 | 5 (d=3) | 4 5 | 0 1 2 1 2 3 . . |
| 5 | 4 | 6 (d=3) | 5 6 | 0 1 2 1 2 3 3 . |
| 6 | 5 | 7 (d=4) | 6 7 | 0 1 2 1 2 3 3 4 |
| 7 | 6 | — | 7 | 0 1 2 1 2 3 3 4 |
| 8 | 7 | — | (empty) | 0 1 2 1 2 3 3 4 |

```
   levels: L0={0}  L1={1,3}  L2={2,4}  L3={5,6}  L4={7}
   parent: [-1, 0, 1, 0, 1, 2, 4, 5]
   path to 7: follow parents 7 -> 5 -> 2 -> 1 -> 0, reverse: 0 1 2 5 7 (4 edges)

   BFS tree (the tree edges):        0
                                    / \
                                   1   3
                                  / \
                                 2   4
                                 |   |
                                 5   6
                                 |
                                 7
```

### Variants worth knowing

* **Multi-source BFS** (`cpp/02_bfs.cpp`): put *all* sources in the queue at
  distance 0. One pass gives every vertex its distance to the *nearest*
  source: nearest exit, nearest server, distance transform of an image,
  "how long until the fire spreads here".
* **BFS on a grid**: flood fill, maze solving, connected blobs in a binary
  image (a camera-pipeline "connected components labelling" step).
* **Bipartite check**: colour by level parity (see connectivity notes).
* **0-1 BFS**: deque instead of queue when edges weigh 0 or 1 (shortest
  paths notes).

### Complexity

O(n + m) time: every vertex enters the queue once, every adjacency list is
scanned once. O(n) extra space (queue + dist + parent). With an adjacency
matrix it becomes O(n^2), because finding neighbours costs a full row.

---

## DFS — depth-first search

### Intuition

Walk forward as long as you see an unvisited neighbour. When stuck, step
back to the most recent vertex that still has unexplored neighbours. The
"most recent" makes it a **stack** — usually the call stack (recursion).

DFS does **not** find shortest paths. What it finds is *structure*: cycles,
components, bridges, SCCs, topological order. It does this through two
timestamps and three colours.

### Colours and timestamps

```
   WHITE  never seen
   GRAY   discovered, still open: it is on the current recursion path
   BLACK  finished: it and everything reachable from it are done

   d[v] = clock value when v turns GRAY  (discovery)
   f[v] = clock value when v turns BLACK (finish)
```

### Trace on G2 (directed), exact output of `c/03_dfs.c`

```
            3 -> 0 (closes the cycle 0->1->3->0)
         +-----------------------------+
         v                             |
         0 ---------> 1 -------------> 3
         | \          ^                ^
         |  +---------|----------------+   0 -> 3 (shortcut)
         v            |
         2 -----------+   2 -> 1
         ^
         |
         5 <--------- 4

   arcs in adjacency order: 0:[1,2,3] 1:[3] 2:[1] 3:[0] 4:[5] 5:[2]
```

| clock | event | stack (gray path) | edge examined -> kind |
|---|---|---|---|
| 1 | discover 0 | 0 | |
| 2 | discover 1 | 0 1 | 0->1 tree |
| 3 | discover 3 | 0 1 3 | 1->3 tree |
| | | | 3->0: 0 is GRAY -> **back** (cycle!) |
| 4 | finish 3 | 0 1 | |
| 5 | finish 1 | 0 | |
| 6 | discover 2 | 0 2 | 0->2 tree |
| | | | 2->1: 1 BLACK, d[2]=6 > d[1]=2 -> **cross** |
| 7 | finish 2 | 0 | |
| | | | 0->3: 3 BLACK, d[0]=1 < d[3]=3 -> **forward** |
| 8 | finish 0 | | |
| 9 | discover 4 | 4 | (new DFS tree) |
| 10 | discover 5 | 4 5 | 4->5 tree |
| | | | 5->2: 2 BLACK, d[5]=10 > d[2]=6 -> **cross** |
| 11 | finish 5 | 4 | |
| 12 | finish 4 | | |

```
   vertex : 0  1  2  3  4  5
   d      : 1  2  6  3  9  10
   f      : 8  5  7  4  12 11

   parenthesis structure: (0 (1 (3 3) 1) (2 2) 0) (4 (5 5) 4)
```

**Parenthesis theorem:** for any two vertices the intervals `[d, f]` are
either nested (one is a descendant of the other) or disjoint — never
partially overlapping. The program checks this for all pairs.

### Edge classification (directed graphs)

When DFS examines `u -> v`, the colour of v tells you the edge type:

| colour of v | edge type | meaning | picture |
|---|---|---|---|
| WHITE | **tree** | DFS goes to v through this edge | part of the DFS forest |
| GRAY | **back** | v is an ancestor on the current path | u->v closes a **cycle** |
| BLACK, d[u] < d[v] | **forward** | v is an already-finished descendant | a shortcut down the tree |
| BLACK, d[u] > d[v] | **cross** | v is in a finished, unrelated branch | sideways / to an older tree |

```
             0               tree:    0->1, 1->3, 0->2, 4->5
           /   \             back:    3->0     <- the cycle detector
          1     2            forward: 0->3
          |                  cross:   2->1, 5->2
          3
```

* **A directed graph has a cycle iff DFS finds a back edge.** That is the
  three-colour cycle detector.
* In an **undirected** DFS only tree and back edges exist (every edge is seen
  from both ends; the "other" kinds cannot happen).
* Sorting vertices by **decreasing finish time** gives a topological order
  of a DAG (because for every edge u->v that is not a back edge,
  f[u] > f[v]).

### Recursive vs iterative

Recursion is the clearest DFS, but the recursion depth can reach n. A path
graph with 10^6 vertices overflows an 8 MB user stack — and a kernel stack is
only 16 KB (8 KB on some 32-bit configs), an RTOS task stack often 1-4 KB.

The iterative version must store what a recursion frame stores: the vertex
**and how far through its neighbour list it got**:

```
   recursion frames              explicit stack of (u, next-index)
   +--------------+
   | u=3, i=0     |  top         [(0,1), (1,1), (3,0)]
   | u=1, i=1     |
   | u=0, i=1     |              peek top (u,i): if i < deg(u): look at adj[u][i], i++
   +--------------+                              else: pop, finish u
```

The popular shortcut "pop u; push all unvisited neighbours" visits vertices
in a valid DFS-like order but gets the finish times wrong (and pushes a
vertex several times). It is fine for reachability, wrong for anything that
needs d/f (topological order, low-link). Both programs verify that the
(u, index) version reproduces the recursive d, f, parent and edge kinds
exactly; the C++ one then runs it on a 200,000-vertex path.

### Complexity

O(n + m) time, O(n) space (colour/time arrays + stack of depth <= n).

---

## BFS vs DFS cheat sheet

| question | use |
|---|---|
| fewest edges from s to t | **BFS** |
| "everything within k hops" / levels | **BFS** |
| nearest of several sources | multi-source **BFS** |
| is there a cycle? (directed) | **DFS** colours |
| topological order | **DFS** finish order (or Kahn = BFS on in-degrees) |
| bridges, articulation points, SCC | **DFS** + low-link |
| just "is t reachable?" / components | either (BFS avoids recursion depth issues) |
| explore a huge state space looking for any solution | DFS (memory = depth) or iterative deepening |

## Pitfalls

* Marking visited on dequeue instead of enqueue (BFS duplicates).
* Forgetting to restart from every unvisited vertex: you only explore the
  source's component. Disconnected graphs need the outer `for s in V` loop.
* Recursing on a million-vertex graph. Use the explicit stack.
* Using DFS when the question says "shortest" (unweighted): DFS paths can be
  arbitrarily long.
* In the iterative DFS, holding a reference to the top stack element across
  a `push` on a `std::vector` — the push may reallocate and the reference
  dangles. (The C++ programs re-read the top each loop iteration.)

[Previous: representations](02-representations.md) · [Next: connectivity](04-connectivity.md)
