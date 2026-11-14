/*
 * 05_cycles_and_bipartite.cpp — cycle detection (undirected via DSU,
 *                               directed via 3 colours) and bipartite check
 *
 * Concept : Undirected: an edge whose two ends are ALREADY connected closes a
 *           cycle — a DSU answers that in ~O(1) per edge (no DFS needed).
 *           Directed: only an edge into a GRAY vertex (on the current DFS
 *           path) closes a cycle; we return the cycle itself.
 *           Bipartite: BFS 2-colouring; a same-colour edge = odd cycle.
 *           (The C version does undirected cycles with DFS; this one shows
 *           the DSU way.)
 * Build   : make run P=dsa/13-graphs/cpp/05_cycles_and_bipartite.cpp
 * Read    : ../README.md and ../notes/04-connectivity.md
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Graph = std::vector<std::vector<int>>;
using Edges = std::vector<std::pair<int, int>>;

struct Dsu {
    std::vector<int> p;
    explicit Dsu(int n) : p(n) { std::iota(p.begin(), p.end(), 0); }
    int find(int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }   // path halving
    bool unite(int a, int b) { a = find(a); b = find(b); if (a == b) return false; p[b] = a; return true; }
};

// First edge that closes a cycle, or nullopt for a forest. O(E alpha(V)).
std::optional<std::pair<int, int>> undirected_cycle_edge(int n, const Edges &e)
{
    Dsu d(n);
    for (auto [u, v] : e)
        if (!d.unite(u, v)) return std::make_pair(u, v);
    return std::nullopt;
}

// Directed: returns the cycle's vertices in order, or empty. O(V + E).
std::vector<int> directed_cycle(const Graph &g)
{
    enum { White, Gray, Black };
    std::vector<int> color(g.size(), White), parent(g.size(), -1), cyc;
    // explicit-stack DFS so this works for any depth
    for (size_t s = 0; s < g.size() && cyc.empty(); s++) {
        if (color[s] != White) continue;
        std::vector<std::pair<int, size_t>> st{ { static_cast<int>(s), 0 } };
        color[s] = Gray;
        while (!st.empty() && cyc.empty()) {
            auto &[u, i] = st.back();
            if (i == g[u].size()) { color[u] = Black; st.pop_back(); continue; }
            int v = g[u][i++];
            if (color[v] == Gray) {                       // back edge u->v
                for (int x = u; x != v; x = parent[x]) cyc.push_back(x);
                cyc.push_back(v);
                std::reverse(cyc.begin(), cyc.end());
            } else if (color[v] == White) {
                parent[v] = u;
                color[v] = Gray;
                st.push_back({ v, 0 });                   // invalidates u, i: loop re-reads
            }
        }
    }
    return cyc;
}

// side[v] in {0,1}, or nullopt + the conflicting edge.
std::optional<std::vector<int>> two_colour(const Graph &g, std::pair<int, int> *conflict)
{
    std::vector<int> side(g.size(), -1);
    for (size_t s = 0; s < g.size(); s++) {
        if (side[s] != -1) continue;
        std::queue<int> q;
        q.push(static_cast<int>(s));
        side[s] = 0;
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int v : g[u]) {
                if (side[v] == -1) { side[v] = 1 - side[u]; q.push(v); }
                else if (side[v] == side[u]) { if (conflict) *conflict = { u, v }; return std::nullopt; }
            }
        }
    }
    return side;
}

Graph undirected(int n, const Edges &e)
{
    Graph g(n);
    for (auto [u, v] : e) { g[u].push_back(v); g[v].push_back(u); }
    return g;
}

int main()
{
    const Edges g1 = { {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7} };
    auto ce = undirected_cycle_edge(8, g1);
    CHECK(ce && ce->first == 3 && ce->second == 4);
    std::printf("1) G1 via DSU: edge %d-%d joins two already-connected vertices -> cycle (0-1-4-3)\n",
                ce->first, ce->second);
    const Edges tree = { {0,1},{0,3},{1,2},{1,4},{2,5},{4,6},{5,7} };
    CHECK(!undirected_cycle_edge(8, tree));
    std::printf("   G1 minus 3-4, 6-7: a tree, no cycle edge\n");

    Graph g2(6);
    for (auto [u, v] : Edges{ {0,1},{0,2},{0,3},{1,3},{2,1},{3,0},{4,5},{5,2} }) g2[u].push_back(v);
    auto cyc = directed_cycle(g2);
    std::printf("\n2) G2 directed cycle:");
    for (int v : cyc) std::printf(" %d ->", v);
    std::printf(" %d\n", cyc.front());
    CHECK((cyc == std::vector<int>{0, 1, 3}));
    g2[3].clear();                                   // remove 3->0
    CHECK(directed_cycle(g2).empty());
    std::printf("   without 3->0: no cycle (a DAG)\n");

    std::pair<int, int> bad{ -1, -1 };
    auto side = two_colour(undirected(8, g1), &bad);
    CHECK(side && (*side == std::vector<int>{0, 1, 0, 1, 0, 1, 1, 0}));
    std::printf("\n3) G1 is bipartite: sides =");
    for (int s : *side) std::printf(" %c", s ? 'B' : 'A');
    Edges g1t = g1;
    g1t.push_back({ 0, 4 });
    CHECK(!two_colour(undirected(8, g1t), &bad));
    std::printf("\n   G1 + 0-4: conflict at edge %d-%d -> odd cycle -> not bipartite\n", bad.first, bad.second);
    CHECK(bad.first == 1 && bad.second == 4);

    // cross-checks: forest <=> m == n - components; bipartite <=> some 2^n colouring works;
    // directed cycle <=> some vertex reaches itself
    std::mt19937 rng(777);
    for (int t = 0; t < 400; t++) {
        int n = 1 + static_cast<int>(rng() % 10), m = static_cast<int>(rng() % (2 * n));
        Edges e;
        for (int i = 0; i < m; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n);
            if (a != b) e.push_back({ a, b });
        }
        Dsu d(n);
        int comps = n;
        for (auto [a, b] : e) comps -= d.unite(a, b);
        CHECK(undirected_cycle_edge(n, e).has_value() == (static_cast<int>(e.size()) != n - comps));

        bool brute_bip = false;
        for (unsigned mask = 0; mask < (1u << n) && !brute_bip; mask++)
            brute_bip = std::all_of(e.begin(), e.end(), [mask](const std::pair<int, int> &x) {
                return ((mask >> x.first) & 1) != ((mask >> x.second) & 1);
            });
        CHECK(two_colour(undirected(n, e), nullptr).has_value() == brute_bip);

        Graph dg(n);
        std::vector<std::vector<char>> r(n, std::vector<char>(n, 0));
        for (auto [a, b] : e) { dg[a].push_back(b); r[a][b] = 1; }
        for (int k = 0; k < n; k++) for (int i = 0; i < n; i++) for (int j = 0; j < n; j++)
            if (r[i][k] && r[k][j]) r[i][j] = 1;
        bool brute_cyc = false;
        for (int v = 0; v < n; v++) brute_cyc |= r[v][v] != 0;
        auto c = directed_cycle(dg);
        CHECK(!c.empty() == brute_cyc);
        for (size_t i = 0; i < c.size(); i++) {       // returned cycle must be real
            int a = c[i], b = c[(i + 1) % c.size()];
            CHECK(std::find(dg[a].begin(), dg[a].end(), b) != dg[a].end());
        }
    }
    std::printf("\nrandom cross-check: 400 graphs, all three detectors == brute force\nOK\n");
    return 0;
}
