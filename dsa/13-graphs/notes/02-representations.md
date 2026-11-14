# 2. Representations: matrix, list, edge list, CSR

[Back to the chapter](../README.md) · Programs:
[c/01_representations.c](../c/01_representations.c),
[cpp/01_representations.cpp](../cpp/01_representations.cpp)

The same graph G1 four ways:

```
       0 ----- 1 ----- 2
       |       |       |
       3 ----- 4       5
               |       |
               6 ----- 7
```

## 1. Edge list — "just the pairs"

```
   0-1  0-3  1-2  1-4  2-5  3-4  4-6  5-7  6-7
```

An array of `{u, v}` (or `{u, v, w}`) structs. It is how input files and
netlists usually arrive, and it is exactly what **Kruskal** (sort the edges)
and **Bellman-Ford** (relax every edge) want. Bad at "who are v's
neighbours?" — that needs a scan of all m edges.

## 2. Adjacency matrix — "a table of yes/no"

```
        0 1 2 3 4 5 6 7
     0: 0 1 0 1 0 0 0 0
     1: 1 0 1 0 1 0 0 0        row u, column v = 1 if u-v is an edge
     2: 0 1 0 0 0 1 0 0        symmetric for undirected graphs
     3: 1 0 0 0 1 0 0 0        weights: store w instead of 1, INF for "none"
     4: 0 1 0 1 0 0 1 0
     5: 0 0 1 0 0 0 0 1
     6: 0 0 0 0 1 0 0 1
     7: 0 0 0 0 0 1 1 0
```

* `has_edge(u, v)` in O(1). Neighbours of u: scan a whole row, O(n).
* Memory O(n^2) **regardless of m**.
* Natural for dense graphs and for **Floyd-Warshall** (it *is* a matrix
  algorithm), for the capacity matrix of small flow problems, for bitset
  tricks (a row as a bitmask: `adj[u] & adj[v]` = common neighbours).
* In C, allocate one flat block `n*n` and index `u*n + v` — one malloc, one
  free, cache-friendly rows.

## 3. Adjacency list — "each vertex keeps its own neighbour list"

In C, the classic form is an **array of linked lists**:

```
   head[0] -> 1 -> 3 -> NULL
   head[1] -> 0 -> 2 -> 4 -> NULL
   head[2] -> 1 -> 5 -> NULL
   head[3] -> 0 -> 4 -> NULL
   head[4] -> 1 -> 3 -> 6 -> NULL        each arrow is a malloc'd node
   head[5] -> 2 -> 7 -> NULL             { int to; struct node *next; }
   head[6] -> 4 -> 7 -> NULL
   head[7] -> 5 -> 6 -> NULL
```

In C++ it is `std::vector<std::vector<int>>` (or `vector<vector<pair<int,int>>>`
for weights, or a vector of small `struct Arc { int to; int w; }`). Most C
programs in this chapter use the same idea with growable arrays per vertex
(`int *adj[u]`, `len[u]`, `cap[u]`), which keeps neighbours contiguous.

* Neighbours of u in O(deg(u)) — exactly what BFS/DFS/Dijkstra need.
* `has_edge(u, v)` costs O(deg(u)) (or O(1) with a hash set per vertex).
* Memory O(n + m). Undirected graphs store every edge twice.
* Insertion is cheap (append), deletion costs a search.

Linked list vs array per vertex: a linked node is ~16 bytes and each `->next`
is a likely cache miss; a contiguous array of 4-byte ints is 4x smaller and
the hardware prefetcher streams it. On the 1-million-vertex road graph that
is the difference between "fits in L2/L3" and "thrashes DRAM".

## 4. CSR — Compressed Sparse Row

Two flat arrays. All neighbour lists are concatenated into `col[]`;
`row_ptr[u]` says where u's slice starts.

```
   row_ptr = [0, 2, 5, 7, 9, 12, 14, 16, 18]          (n + 1 entries)
   col     = [1,3, 0,2,4, 1,5, 0,4, 1,3,6, 2,7, 4,7, 5,6]   (2m entries)
              \_/  \___/  \_/  \_/  \___/  \_/  \_/  \_/
               0     1     2    3     4     5    6    7

   neighbours of 4 = col[row_ptr[4] .. row_ptr[5]) = col[9..12) = {1, 3, 6}
   degree of u     = row_ptr[u+1] - row_ptr[u]
```

Building it (a counting sort by source vertex), O(n + m):

```
   pass 1: count degrees          row_ptr[u+1]++ for every arc u->v
   pass 2: prefix sum             row_ptr[u+1] += row_ptr[u]
   pass 3: scatter                col[cursor[u]++] = v   (cursor = copy of row_ptr)
```

* Smallest possible memory: `(n + 1 + m)` ints (+ m weights in a parallel
  array). No pointers at all — you can `mmap` it from a file or `memcpy` it
  to a GPU / DSP / C7x core and it still works (offsets, not addresses).
* Perfect for **static** graphs: road networks (route planners precompute
  CSR), social graphs in analytics, sparse matrices in numerical code (CSR is
  literally the sparse matrix format used by cuSPARSE, Eigen, SciPy), graph
  neural networks, PageRank, BFS on GPUs (Gunrock, cuGraph).
* Adding one edge means rebuilding (or batching updates). Dynamic graphs use
  lists; static ones get compiled to CSR.

## Trade-offs at a glance

| Representation | Memory | `has_edge(u,v)` | iterate neighbours of u | add edge | best for |
|---|---|---|---|---|---|
| Edge list | O(m) | O(m) | O(m) | O(1) | Kruskal, Bellman-Ford, input/output |
| Adjacency matrix | O(n^2) | **O(1)** | O(n) | O(1) | dense graphs, Floyd-Warshall, n <= ~10^4 |
| Adjacency list (linked) | O(n + m) | O(deg u) | O(deg u) | O(1) | teaching, frequent insert/delete |
| `vector<vector<int>>` | O(n + m) | O(deg u) | O(deg u) | amortised O(1) | **default choice** |
| CSR | O(n + m), smallest | O(deg u), or O(log deg) if sorted | O(deg u), contiguous | rebuild | big static graphs, GPU, HPC |

Real numbers printed by the C program, for a road-sized graph with
`n = 10^6`, `m = 5·10^6`: matrix ~125 GB even as a bitset; linked lists
~176 MB; CSR ~44 MB.

## Which one in practice?

```
   n small (<= a few thousand) and you need O(1) edge tests  -> matrix
   general purpose, graph changes                             -> vector<vector<>>
   input is "m lines of u v w", algorithm loops over edges    -> edge list
   graph is big and fixed, or goes to a GPU / accelerator     -> CSR
   graph is huge or infinite (grid, puzzle, game tree)        -> implicit: neighbours() function
```

## Pitfalls

* Forgetting the second direction for undirected edges (`adj[v].push_back(u)`).
* Using `int` for the matrix of a 50,000-vertex graph: 10 GB. Do the
  arithmetic `n*n*sizeof(cell)` before you allocate.
* `n*n` overflowing `int` in the index: cast to `size_t` first.
* Linked-list adjacency with head insertion reverses neighbour order: traces
  look different from the textbook. (Append at the tail or accept it.)
* CSR with 32-bit offsets overflows past 2^31 arcs; big graph libraries use
  64-bit `row_ptr` and 32-bit `col`.

[Previous: graph types](01-graph-types.md) · [Next: traversals](03-traversals.md)
