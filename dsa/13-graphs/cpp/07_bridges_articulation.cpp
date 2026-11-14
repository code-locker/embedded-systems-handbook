/*
 * 07_bridges_articulation.cpp — bridges and articulation points (Tarjan
 *                               low-link), packaged as a class
 *
 * Concept : disc[u] = DFS entry time; low[u] = smallest disc reachable from
 *           u's subtree with at most one back edge. For tree edge u->v:
 *             low[v] >  disc[u]  => u-v is a BRIDGE (v's side hangs on it)
 *             low[v] >= disc[u]  => u is an ARTICULATION POINT (non-root)
 *           Root: articulation point iff >= 2 DFS children.
 *           Use: find single points of failure in a network / power grid /
 *           CAN-bus topology. Cross-checked by deleting and re-counting.
 * Build   : make run P=dsa/13-graphs/cpp/07_bridges_articulation.cpp
 * Read    : ../README.md and ../notes/04-connectivity.md  (section "Bridges")
 *
 *     0               5
 *     | \           / |
 *     |   1 - 3 - 4   |      bridges: 1-3, 3-4
 *     | /           \ |      articulation points: 1, 3, 4
 *     2               6
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

class CutFinder {
public:
    CutFinder(int n, const std::vector<std::pair<int, int>> &edges)
        : adj_(n), disc_(n, -1), low_(n, 0), is_ap_(n, false), is_bridge_(edges.size(), false)
    {
        for (size_t i = 0; i < edges.size(); i++) {
            adj_[edges[i].first].push_back({ edges[i].second, static_cast<int>(i) });
            adj_[edges[i].second].push_back({ edges[i].first, static_cast<int>(i) });
        }
        for (int v = 0; v < n; v++) if (disc_[v] == -1) dfs(v, -1);
    }
    const std::vector<bool> &articulation() const { return is_ap_; }
    const std::vector<bool> &bridges() const { return is_bridge_; }
    const std::vector<int> &disc() const { return disc_; }
    const std::vector<int> &low() const { return low_; }

private:
    struct Arc { int to, id; };

    void dfs(int u, int in_edge)                 // O(V + E)
    {
        disc_[u] = low_[u] = timer_++;
        int children = 0;
        for (auto [v, id] : adj_[u]) {
            if (id == in_edge) continue;         // skip the exact edge we came by
            if (disc_[v] == -1) {
                ++children;
                dfs(v, id);
                low_[u] = std::min(low_[u], low_[v]);
                if (in_edge != -1 && low_[v] >= disc_[u]) is_ap_[u] = true;
                if (low_[v] > disc_[u]) is_bridge_[id] = true;
            } else {
                low_[u] = std::min(low_[u], disc_[v]);
            }
        }
        if (in_edge == -1 && children >= 2) is_ap_[u] = true;
    }

    std::vector<std::vector<Arc>> adj_;
    std::vector<int> disc_, low_;
    std::vector<bool> is_ap_, is_bridge_;
    int timer_ = 0;
};

int count_components(int n, const std::vector<std::pair<int, int>> &e, int skip_v, int skip_e)
{
    std::vector<int> p(n);
    for (int i = 0; i < n; i++) p[i] = i;
    auto find = [&p](int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; };
    int c = n - (skip_v >= 0);
    for (size_t i = 0; i < e.size(); i++) {
        if (static_cast<int>(i) == skip_e || e[i].first == skip_v || e[i].second == skip_v) continue;
        int a = find(e[i].first), b = find(e[i].second);
        if (a != b) { p[a] = b; --c; }
    }
    return c;
}

int main()
{
    const std::vector<std::pair<int, int>> g4 = { {0,1},{0,2},{1,2},{1,3},{3,4},{4,5},{4,6},{5,6} };
    CutFinder cf(7, g4);
    std::printf("disc:");
    for (int x : cf.disc()) std::printf(" %d", x);
    std::printf("\nlow :");
    for (int x : cf.low()) std::printf(" %d", x);
    std::printf("\narticulation points:");
    for (int v = 0; v < 7; v++) if (cf.articulation()[v]) std::printf(" %d", v);
    std::printf("\nbridges:");
    for (size_t i = 0; i < g4.size(); i++) if (cf.bridges()[i]) std::printf(" %d-%d", g4[i].first, g4[i].second);
    std::printf("\n");
    CHECK((cf.low() == std::vector<int>{0, 0, 0, 3, 4, 4, 4}));
    CHECK((cf.articulation() == std::vector<bool>{false, true, false, true, true, false, false}));
    CHECK((cf.bridges() == std::vector<bool>{false, false, false, true, true, false, false, false}));

    std::mt19937 rng(31337);
    for (int t = 0; t < 500; t++) {
        int n = 1 + static_cast<int>(rng() % 12), m = static_cast<int>(rng() % (2 * n));
        std::vector<std::pair<int, int>> e;
        std::vector<int> deg(n, 0);
        for (int i = 0; i < m; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n);
            if (a != b) { e.push_back({ a, b }); ++deg[a]; ++deg[b]; }
        }
        CutFinder c(n, e);
        int base = count_components(n, e, -1, -1);
        for (int v = 0; v < n; v++)
            CHECK(c.articulation()[v] == (count_components(n, e, v, -1) > base - (deg[v] == 0)));
        for (size_t i = 0; i < e.size(); i++)
            CHECK(c.bridges()[i] == (count_components(n, e, -1, static_cast<int>(i)) > base));
    }
    std::printf("random cross-check: 500 multigraphs, Tarjan == delete-and-count\nOK\n");
    return 0;
}
