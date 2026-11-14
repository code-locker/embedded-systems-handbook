# 6. Shortest paths

[Back to the chapter](../README.md) · Programs:
[09_dijkstra](../c/09_dijkstra.c), [10_bellman_ford_floyd_warshall](../c/10_bellman_ford_floyd_warshall.c),
[11_zero_one_bfs_dag_paths](../c/11_zero_one_bfs_dag_paths.c), [12_astar_grid](../c/12_astar_grid.c)
(each also in [cpp/](../cpp/))

## The one idea behind all of them: relaxation

`dist[v]` is the best distance to v *found so far* (starts at infinity).
**Relaxing** edge `u -> v` with weight w means:

```
   if dist[u] + w < dist[v]:          "going through u is a shortcut to v"
       dist[v]   = dist[u] + w
       parent[v] = u
```

Every single-source algorithm below is "relax edges in a clever order until
nothing improves". They differ only in the order — and the order they can
use depends on the weights:

| weights | algorithm | order of relaxation | time |
|---|---|---|---|
| none (all 1) | BFS | by levels (FIFO queue) | O(V + E) |
| only 0 or 1 | 0-1 BFS | deque: 0-edges front, 1-edges back | O(V + E) |
| >= 0 | Dijkstra | smallest tentative distance first (heap) | O((V + E) log V) |
| >= 0, one target, good estimate | A* | smallest dist + estimate first | <= Dijkstra in practice |
| any, graph is a DAG | DAG-SP | topological order | O(V + E) |
| any (negatives OK) | Bellman-Ford | all edges, V-1 times | O(V E) |
| any, all pairs | Floyd-Warshall | DP over intermediate vertices | O(V^3) |

---

## Dijkstra

### Intuition: water filling pipes

Pour water in at the source; it spreads along pipes at constant speed. The
order in which junctions get wet is the order of their shortest distances.
Dijkstra simulates that: always **settle** the unsettled vertex with the
smallest tentative distance. With non-negative weights nothing found later
can be shorter (every extra edge only adds length), so a settled distance is
**final**.

### Mechanics with a binary heap (lazy deletion)

```
   dist[*] = INF; dist[s] = 0; heap = {(0, s)}
   while heap not empty:
       (d, u) = pop min
       if d > dist[u]: continue        # stale entry: u improved after this push
       for each u -> v (w):
           if d + w < dist[v]:
               dist[v] = d + w; parent[v] = u
               push (dist[v], v)        # no decrease-key: just push again
```

`std::priority_queue` and most heaps have no decrease-key, so we push
duplicates and skip the stale ones. At most E pushes, so O(E log E) =
O(E log V).

### Trace on G6 (exact output of `c/09_dijkstra.c`)

```
   G6: one-way streets, weights = minutes
              4            1
        0 --------> 1 --------> 3
        |           ^         ^ | \ 6
      1 |         2 |       5/  |3 \
        v           |       /   v   v
        2 ----------+------+    4 -> 5
        |                       ^  2
        +----------- 8 ---------+

   arcs: 0->1:4  0->2:1  2->1:2  1->3:1  2->3:5  2->4:8  3->4:3  3->5:6  4->5:2
```

| pop (d,v) | action | dist[0..5] | heap after (sorted) |
|---|---|---|---|
| (0,0) | d[1]=4, d[2]=1 | 0 4 1 ∞ ∞ ∞ | (1,2) (4,1) |
| (1,2) | d[1]=3, d[3]=6, d[4]=9 | 0 3 1 6 9 ∞ | (3,1) (4,1) (6,3) (9,4) |
| (3,1) | d[3]=4 | 0 3 1 4 9 ∞ | (4,1) (4,3) (6,3) (9,4) |
| (4,1) | **stale**, skip | | (4,3) (6,3) (9,4) |
| (4,3) | d[4]=7, d[5]=10 | 0 3 1 4 7 10 | (6,3) (7,4) (9,4) (10,5) |
| (6,3) | **stale**, skip | | (7,4) (9,4) (10,5) |
| (7,4) | d[5]=9 | 0 3 1 4 7 9 | (9,4) (9,5) (10,5) |
| (9,4) | **stale** | | (9,5) (10,5) |
| (9,5) | settle, nothing improves | | (10,5) |
| (10,5) | **stale** | | |

Shortest 0 -> 5 = 9 via 0 -> 2 -> 1 -> 3 -> 4 -> 5. Note the direct-looking
0 -> 1 (4) loses to 0 -> 2 -> 1 (3).

**Early exit:** if you only need one target t, stop when t is *popped*
(settled). `cpp/09` reaches target 3 in 5 pops instead of 10. Stopping when t
is merely *pushed* is a bug: a shorter path may still come.

### Why Dijkstra fails with negative edges

```
   0 --2--> 1 --1--> 3
   |        ^
   5       -4
   v        |
   2 -------+
```

Dijkstra settles 1 at distance 2 (it is the smallest), then settles 3 at 3.
Only later does it pop 2 (distance 5) and discover 0 -> 2 -> 1 = 1. But 1 was
already "final", and 3 was computed from the wrong value. The textbook
version (never touch settled vertices) answers d[1]=2, d[3]=3; the truth is
d[1]=1, d[3]=2 (Bellman-Ford). The proof's key step "any later path is at
least as long, because it only adds non-negative edges" is exactly what a
negative edge breaks.

(The lazy-heap version without early exit happens to repair this example by
re-pushing 1 — but then popped no longer means final and the worst case
becomes exponential. Don't rely on it; use Bellman-Ford, or Johnson's
reweighting trick for all-pairs.)

Adding a constant to every edge to make them non-negative does **not** work:
it penalises paths with more edges.

---

## Bellman-Ford

### Intuition

A shortest path (if no negative cycle) is simple, so it has at most V-1
edges. After pass k of "relax every edge", every shortest path that uses at
most k edges is correct. So V-1 passes are enough. If pass V **still**
improves something, there is a negative cycle reachable from the source:
you can go round it forever and the "shortest" distance is -infinity.

### Trace on G7 (CLRS fig. 24.4; s=0 t=1 y=2 x=3 z=4)

This graph has too many crossing arcs to draw cleanly in ASCII, so here is
the layout plus every arc, grouped by where it starts:

```
             t(1)              x(3)          from s(0): s->t 6,  s->y 7
                                             from t(1): t->x 5,  t->y 8,  t->z -4
     s(0)                                    from x(3): x->t -2
                                             from y(2): y->x -3, y->z 9
             y(2)              z(4)          from z(4): z->s 2,  z->x 7
```

Edges relaxed in exactly this order every pass:
`0->1 0->2 1->2 1->3 1->4 3->1 2->3 2->4 4->0 4->3` (output of `c/10`):

| after pass | d0 | d1 | d2 | d3 | d4 |
|---|---|---|---|---|---|
| 1 | 0 | 6 | 7 | 4 | 2 |
| 2 | 0 | **2** | 7 | 4 | 2 |
| 3 | 0 | 2 | 7 | 4 | **-2** |
| 4 | 0 | 2 | 7 | 4 | -2 (no change: stop early) |

Pass 1: 0->1 gives 6, 0->2 gives 7, 1->3 gives 11 then 2->3 (-3) gives 4,
1->4 gives 2. Pass 2: 3->1 (-2) gives d1 = 2. Pass 3: 1->4 (-4) gives -2.
Final path to 4: 0 -> 2 -> 3 -> 1 -> 4 = 7 - 3 - 2 - 4 = -2.

### Negative cycle

```
   0 --1--> 1 --(-1)--> 2 --(-1)--> 3 --2--> 4
            ^                       |
            +----------1------------+          cycle 1->2->3->1 = -1
```

```
   after pass 1:    0   0   0  -1   1
   after pass 2:    0  -1  -1  -2   0      keeps going down forever
   after pass 3:    0  -2  -2  -3  -1
   after pass 4:    0  -3  -3  -4  -2
   pass V still relaxes -> NEGATIVE CYCLE
```

To **find** the cycle: take a vertex that relaxed in pass V, walk `parent[]`
back V times (you are now guaranteed to be inside the cycle), then walk until
you return. `cpp/10` additionally marks every vertex reachable from the cycle
as -infinity (here 1, 2, 3 *and* 4).

Real use: **RIP** and other distance-vector routing protocols are a
distributed Bellman-Ford: each router repeatedly relaxes using its
neighbours' distance tables. Its famous "count to infinity" problem is the
negative-cycle-like slow convergence after a link dies. Also: currency
arbitrage = a negative cycle in `-log(exchange rate)` weights.

---

## Floyd-Warshall (all pairs)

### Intuition

`d_k[i][j]` = shortest i -> j path whose intermediate stops are only from
{0..k}. Allowing one more possible stop k:

```
   d[i][j] = min( d[i][j],  d[i][k] + d[k][j] )
                  not via k    via k (once)
```

Three nested loops, **k outermost** (with k inside, you combine
not-yet-finished values). O(V^3) time, O(V^2) space, negative edges fine;
`d[i][i] < 0` at the end means i is on a negative cycle.

### Trace on G8 (exact output of `c/10`)

```
   G8:  0->1:3  0->3:7  1->0:8  1->2:2  2->0:5  2->3:1  3->0:2

   initial          after k=0          after k=1         after k=2         after k=3
   0   3 inf  7     0   3 inf  7       0  3  5  7        0  3  5  6        0  3  5  6
   8   0   2 inf    8   0   2 15       8  0  2 15        7  0  2  3        5  0  2  3
   5 inf   0  1     5   8   0  1       5  8  0  1        5  8  0  1        3  6  0  1
   2 inf inf  0     2   5 inf  0       2  5  7  0        2  5  7  0        2  5  7  0
```

Read a change: after k=0, `d[1][3] = 15` = 1->0 (8) + 0->3 (7). After k=2,
`d[1][3] = 3` = 1->2 (2) + 2->3 (1). After k=3, `d[1][0] = 5` = 1->2->3->0.

**Path reconstruction / routing table** (`cpp/10`): keep `nxt[i][j]` = first
hop. When `via k` wins, `nxt[i][j] = nxt[i][k]`. Then "to reach j from i,
send to nxt[i][j]" — literally a router's forwarding table.

```
   next-hop table          route 1 -> 0: 1 -> 2 -> 3 -> 0 (cost 5; the direct
   0  1  1  1                            1->0 link costs 8)
   2  1  2  2
   3  3  2  3
   0  0  0  3
```

Use it when V is a few hundred to a couple of thousand and you need all
pairs (small maps, network latency matrices, transitive closure with booleans
— "Warshall"). For sparse big graphs, run Dijkstra from every vertex instead.

---

## 0-1 BFS

When every weight is 0 or 1, the "heap" only ever holds two distinct values
(d and d+1). A **deque** does the job: a 0-edge keeps you at the same
distance -> push **front**; a 1-edge -> push **back**. O(V + E).

Example (`c/11`): fewest one-way streets to reverse to drive 0 -> 5.
Driving with a street costs 0; driving against it means reversing it, cost 1.

```
        0 ---> 1 <--- 2 ---> 3 <--- 4 ---> 5
        |                                  |
        +-------------> 6 <----------------+
```

| pop | deque after |
|---|---|
| 0 (d=0) | 6(d=0) 1(d=0) |
| 6 (d=0) | 1(d=0) 5(d=1) |
| 1 (d=0) | 5(d=1) 2(d=1) |
| 5 (d=1) | 2(d=1) 4(d=2) |
| 2 (d=1) | 3(d=1) 4(d=2) |
| 3 (d=1) | 4(d=2) |
| 4 (d=2) | |

Answer: 1 reversal (drive 0 -> 6, reverse 5 -> 6). `cpp/11` does the grid
version ("repaint the fewest arrows to reach the corner").

Uses: grid games where some moves are free, minimum edge reversals,
"fewest walls to break", network paths counting only the paid links.

---

## A* on a grid

### Intuition

Dijkstra expands in a circle around the start — it has no idea where the
goal is. A* orders the heap by

```
   f(v) = g(v)  +  h(v)
          cost     optimistic guess of the cost still to go
          so far   (e.g. straight-line or Manhattan distance)
```

which pulls the search toward the goal. If h never **over**estimates
(admissible) — and is consistent, `h(u) <= w(u,v) + h(v)`, which Manhattan
distance is on a 4-neighbour unit grid — the first time the goal is popped
its distance is optimal. `h = 0` gives back Dijkstra.

### The U-trap example (exact output of `c/12_astar_grid.c`)

```
   o = expanded, * = path

   Dijkstra (h = 0): 49 cells          A* (h = Manhattan): 21 cells
     o o o o o o o o o o                 . . . . . . . . . .
     o * * * * * * * * o                 . * * * * * * * . .
     * * # # # # # o * *                 o * # # # # # * . .
     S o o o o o # o o G                 S * o o o o # * * G
     o o # # # # # o o o                 o o # # # # # . . .
     o o o o o o o o o o                 . . . . . . . . . .

   both find length 13 (Manhattan lower bound is 9: the wall costs 4 extra)
```

A* first runs straight into the pocket (h says "the goal is right there"),
then spills over the top. It still expands less than half of what Dijkstra
does.

### Inadmissible heuristics (`cpp/12`)

Multiply h by 5 ("weighted A*") and the search becomes greedy: it rushes
toward the goal and closes cells with non-optimal g values. On the map in
`cpp/12` it returns a path of cost **22 instead of 12**. Games and robots
sometimes accept that trade (faster, "good enough"); route planners that
promise the shortest route must not.

Real use: game path-finding, robot motion planning (ROS navigation
planners), GPS routing combined with precomputation (ALT landmarks,
contraction hierarchies), puzzle solving (15-puzzle with Manhattan-sum
heuristic).

---

## Shortest and longest paths in a DAG

No cycles means a topological order exists. Relax vertices **in that
order**: when you reach u, every path into u has already been relaxed, so
dist[u] is final. One pass, O(V + E), and **negative weights are fine**.

CLRS fig. 24.5 from s (exact output of `c/11`):

```
   r=0 s=1 t=2 x=3 y=4 z=5
        5      2      7     -1     -2
   r ----> s ----> t ----> x ----> y ----> z
   r->t:3  s->x:6  t->y:4  t->z:2  x->z:1

   topological order: 0 1 2 3 4 5
   relax out of 1: dist = inf 0 2 6 inf inf
   relax out of 2: dist = inf 0 2 6 6 4
   relax out of 3: dist = inf 0 2 6 5 4
   relax out of 4: dist = inf 0 2 6 5 3
   relax out of 5: dist = inf 0 2 6 5 3
```

### Critical path = longest path in a DAG

Longest path is NP-hard in general graphs but easy in a DAG: negate the
weights and run DAG-SP (or flip `min` to `max`). That is the **critical path
method** for schedules: the longest chain of dependent tasks is the minimum
total time, however many workers you have.

```
   building a board image (duration in minutes)
   fetch(2) -> config(1) -+-> uboot(5)  -+
                          +-> kernel(12) +-> package(3) -> done
                          +-> rootfs(8)  +

   earliest start: fetch=0 config=2 uboot=3 kernel=3 rootfs=3 package=15 done=18
   critical path : fetch -> config -> kernel -> package  = 18 minutes
```

Speeding up uboot or rootfs changes nothing; only the kernel build is on the
critical path. (This is why `make -j` wall time is bounded by the longest
dependency chain, not by the total work.)

---

## Pitfalls

* `INF + w` overflowing `int` (undefined behaviour in C). Use `INT_MAX / 4`
  or check `dist[u] != INF` before adding; use 64-bit sums for big graphs.
* Dijkstra with negative edges: silently wrong.
* Stopping Dijkstra when the target is pushed instead of popped.
* Floyd-Warshall with k as the inner loop.
* Using Bellman-Ford's answer without running the V-th detection pass.
* A* with an overestimating heuristic and expecting optimal paths; also
  Manhattan distance is NOT admissible on an 8-neighbour grid with diagonal
  cost 1 (use Chebyshev / octile distance).
* Forgetting that "shortest" with weights is not "fewest edges": BFS on a
  weighted graph is wrong.

[Previous: ordering](05-ordering.md) · [Next: MST](07-mst.md)
