/*
 * 02_bfs.cpp — BFS with std::queue: distances, levels, path reconstruction,
 *              and multi-source BFS
 *
 * Concept : FIFO order = vertices leave the queue in non-decreasing distance,
 *           so the first discovery of a vertex is via a shortest path (in
 *           edges). Multi-source BFS starts with SEVERAL vertices at
 *           distance 0 — "distance to the nearest exit / server / fire" —
 *           in one O(V + E) pass instead of one BFS per source.
 * Build   : make run P=dsa/13-graphs/cpp/02_bfs.cpp
 * Read    : ../README.md and ../notes/03-traversals.md  (section "BFS")
 *
 *   G1:   0 ----- 1 ----- 2
 *         |       |       |
 *         3 ----- 4       5
 *                 |       |
 *                 6 ----- 7
 */
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <random>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Graph = std::vector<std::vector<int>>;

struct BfsResult {
    std::vector<int> dist, parent;   // dist = -1: unreachable
};

// O(V + E). `sources` may hold one or many start vertices.
BfsResult bfs(const Graph &g, const std::vector<int> &sources, bool trace = false)
{
    BfsResult r{ std::vector<int>(g.size(), -1), std::vector<int>(g.size(), -1) };
    std::queue<int> q;
    for (int s : sources) { r.dist[s] = 0; q.push(s); }
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        for (int v : g[u]) {
            if (r.dist[v] != -1) continue;       // first discovery is final
            r.dist[v] = r.dist[u] + 1;
            r.parent[v] = u;
            q.push(v);
        }
        if (trace) {
            std::printf("  pop %d | queue:", u);
            std::queue<int> copy = q;            // std::queue has no iteration: copy & drain
            int w = 0;
            while (!copy.empty()) { w += std::printf(" %d", copy.front()); copy.pop(); }
            std::printf("%*s | dist:", 10 - w, "");
            for (int d : r.dist) { if (d < 0) std::printf(" ."); else std::printf(" %d", d); }
            std::printf("\n");
        }
    }
    return r;
}

std::vector<int> path_to(const BfsResult &r, int dst)
{
    std::vector<int> p;
    if (r.dist[dst] < 0) return p;               // unreachable: empty path
    for (int v = dst; v != -1; v = r.parent[v]) p.push_back(v);
    std::reverse(p.begin(), p.end());
    return p;
}

// Reference: unit-weight Bellman-Ford. O(V * E), obviously correct.
std::vector<int> brute(const Graph &g, int s)
{
    std::vector<int> d(g.size(), INT_MAX);
    d[s] = 0;
    for (size_t round = 0; round < g.size(); round++)
        for (size_t u = 0; u < g.size(); u++)
            if (d[u] != INT_MAX)
                for (int v : g[u]) d[v] = std::min(d[v], d[u] + 1);
    for (int &x : d) if (x == INT_MAX) x = -1;
    return d;
}

int main()
{
    Graph g(8);
    for (auto [u, v] : std::vector<std::pair<int, int>>{ {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7} }) {
        g[u].push_back(v);
        g[v].push_back(u);
    }

    std::printf("BFS on G1 from 0:\n");
    BfsResult r = bfs(g, {0}, true);
    CHECK((r.dist == std::vector<int>{0, 1, 2, 1, 2, 3, 3, 4}));
    auto p = path_to(r, 7);
    std::printf("path 0 -> 7:");
    for (int v : p) std::printf(" %d", v);
    std::printf("  (%zu edges)\n", p.size() - 1);
    CHECK((p == std::vector<int>{0, 1, 2, 5, 7}));

    // Multi-source: two "exits" at 0 and 7. Every vertex learns its distance
    // to the NEAREST exit in a single pass.
    std::printf("\nmulti-source BFS from {0, 7}:\n");
    BfsResult m = bfs(g, {0, 7}, true);
    CHECK((m.dist == std::vector<int>{0, 1, 2, 1, 2, 1, 1, 0}));
    for (int v = 0; v < 8; v++) CHECK(m.dist[v] == std::min(r.dist[v], bfs(g, {7}).dist[v]));
    std::printf("each dist == min(dist from 0, dist from 7)\n");

    std::mt19937 rng(12345);
    for (int t = 0; t < 200; t++) {
        int n = 2 + static_cast<int>(rng() % 12);
        Graph rg(n);
        int edges = static_cast<int>(rng() % (2 * n));
        for (int i = 0; i < edges; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n);
            rg[a].push_back(b);
            rg[b].push_back(a);
        }
        int s = static_cast<int>(rng() % n);
        CHECK(bfs(rg, {s}).dist == brute(rg, s));
    }
    std::printf("\nrandom cross-check: 200 graphs, BFS == brute force\nOK\n");
    return 0;
}
