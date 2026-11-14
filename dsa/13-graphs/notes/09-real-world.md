# 9. Where graphs are used

[Back to the chapter](../README.md)

Graphs are the data structure of *relationships*. Once you look for them,
you see them everywhere in the stack you work on every day — from the
device tree that describes your BeagleBone AI-64 to the GStreamer pipeline
that moves its camera frames.

| Domain | Vertices | Edges | Algorithm(s) |
|---|---|---|---|
| Maps / GPS | junctions | road segments (weight = time) | Dijkstra, A*, contraction hierarchies |
| IP routing, OSPF / IS-IS | routers | links (cost) | **Dijkstra** (link-state, SPF) |
| IP routing, RIP | routers | links (hop count) | **Bellman-Ford** (distance-vector) |
| Ethernet switching | bridges | links | spanning tree (STP) |
| Social networks | people | follows / friendships | BFS (degrees of separation), components, PageRank-like ranking |
| Package managers, build systems | packages / targets | depends-on | topological sort, cycle detection |
| Compilers | basic blocks / variables | jumps / interference | CFG analysis, dominators, SCC, graph colouring |
| Deadlock detection | threads, locks | waits-for / held-by | cycle detection |
| Linux kernel | devices, locks, initcalls | parent/phandle links, lock order | tree walks, lockdep cycle detection, ordering |
| GStreamer | elements | pad links | DAG, topological order, negotiation along links |
| Recommendations | users, items | interactions | bipartite graphs, random walks, graph embeddings |
| Web search | pages | links | PageRank (random walk / eigenvector) |
| Chips & PCBs | cells, pins | nets | partitioning (min-cut), routing (A*/maze), MST/Steiner |
| Biology | k-mers, proteins | overlaps, interactions | Euler paths (assembly), clustering |

---

## Maps and GPS routing

A road network is a sparse, weighted, mostly-planar directed graph (one-way
streets): ~10^7-10^8 vertices for a continent. Plain Dijkstra explores a
huge disc around the start; production routers (OSRM, Valhalla, GraphHopper,
in-car navigation) preprocess the graph:

* **A\*** with landmark heuristics (ALT): precompute distances to a few
  landmarks; the triangle inequality gives an admissible h.
* **Contraction hierarchies**: add "shortcut" edges that skip unimportant
  vertices, then run a bidirectional Dijkstra that only climbs the hierarchy.
  Queries in microseconds on continent-size graphs.
* The graph is stored as **CSR** arrays — precisely because it is big and
  static.
* Live traffic = changing edge weights, so they re-customise weights without
  rebuilding the whole structure.

## Network routing: OSPF is Dijkstra, RIP is Bellman-Ford

* **OSPF / IS-IS (link-state):** every router floods the state of its links
  to all routers, so each one holds the *entire* topology graph and runs
  **Dijkstra (SPF)** from itself. The resulting shortest-path tree gives the
  next hop for each destination — exactly the next-hop table from
  Floyd-Warshall in `cpp/10`, but computed per source.
* **RIP (distance-vector):** a router only knows its neighbours' distance
  tables and does `my_dist[d] = min over neighbour n (cost(n) +
  n.dist[d])` — that is one **Bellman-Ford relaxation step**, run
  asynchronously across the network. It converges slowly, and after a link
  failure can "count to infinity" (RIP caps infinity at 16 hops for that
  reason).
* **BGP** (between ISPs) is a path-vector protocol where policy, not just
  shortest distance, picks routes.
* **STP** (Spanning Tree Protocol) on Ethernet switches computes a spanning
  tree so broadcast frames cannot loop forever.

## Social networks

People = vertices, friendships = undirected edges (followers = directed).
"Six degrees of separation" = BFS distances. "People you may know" = vertices
at distance 2 with many common neighbours. Communities = dense clusters
(modularity, label propagation). Influencers = high degree / PageRank /
betweenness centrality (how many shortest paths pass through you). Social
graphs have a few vertices with enormous degree, which matters for memory
layout and parallel BFS.

## Dependency resolution

Package managers (apt, pip, cargo, npm), build systems (make, ninja, Bazel,
BitBake in Yocto), init systems (systemd), CI pipelines (GitHub Actions
`needs:`), spreadsheets: all are DAGs. They need:

* **topological sort** for an order,
* **cycle detection** to reject or report circular dependencies,
* sometimes **SCC** to treat a cycle as one unit,
* **critical path** (longest path) to predict how long a parallel build takes.

Version constraints turn package resolution into a SAT problem (apt, pip's
resolver, cargo) — graph search with backtracking.

## Compilers

* **Control-flow graph (CFG):** vertices = basic blocks (straight-line code),
  edges = possible jumps. Loops = back edges / SCCs; dead code = vertices
  unreachable from the entry (DFS); dominator trees drive SSA construction.
* **Call graph:** functions and who calls whom. SCCs = groups of mutually
  recursive functions; used for inlining decisions and whole-program analysis.
* **Data-dependency DAG:** instruction scheduling inside a block is a
  topological order that also respects pipeline latencies (the critical path
  again).
* **Register allocation as graph colouring:** vertex = variable (live
  range), edge = "both alive at the same time" (they interfere). Colour the
  graph with k colours = k registers so no two interfering variables share
  one. Graph colouring is NP-hard, so compilers use heuristics (Chaitin-
  Briggs: repeatedly remove vertices with degree < k; if stuck, spill a
  variable to the stack). On register-starved targets (DSPs, small MCUs)
  this is very visible in the generated code.

## Deadlock detection: wait-for graphs

Draw `T1 -> T2` when thread T1 waits for a resource held by T2. A **cycle**
means deadlock: everyone in it waits forever. Databases (PostgreSQL, MySQL
InnoDB) build this graph and abort one transaction in the cycle. With
resource nodes too it is a resource-allocation graph; with single-instance
resources a cycle is deadlock.

## The Linux kernel

* **Device tree** (what your AI-64 boots with): nodes form a **tree** (the
  `/` root, buses, devices), but `phandle` references (clocks, interrupts,
  power domains, GPIOs, `remote-endpoint` in camera/display graphs) add
  cross links, turning it into a general graph. The kernel's **fw_devlink**
  turns those references into supplier -> consumer links so a device probes
  only after its suppliers — a dependency DAG, with cycle detection for
  broken DTs. The media/V4L2 subsystem's `of_graph` (ports / endpoints)
  describes the camera -> CSI-2 receiver -> ISP pipeline as an explicit graph.
* **lockdep:** records the order in which every lock class is ever taken
  ("A held while acquiring B" = edge A -> B). If a new acquisition would
  create a **cycle** in this lock-order graph, it prints a "possible
  circular locking dependency" splat — catching deadlocks that have not
  happened yet. That is directed cycle detection, run inside the kernel.
* **Kconfig** `depends on` / `select` form a dependency graph that
  `menuconfig` resolves (and warns about recursive dependencies — cycles).
* **Module dependencies**: `depmod` computes which modules a module needs;
  `modprobe` loads them in topological order.
* The scheduler's and memory-management's structures are trees (rbtree,
  radix/maple tree) — graphs with extra structure.

## GStreamer pipelines as a DAG of elements

A pipeline is a directed graph: elements are vertices, links from a **src
pad** to a **sink pad** are arcs. For data flow it must be acyclic (a DAG):

```
   v4l2src -> capsfilter -> tee -+-> queue -> v4l2h264enc -> h264parse -> mp4mux -> filesink
                                 +-> queue -> videoconvert -> autovideosink
```

* **tee** = fan-out vertex (one in, many out); **compositor / audiomixer /
  muxers** = fan-in vertices. `queue` after each `tee` branch adds a thread
  boundary so branches do not block each other.
* **State changes** run sink -> source, i.e. reverse topological order, so
  downstream is ready before upstream pushes.
* **Caps negotiation** and **allocation queries** travel along the edges:
  queries go upstream/downstream along the links, and the negotiated format
  must be consistent along every path.
* **Latency**: the pipeline latency is computed from the per-element
  latencies along the paths — a longest-path flavour computation on the DAG.
* `GST_DEBUG_DUMP_DOT_DIR` dumps the pipeline as a **Graphviz DOT graph**
  — GStreamer literally hands you the graph to look at.
* On the TDA4VM, TI's TIOVX (OpenVX) framework is also an explicit graph of
  nodes (capture -> VISS/ISP -> LDC -> MSC scaler -> DL inference on C7x),
  verified and scheduled as a DAG before it runs.

## Recommendation systems

Users and items form a **bipartite graph** (user -> item if watched/bought/
clicked). "People who bought this also bought" = 2-step walks
item -> user -> item. Modern systems run random walks (Pinterest's Pixie)
or graph neural networks over these graphs to learn embeddings; the graphs
are stored in CSR-like formats on GPUs.

## PageRank

Model a random surfer clicking links: with probability 0.85 follow a random
outgoing link, otherwise jump to a random page. A page's rank = the long-run
fraction of time the surfer spends there:

```
   PR(v) = (1 - 0.85) / N  +  0.85 * sum over u -> v of  PR(u) / outdeg(u)
```

Iterate until it stops changing (power iteration = repeated sparse
matrix-vector multiplication — the CSR format again). The random jump
handles dead ends and "spider traps" (SCCs with no way out). The same idea
ranks people in social networks, papers in citation graphs, and important
functions in a call graph.

---

[Previous: advanced](08-advanced.md) · [Back to the chapter](../README.md)
