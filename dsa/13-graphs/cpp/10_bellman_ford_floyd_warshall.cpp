/*
 * 10_bellman_ford_floyd_warshall.cpp — Bellman-Ford that marks every vertex
 *     whose distance is -infinity, and Floyd-Warshall with path
 *     reconstruction (next-hop matrix, like a router's forwarding table)
 *
 * Concept : Bellman-Ford: V-1 rounds of relaxing all edges. If round V still
 *           improves v, v is on or behind a negative cycle — and so is
 *           everything reachable from v: those distances are -infinity.
 *           Floyd-Warshall: d[i][j] = min(d[i][j], d[i][k] + d[k][j]) for
 *           k = 0..V-1 (k outermost!). nxt[i][j] = first hop on the best
 *           i->j path; following nxt hop by hop rebuilds the path — exactly
 *           a routing table ("to reach j, send to nxt[i][j]").
 * Build   : make run P=dsa/13-graphs/cpp/10_bellman_ford_floyd_warshall.cpp
 * Read    : ../README.md and ../notes/06-shortest-paths.md
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <queue>
#include <random>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using W = long long;
constexpr W INF = std::numeric_limits<W>::max() / 4;
constexpr W NEG_INF = std::numeric_limits<W>::min() / 4;
struct Edge { int u, v; W w; };

// O(V * E). Returns dist with NEG_INF for "arbitrarily small", INF for unreachable.
std::vector<W> bellman_ford(int n, const std::vector<Edge> &e, int s)
{
    std::vector<W> d(n, INF);
    d[s] = 0;
    for (int round = 1; round < n; round++) {
        bool changed = false;
        for (const auto &x : e)
            if (d[x.u] < INF && d[x.u] + x.w < d[x.v]) { d[x.v] = d[x.u] + x.w; changed = true; }
        if (!changed) return d;                       // converged early
    }
    // round V: whoever still improves is affected by a negative cycle; BFS
    // forward from them marks everything they reach as -infinity.
    std::vector<std::vector<int>> adj(n);
    for (const auto &x : e) adj[x.u].push_back(x.v);
    std::queue<int> q;
    for (const auto &x : e)
        if (d[x.u] < INF && d[x.u] + x.w < d[x.v] && d[x.v] != NEG_INF) { d[x.v] = NEG_INF; q.push(x.v); }
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        for (int v : adj[u]) if (d[v] != NEG_INF) { d[v] = NEG_INF; q.push(v); }
    }
    return d;
}

struct AllPairs {
    std::vector<std::vector<W>> d;
    std::vector<std::vector<int>> nxt;      // first hop, -1 = no path

    AllPairs(int n, const std::vector<Edge> &e) : d(n, std::vector<W>(n, INF)), nxt(n, std::vector<int>(n, -1))
    {
        for (int i = 0; i < n; i++) { d[i][i] = 0; nxt[i][i] = i; }
        for (const auto &x : e)
            if (x.w < d[x.u][x.v]) { d[x.u][x.v] = x.w; nxt[x.u][x.v] = x.v; }
        for (int k = 0; k < n; k++)                  // O(V^3)
            for (int i = 0; i < n; i++) {
                if (d[i][k] >= INF) continue;
                for (int j = 0; j < n; j++)
                    if (d[k][j] < INF && d[i][k] + d[k][j] < d[i][j]) {
                        d[i][j] = d[i][k] + d[k][j];
                        nxt[i][j] = nxt[i][k];       // go toward k first
                    }
            }
    }
    std::vector<int> path(int i, int j) const
    {
        std::vector<int> p;
        if (nxt[i][j] == -1) return p;
        for (p.push_back(i); i != j; p.push_back(i)) i = nxt[i][j];
        return p;
    }
};

int main()
{
    // G7 (CLRS): s=0 t=1 y=2 x=3 z=4
    const std::vector<Edge> g7 = { {0,1,6},{0,2,7},{1,2,8},{1,3,5},{1,4,-4},
                                   {3,1,-2},{2,3,-3},{2,4,9},{4,0,2},{4,3,7} };
    auto d = bellman_ford(5, g7, 0);
    std::printf("Bellman-Ford G7 from 0:");
    for (W x : d) std::printf(" %lld", x);
    std::printf("\n");
    CHECK((d == std::vector<W>{0, 2, 7, 4, -2}));

    // negative cycle 1->2->3->1 (-1); 4 is reachable from it; 5 is not.
    const std::vector<Edge> nc = { {0,1,1},{1,2,-1},{2,3,-1},{3,1,1},{3,4,2},{0,5,3} };
    auto dn = bellman_ford(6, nc, 0);
    std::printf("negative cycle graph:   ");
    for (W x : dn) { if (x == NEG_INF) std::printf(" -inf"); else std::printf(" %lld", x); }
    std::printf("\n");
    CHECK(dn[0] == 0 && dn[1] == NEG_INF && dn[2] == NEG_INF && dn[3] == NEG_INF && dn[4] == NEG_INF && dn[5] == 3);

    // G8 Floyd-Warshall with routing table
    const std::vector<Edge> g8 = { {0,1,3},{0,3,7},{1,0,8},{1,2,2},{2,0,5},{2,3,1},{3,0,2} };
    AllPairs ap(4, g8);
    std::printf("\nFloyd-Warshall G8 distance matrix / next-hop table:\n");
    for (int i = 0; i < 4; i++) {
        std::printf("  ");
        for (int j = 0; j < 4; j++) std::printf("%3lld", ap.d[i][j]);
        std::printf("      ");
        for (int j = 0; j < 4; j++) std::printf("%3d", ap.nxt[i][j]);
        std::printf("\n");
    }
    const std::vector<std::vector<W>> expect = { {0,3,5,6}, {5,0,2,3}, {3,6,0,1}, {2,5,7,0} };
    CHECK(ap.d == expect);
    auto p = ap.path(1, 0);
    std::printf("route 1 -> 0:");
    for (int v : p) std::printf(" %d", v);
    std::printf("  (cost %lld; the direct 1->0 link costs 8)\n", ap.d[1][0]);
    CHECK((p == std::vector<int>{1, 2, 3, 0}));

    std::mt19937 rng(65537);
    int with_cycle = 0;
    for (int t = 0; t < 300; t++) {
        int n = 1 + static_cast<int>(rng() % 9), m = static_cast<int>(rng() % (2 * n + 1));
        std::vector<Edge> e;
        for (int i = 0; i < m; i++)
            e.push_back({ static_cast<int>(rng() % n), static_cast<int>(rng() % n), static_cast<W>(rng() % 15) - 3 });
        AllPairs a(n, e);
        for (int s = 0; s < n; s++) {
            auto b = bellman_ford(n, e, s);
            for (int v = 0; v < n; v++) {
                // v is -inf from s <=> some k with d[k][k] < 0, s reaches k, k reaches v
                bool neg = false;
                for (int k = 0; k < n; k++) neg |= a.d[s][k] < INF && a.d[k][k] < 0 && a.d[k][v] < INF;
                CHECK((b[v] == NEG_INF) == neg);
                if (!neg) {
                    CHECK(b[v] == a.d[s][v]);
                    if (b[v] < INF) {                // the rebuilt path must cost exactly d
                        auto path = a.path(s, v);
                        W c = 0;
                        for (size_t i = 0; i + 1 < path.size(); i++) {
                            W best = INF;
                            for (const auto &x : e) if (x.u == path[i] && x.v == path[i + 1]) best = std::min(best, x.w);
                            c += best;
                        }
                        CHECK(c == b[v]);
                    }
                }
                with_cycle += neg;
            }
        }
    }
    std::printf("random cross-check: 300 graphs, Bellman-Ford (incl. %d -inf answers) == Floyd-Warshall,\n"
                "rebuilt paths cost exactly the reported distance\nOK\n", with_cycle);
    CHECK(with_cycle > 0);
    return 0;
}
