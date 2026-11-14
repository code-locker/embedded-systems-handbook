/*
 * 08_scc.cpp — strongly connected components: Kosaraju and Tarjan, plus the
 *              condensation DAG
 *
 * Concept : Kosaraju: DFS for finish order, then DFS on the TRANSPOSED graph
 *           in decreasing finish order — each tree is an SCC. Tarjan: one DFS
 *           with a stack; when low[u] == disc[u], u is the root of an SCC and
 *           everything above it on the stack belongs to it.
 *           Condensing SCCs into single nodes always yields a DAG — e.g. turn
 *           a module dependency graph with cycles into an orderable one, or
 *           find groups of mutually-waiting threads in a wait-for graph.
 * Build   : make run P=dsa/13-graphs/cpp/08_scc.cpp
 * Read    : ../README.md and ../notes/04-connectivity.md  (section "SCC")
 *
 *   G5:  {0 -> 1 -> 2 -> 0}  --1->3-->  {3 -> 4 -> 5 -> 3}  --5->6-->  {6 <-> 7}
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Graph = std::vector<std::vector<int>>;

// Returns comp[v]; Kosaraju numbers components in topological order.
std::vector<int> kosaraju(const Graph &g)
{
    const int n = static_cast<int>(g.size());
    Graph gt(n);
    for (int u = 0; u < n; u++) for (int v : g[u]) gt[v].push_back(u);

    std::vector<char> seen(n, 0);
    std::vector<int> order;
    std::function<void(int)> dfs1 = [&](int u) {
        seen[u] = 1;
        for (int v : g[u]) if (!seen[v]) dfs1(v);
        order.push_back(u);
    };
    for (int u = 0; u < n; u++) if (!seen[u]) dfs1(u);

    std::vector<int> comp(n, -1);
    std::function<void(int, int)> dfs2 = [&](int u, int c) {
        comp[u] = c;
        for (int v : gt[u]) if (comp[v] == -1) dfs2(v, c);
    };
    int c = 0;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (comp[*it] == -1) dfs2(*it, c++);
    return comp;
}

// Tarjan: components come out in REVERSE topological order.
std::vector<int> tarjan(const Graph &g)
{
    const int n = static_cast<int>(g.size());
    std::vector<int> disc(n, -1), low(n), comp(n, -1), st;
    std::vector<char> on(n, 0);
    int timer = 0, c = 0;
    std::function<void(int)> dfs = [&](int u) {
        disc[u] = low[u] = timer++;
        st.push_back(u);
        on[u] = 1;
        for (int v : g[u]) {
            if (disc[v] == -1) { dfs(v); low[u] = std::min(low[u], low[v]); }
            else if (on[v]) low[u] = std::min(low[u], disc[v]);
        }
        if (low[u] == disc[u]) {
            int v;
            do { v = st.back(); st.pop_back(); on[v] = 0; comp[v] = c; } while (v != u);
            ++c;
        }
    };
    for (int u = 0; u < n; u++) if (disc[u] == -1) dfs(u);
    return comp;
}

// Same partition? (ids may differ between algorithms)
bool same_partition(const std::vector<int> &a, const std::vector<int> &b)
{
    for (size_t i = 0; i < a.size(); i++)
        for (size_t j = 0; j < a.size(); j++)
            if ((a[i] == a[j]) != (b[i] == b[j])) return false;
    return true;
}

int main()
{
    Graph g(8);
    for (auto [u, v] : std::vector<std::pair<int, int>>{ {0,1},{1,2},{2,0},{1,3},{3,4},{4,5},{5,3},{5,6},{6,7},{7,6} })
        g[u].push_back(v);

    auto k = kosaraju(g), t = tarjan(g);
    std::printf("vertex   : 0 1 2 3 4 5 6 7\nKosaraju :");
    for (int c : k) std::printf(" %d", c);
    std::printf("   (topological order of SCCs)\nTarjan   :");
    for (int c : t) std::printf(" %d", c);
    std::printf("   (reverse topological order)\n");
    CHECK((k == std::vector<int>{0, 0, 0, 1, 1, 1, 2, 2}));
    CHECK((t == std::vector<int>{2, 2, 2, 1, 1, 1, 0, 0}));

    std::set<std::pair<int, int>> dag;           // condensation edges, deduplicated
    for (int u = 0; u < 8; u++) for (int v : g[u]) if (k[u] != k[v]) dag.insert({ k[u], k[v] });
    std::printf("condensation DAG:");
    for (auto [a, b] : dag) std::printf(" %c->%c", 'A' + a, 'A' + b);
    std::printf("\n");
    CHECK((dag == std::set<std::pair<int, int>>{ {0, 1}, {1, 2} }));

    std::mt19937 rng(4242);
    for (int it = 0; it < 500; it++) {
        int n = 1 + static_cast<int>(rng() % 14), m = static_cast<int>(rng() % (2 * n + 1));
        Graph r(n);
        std::vector<std::vector<char>> reach(n, std::vector<char>(n, 0));
        for (int i = 0; i < n; i++) reach[i][i] = 1;
        for (int i = 0; i < m; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n);
            r[a].push_back(b);
            reach[a][b] = 1;
        }
        for (int x = 0; x < n; x++) for (int i = 0; i < n; i++) for (int j = 0; j < n; j++)
            if (reach[i][x] && reach[x][j]) reach[i][j] = 1;
        auto a = kosaraju(r), b = tarjan(r);
        CHECK(same_partition(a, b));
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) CHECK((a[i] == a[j]) == (reach[i][j] && reach[j][i]));
    }
    std::printf("random cross-check: 500 digraphs, Kosaraju == Tarjan == mutual reachability\nOK\n");
    return 0;
}
