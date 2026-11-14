/*
 * 09_dijkstra.cpp — Dijkstra with std::priority_queue (min-heap via
 *                   std::greater), path reconstruction, early exit
 *
 * Concept : priority_queue has no decrease-key, so we push (newdist, v) again
 *           and skip entries whose distance is larger than the best known
 *           ("lazy deletion"). For a single target we may stop as soon as the
 *           target is POPPED — popped means settled means final.
 *           With a negative edge the "popped = final" rule is false; the
 *           program shows the wrong answer that results.
 * Build   : make run P=dsa/13-graphs/cpp/09_dijkstra.cpp
 * Read    : ../README.md and ../notes/06-shortest-paths.md  (section "Dijkstra")
 *
 *   G6 arcs: 0->1:4 0->2:1 2->1:2 1->3:1 2->3:5 2->4:8 3->4:3 3->5:6 4->5:2
 *   (picture in ../c/09_dijkstra.c and the notes)
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <queue>
#include <random>
#include <tuple>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Weight = long long;                           // sums of many ints: use 64-bit
constexpr Weight INF = std::numeric_limits<Weight>::max() / 4;
struct Arc { int to; Weight w; };
using Graph = std::vector<std::vector<Arc>>;

struct Result { std::vector<Weight> dist; std::vector<int> parent; int pops = 0; };

// O((V + E) log V). target = -1: all vertices; else stop when target settles.
Result dijkstra(const Graph &g, int src, int target = -1, bool trace = false)
{
    Result r{ std::vector<Weight>(g.size(), INF), std::vector<int>(g.size(), -1) };
    using Item = std::pair<Weight, int>;            // (dist, vertex): pair compares dist first
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    r.dist[src] = 0;
    pq.push({ 0, src });
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        ++r.pops;
        if (d > r.dist[u]) { if (trace) std::printf("  pop (%lld,%d) stale\n", d, u); continue; }
        if (trace) std::printf("  pop (%lld,%d) settle %d\n", d, u, u);
        if (u == target) break;                     // early exit: final for sure
        for (auto [v, w] : g[u])
            if (d + w < r.dist[v]) {
                r.dist[v] = d + w;
                r.parent[v] = u;
                pq.push({ r.dist[v], v });
                if (trace) std::printf("        relax %d->%d: dist[%d] = %lld\n", u, v, v, r.dist[v]);
            }
    }
    return r;
}

std::vector<int> path(const Result &r, int t)
{
    std::vector<int> p;
    if (r.dist[t] >= INF) return p;
    for (int v = t; v != -1; v = r.parent[v]) p.push_back(v);
    std::reverse(p.begin(), p.end());
    return p;
}

std::vector<Weight> bellman_ford(const Graph &g, int s)
{
    std::vector<Weight> d(g.size(), INF);
    d[s] = 0;
    for (size_t k = 1; k < g.size(); k++)
        for (size_t u = 0; u < g.size(); u++)
            if (d[u] < INF) for (auto [v, w] : g[u]) d[v] = std::min(d[v], d[u] + w);
    return d;
}

int main()
{
    Graph g(6);
    for (auto [u, v, w] : std::vector<std::tuple<int, int, int>>{
             {0,1,4},{0,2,1},{2,1,2},{1,3,1},{2,3,5},{2,4,8},{3,4,3},{3,5,6},{4,5,2} })
        g[u].push_back({ v, w });

    std::printf("Dijkstra on G6 from 0:\n");
    Result r = dijkstra(g, 0, -1, true);
    CHECK((r.dist == std::vector<Weight>{0, 3, 1, 4, 7, 9}));
    CHECK((path(r, 5) == std::vector<int>{0, 2, 1, 3, 4, 5}));
    std::printf("dist: 0 3 1 4 7 9; path 0->5: 0 2 1 3 4 5\n");

    Result early = dijkstra(g, 0, 3);
    std::printf("\nearly exit for target 3: %d pops instead of %d, dist[3] = %lld\n",
                early.pops, r.pops, early.dist[3]);
    CHECK(early.dist[3] == 4 && early.pops < r.pops);

    // Negative edge: 0->1:2 0->2:5 2->1:-4 1->3:1. Vertex 3 is popped
    // (settled) at distance 3 before the path 0->2->1 (= 1) is ever looked
    // at. Stopping at the target — exactly what "settled = final" allows —
    // returns a wrong answer. (Run to completion, this lazy version would
    // re-push 1 and repair it, but then pops are no longer final and the
    // running time can blow up exponentially. Use Bellman-Ford.)
    Graph ng(4);
    ng[0] = { {1, 2}, {2, 5} };
    ng[2] = { {1, -4} };
    ng[1] = { {3, 1} };
    Result bad = dijkstra(ng, 0, 3);
    auto good = bellman_ford(ng, 0);
    std::printf("\nnegative edge 2->1 = -4: Dijkstra (stop when 3 settles) says d[3] = %lld,"
                " Bellman-Ford says %lld\n", bad.dist[3], good[3]);
    CHECK(bad.dist[3] == 3 && good[3] == 2);

    std::mt19937 rng(8086);
    for (int t = 0; t < 400; t++) {
        int n = 1 + static_cast<int>(rng() % 14), m = static_cast<int>(rng() % (3 * n + 1));
        Graph rg(n);
        for (int i = 0; i < m; i++)
            rg[rng() % n].push_back({ static_cast<int>(rng() % n), static_cast<Weight>(rng() % 20) });
        int s = static_cast<int>(rng() % n);
        CHECK(dijkstra(rg, s).dist == bellman_ford(rg, s));
    }
    std::printf("random cross-check: 400 graphs, Dijkstra == Bellman-Ford\nOK\n");
    return 0;
}
