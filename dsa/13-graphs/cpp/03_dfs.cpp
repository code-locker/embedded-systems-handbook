/*
 * 03_dfs.cpp — DFS recursive (lambda) and iterative (explicit stack of
 *              {vertex, next-neighbour index}), discovery/finish times,
 *              edge classification
 *
 * Concept : WHITE/GRAY/BLACK colours + a clock. The colour of v when edge
 *           u->v is examined classifies it: WHITE tree, GRAY back (cycle),
 *           BLACK forward (d[u] < d[v]) or cross. The iterative version stores
 *           exactly what a recursion frame stores, so it reproduces the
 *           recursive times — and cannot overflow the call stack.
 * Build   : make run P=dsa/13-graphs/cpp/03_dfs.cpp
 * Read    : ../README.md and ../notes/03-traversals.md  (section "DFS")
 *
 *   G2 (directed): 0->1 0->2 0->3 1->3 2->1 3->0 4->5 5->2
 *     (cycle 0->1->3->0; picture in ../c/03_dfs.c and the notes)
 */
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stack>
#include <string>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Graph = std::vector<std::vector<int>>;
enum class Color { White, Gray, Black };

struct DfsResult {
    std::vector<int> d, f, parent;
    std::string kinds;          // one char per examined arc: T B F C
    std::string paren;          // "(0 (1 ... 1) 0)"
};

char classify(const DfsResult &r, const std::vector<Color> &c, int u, int v)
{
    if (c[v] == Color::White) return 'T';
    if (c[v] == Color::Gray) return 'B';
    return r.d[u] < r.d[v] ? 'F' : 'C';
}

DfsResult dfs_recursive(const Graph &g)
{
    const size_t n = g.size();
    DfsResult r{ std::vector<int>(n), std::vector<int>(n), std::vector<int>(n, -1), "", "" };
    std::vector<Color> color(n, Color::White);
    int time = 0;
    // std::function lets a lambda call itself; it costs an indirect call per
    // visit — fine for teaching, use a free function in hot code.
    std::function<void(int)> visit = [&](int u) {
        color[u] = Color::Gray;
        r.d[u] = ++time;
        r.paren += "(" + std::to_string(u) + " ";
        for (int v : g[u]) {
            char k = classify(r, color, u, v);
            r.kinds += k;
            if (k == 'T') { r.parent[v] = u; visit(v); }
        }
        color[u] = Color::Black;
        r.f[u] = ++time;
        r.paren += std::to_string(u) + ") ";
    };
    for (size_t u = 0; u < n; u++)
        if (color[u] == Color::White) visit(static_cast<int>(u));
    return r;
}

DfsResult dfs_iterative(const Graph &g)
{
    const size_t n = g.size();
    DfsResult r{ std::vector<int>(n), std::vector<int>(n), std::vector<int>(n, -1), "", "" };
    std::vector<Color> color(n, Color::White);
    int time = 0;
    std::stack<std::pair<int, size_t>> st;     // {vertex, index of next neighbour}
    auto discover = [&](int v) {
        color[v] = Color::Gray;
        r.d[v] = ++time;
        r.paren += "(" + std::to_string(v) + " ";
        st.push({ v, 0 });
    };
    for (size_t root = 0; root < n; root++) {
        if (color[root] != Color::White) continue;
        discover(static_cast<int>(root));
        while (!st.empty()) {
            auto &[u, i] = st.top();               // reference: we advance i in place
            if (i < g[u].size()) {
                int v = g[u][i++];
                char k = classify(r, color, u, v);
                r.kinds += k;
                if (k == 'T') { r.parent[v] = u; discover(v); }   // NOTE: invalidates u, i
            } else {
                int done = u;
                st.pop();
                color[done] = Color::Black;
                r.f[done] = ++time;
                r.paren += std::to_string(done) + ") ";
            }
        }
    }
    return r;
}

int main()
{
    Graph g(6);
    for (auto [u, v] : std::vector<std::pair<int, int>>{ {0,1},{0,2},{0,3},{1,3},{2,1},{3,0},{4,5},{5,2} })
        g[u].push_back(v);

    DfsResult rec = dfs_recursive(g), it = dfs_iterative(g);
    std::printf("v : d  f  parent\n");
    for (int v = 0; v < 6; v++) std::printf("%d : %-2d %-2d %d\n", v, rec.d[v], rec.f[v], rec.parent[v]);
    std::printf("parenthesis: %s\n", rec.paren.c_str());
    std::printf("edge kinds in examination order (0-1,1-3,3-0,0-2,2-1,0-3,4-5,5-2): %s\n", rec.kinds.c_str());

    CHECK((rec.d == std::vector<int>{1, 2, 6, 3, 9, 10}));
    CHECK((rec.f == std::vector<int>{8, 5, 7, 4, 12, 11}));
    CHECK(rec.kinds == "TTBTCFTC");
    CHECK(it.d == rec.d && it.f == rec.f && it.parent == rec.parent);
    CHECK(it.kinds == rec.kinds && it.paren == rec.paren);
    std::printf("iterative == recursive (times, parents, edge kinds)\n");

    // Deep path graph: 200000 vertices in a line. The iterative version is
    // safe at any depth; recursion this deep would risk the 8 MB stack
    // (and ASan makes frames bigger still), so we only run the iterative one.
    const int n = 200000;
    Graph line(n);
    for (int v = 0; v + 1 < n; v++) line[v].push_back(v + 1);
    DfsResult deep = dfs_iterative(line);
    CHECK(deep.d[n - 1] == n && deep.f[n - 1] == n + 1 && deep.f[0] == 2 * n);
    std::printf("iterative DFS on a %d-vertex path: depth %d, no stack overflow\nOK\n", n, n);
    return 0;
}
