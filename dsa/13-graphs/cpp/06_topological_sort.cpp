/*
 * 06_topological_sort.cpp — Kahn (FIFO and "smallest-first" with a
 *                           priority_queue), DFS reverse-postorder, and
 *                           dependency resolution by NAME (like make/apt)
 *
 * Concept : Kahn = keep a set of "ready" vertices (in-degree 0); output one,
 *           decrement its successors, add newly-ready ones. Using a min-heap
 *           for the ready set gives the lexicographically smallest order —
 *           handy when you need a deterministic, reproducible build order.
 *           Real tools work with names, so we map strings to ids with an
 *           unordered_map first.
 * Build   : make run P=dsa/13-graphs/cpp/06_topological_sort.cpp
 * Read    : ../README.md and ../notes/05-ordering.md
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Graph = std::vector<std::vector<int>>;

// Kahn's algorithm, generic over the "ready set" container.
// Returns nullopt if the graph has a cycle. O(V + E) with a queue,
// O(V log V + E) with a priority_queue.
template <class Ready>
std::optional<std::vector<int>> kahn(const Graph &g)
{
    std::vector<int> indeg(g.size(), 0), order;
    for (const auto &out : g) for (int v : out) ++indeg[v];
    Ready ready;
    for (size_t v = 0; v < g.size(); v++) if (indeg[v] == 0) ready.push(static_cast<int>(v));
    while (!ready.empty()) {
        int u;
        if constexpr (std::is_same_v<Ready, std::queue<int>>) u = ready.front();
        else u = ready.top();
        ready.pop();
        order.push_back(u);
        for (int v : g[u]) if (--indeg[v] == 0) ready.push(v);
    }
    if (order.size() != g.size()) return std::nullopt;   // leftover = on/behind a cycle
    return order;
}

using Fifo = std::queue<int>;
using MinFirst = std::priority_queue<int, std::vector<int>, std::greater<int>>;

std::optional<std::vector<int>> dfs_topo(const Graph &g)
{
    enum { White, Gray, Black };
    std::vector<int> color(g.size(), White), post;
    bool cyclic = false;
    std::function<void(int)> visit = [&](int u) {
        color[u] = Gray;
        for (int v : g[u]) {
            if (color[v] == Gray) cyclic = true;
            else if (color[v] == White) visit(v);
        }
        color[u] = Black;
        post.push_back(u);
    };
    for (size_t u = 0; u < g.size(); u++) if (color[u] == White) visit(static_cast<int>(u));
    if (cyclic) return std::nullopt;
    std::reverse(post.begin(), post.end());
    return post;
}

bool valid(const Graph &g, const std::vector<int> &order)
{
    std::vector<int> pos(g.size());
    for (size_t i = 0; i < order.size(); i++) pos[order[i]] = static_cast<int>(i);
    for (size_t u = 0; u < g.size(); u++)
        for (int v : g[u]) if (pos[u] >= pos[v]) return false;
    return order.size() == g.size();
}

// Named dependency graph: "pkg depends on dep" => edge dep -> pkg.
struct NamedDag {
    std::unordered_map<std::string, int> id;
    std::vector<std::string> name;
    Graph g;
    int get(const std::string &s)
    {
        auto [it, inserted] = id.try_emplace(s, static_cast<int>(name.size()));
        if (inserted) { name.push_back(s); g.emplace_back(); }
        return it->second;
    }
    void depends(const std::string &pkg, const std::string &dep) { int p = get(pkg); g[get(dep)].push_back(p); }
};

int main()
{
    // Same build DAG as the C version: 0 app 1 main.o 2 util.o 3 config.h 4 main.c 5 util.c
    Graph b(6);
    for (auto [u, v] : std::vector<std::pair<int, int>>{ {3,1},{4,1},{3,2},{5,2},{1,0},{2,0} }) b[u].push_back(v);
    auto fifo = kahn<Fifo>(b), small = kahn<MinFirst>(b), dfs = dfs_topo(b);
    CHECK(fifo && small && dfs);
    auto show = [](const char *label, const std::vector<int> &o) {
        std::printf("%-22s", label);
        for (int v : o) std::printf(" %d", v);
        std::printf("\n");
    };
    show("Kahn, FIFO queue:", *fifo);
    show("Kahn, min-heap:", *small);
    show("DFS reverse-postorder:", *dfs);
    CHECK((*fifo == std::vector<int>{3, 4, 5, 1, 2, 0}));
    CHECK((*small == std::vector<int>{3, 4, 1, 5, 2, 0}));           // smallest READY vertex each time
    CHECK((*dfs == std::vector<int>{5, 4, 3, 2, 1, 0}));
    CHECK(valid(b, *fifo) && valid(b, *small) && valid(b, *dfs));

    b[0].push_back(3);                                    // app -> config.h : cycle
    CHECK(!kahn<Fifo>(b) && !dfs_topo(b));
    std::printf("with bogus edge app->config.h: both report a cycle\n");

    // Package install order, by name (like apt/pip/cargo resolving dependencies).
    NamedDag pkgs;
    pkgs.depends("gstreamer1.0-plugins-good", "gstreamer1.0-plugins-base");
    pkgs.depends("gstreamer1.0-plugins-base", "libgstreamer1.0-0");
    pkgs.depends("libgstreamer1.0-0", "libglib2.0-0");
    pkgs.depends("gstreamer1.0-plugins-good", "libv4l-0");
    pkgs.depends("libv4l-0", "libc6");
    pkgs.depends("libglib2.0-0", "libc6");
    auto install = kahn<Fifo>(pkgs.g);
    CHECK(install && valid(pkgs.g, *install));
    std::printf("\ninstall order (dependencies first):\n");
    for (int v : *install) std::printf("  %s\n", pkgs.name[v].c_str());
    CHECK(pkgs.name[install->front()] == "libc6" && pkgs.name[install->back()] == "gstreamer1.0-plugins-good");

    std::mt19937 rng(99);
    for (int t = 0; t < 500; t++) {
        int n = 1 + static_cast<int>(rng() % 15);
        std::vector<int> perm(n);
        for (int i = 0; i < n; i++) perm[i] = i;
        std::shuffle(perm.begin(), perm.end(), rng);
        Graph g(n);
        bool dag = t % 2 == 0;
        int m = static_cast<int>(rng() % (2 * n + 1));
        for (int i = 0; i < m; i++) {
            int a = static_cast<int>(rng() % n), c = static_cast<int>(rng() % n);
            if (a == c) continue;
            if (dag && a > c) std::swap(a, c);
            g[perm[a]].push_back(perm[c]);
        }
        auto k1 = kahn<Fifo>(g), k2 = kahn<MinFirst>(g), d = dfs_topo(g);
        CHECK(k1.has_value() == d.has_value() && k1.has_value() == k2.has_value());
        if (dag) CHECK(k1.has_value());
        if (k1) CHECK(valid(g, *k1) && valid(g, *k2) && valid(g, *d));
    }
    std::printf("\nrandom cross-check: 500 graphs, all three agree on acyclicity, all orders valid\nOK\n");
    return 0;
}
