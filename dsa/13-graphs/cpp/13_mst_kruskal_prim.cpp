/*
 * 13_mst_kruskal_prim.cpp — Kruskal (std::sort + DSU) and Prim
 *                           (std::priority_queue), plus MST clustering
 *
 * Concept : Cut property: the lightest edge crossing any cut is in some MST.
 *           Kruskal sorts edges and keeps those joining different DSU sets;
 *           Prim grows one tree, always taking the lightest edge out of it.
 *           Bonus: run Kruskal but stop at k components — that is
 *           single-linkage clustering (group sensors / pixels / hosts that are
 *           close together).
 * Build   : make run P=dsa/13-graphs/cpp/13_mst_kruskal_prim.cpp
 * Read    : ../README.md and ../notes/07-mst.md
 *
 *   G9 edges: 0-1:7 0-3:5 1-2:8 1-3:9 1-4:7 2-4:5 3-4:15 3-5:6 4-5:8 4-6:9 5-6:11
 *   (picture in ../c/13_mst_kruskal_prim.c and the notes)  MST weight 39
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <numeric>
#include <queue>
#include <random>
#include <tuple>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

struct Edge { int u, v, w; };

struct Dsu {
    std::vector<int> p, r;
    int sets;
    explicit Dsu(int n) : p(n), r(n, 0), sets(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }
    bool unite(int a, int b)
    {
        a = find(a); b = find(b);
        if (a == b) return false;
        if (r[a] < r[b]) std::swap(a, b);
        p[b] = a;
        r[a] += r[a] == r[b];
        --sets;
        return true;
    }
};

// Kruskal, stopping when `target_sets` components remain (1 = full MST).
std::vector<Edge> kruskal(int n, std::vector<Edge> e, int target_sets = 1)
{
    std::sort(e.begin(), e.end(), [](const Edge &a, const Edge &b) {
        return std::tie(a.w, a.u, a.v) < std::tie(b.w, b.u, b.v);
    });
    Dsu d(n);
    std::vector<Edge> tree;
    for (const auto &x : e) {
        if (d.sets == target_sets) break;
        if (d.unite(x.u, x.v)) tree.push_back(x);
    }
    return tree;
}

long long prim(int n, const std::vector<Edge> &e)
{
    std::vector<std::vector<std::pair<int, int>>> adj(n);    // (to, w)
    for (const auto &x : e) { adj[x.u].push_back({ x.v, x.w }); adj[x.v].push_back({ x.u, x.w }); }
    std::vector<char> in(n, 0);
    using Item = std::pair<int, int>;                         // (w, vertex)
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    pq.push({ 0, 0 });
    long long total = 0;
    int taken = 0;
    while (!pq.empty() && taken < n) {
        auto [w, u] = pq.top();
        pq.pop();
        if (in[u]) continue;                                  // stale
        in[u] = 1;
        total += w;
        ++taken;
        for (auto [v, wv] : adj[u]) if (!in[v]) pq.push({ wv, v });
    }
    return taken == n ? total : -1;                           // -1: graph not connected
}

long long weight(const std::vector<Edge> &t)
{
    return std::accumulate(t.begin(), t.end(), 0LL, [](long long s, const Edge &x) { return s + x.w; });
}

int main()
{
    const std::vector<Edge> g9 = { {0,1,7},{0,3,5},{1,2,8},{1,3,9},{1,4,7},{2,4,5},
                                   {3,4,15},{3,5,6},{4,5,8},{4,6,9},{5,6,11} };
    auto t = kruskal(7, g9);
    std::printf("Kruskal MST:");
    for (const auto &x : t) std::printf(" %d-%d(%d)", x.u, x.v, x.w);
    std::printf("  weight %lld\nPrim MST weight %lld\n", weight(t), prim(7, g9));
    CHECK(t.size() == 6 && weight(t) == 39 && prim(7, g9) == 39);

    // Clustering: 8 sensors on a line-ish layout; edge weight = distance.
    // Stopping Kruskal at 3 components = 3 clusters of nearby sensors.
    const std::vector<Edge> sensors = { {0,1,1},{1,2,2},{2,3,9},{3,4,1},{4,5,2},{5,6,10},{6,7,1},
                                        {0,2,3},{3,5,3},{2,4,11},{5,7,12} };
    auto forest = kruskal(8, sensors, 3);
    Dsu d(8);
    for (const auto &x : forest) d.unite(x.u, x.v);
    std::printf("\n3 clusters by stopping Kruskal early:");
    for (int c = 0; c < 8; c++) if (d.find(c) == c) {
        std::printf(" {");
        for (int v = 0; v < 8; v++) if (d.find(v) == c) std::printf(" %d", v);
        std::printf(" }");
    }
    std::printf("\n");
    CHECK(d.sets == 3 && d.find(0) == d.find(2) && d.find(3) == d.find(5) && d.find(6) == d.find(7));
    CHECK(d.find(2) != d.find(3) && d.find(5) != d.find(6));

    std::mt19937 rng(5150);
    for (int it = 0; it < 300; it++) {
        int n = 2 + static_cast<int>(rng() % 6);
        std::vector<Edge> e;
        for (int v = 1; v < n; v++) e.push_back({ static_cast<int>(rng() % v), v, 1 + static_cast<int>(rng() % 9) });
        int extra = static_cast<int>(rng() % 6);
        for (int i = 0; i < extra; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n);
            if (a != b) e.push_back({ a, b, 1 + static_cast<int>(rng() % 9) });
        }
        long long best = -1;                                  // brute force over edge subsets
        const int m = static_cast<int>(e.size());
        for (unsigned mask = 0; mask < (1u << m); mask++) {
            std::vector<Edge> pick;
            for (int i = 0; i < m; i++) if (mask >> i & 1) pick.push_back(e[i]);
            if (static_cast<int>(pick.size()) != n - 1) continue;
            Dsu dd(n);
            bool ok = std::all_of(pick.begin(), pick.end(), [&dd](const Edge &x) { return dd.unite(x.u, x.v); });
            if (ok && (best < 0 || weight(pick) < best)) best = weight(pick);
        }
        CHECK(weight(kruskal(n, e)) == best && prim(n, e) == best);
    }
    std::printf("random cross-check: 300 graphs, Kruskal == Prim == brute force\nOK\n");
    return 0;
}
