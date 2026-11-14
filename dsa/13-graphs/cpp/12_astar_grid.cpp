/*
 * 12_astar_grid.cpp — A* as a template over the heuristic: h = 0
 *                     (Dijkstra), h = Manhattan (admissible), h = 5 x
 *                     Manhattan (inadmissible: fast but may be wrong)
 *
 * Concept : A* pops the node with the smallest f = g + h. If h never
 *           OVERestimates the true remaining cost (admissible) — and is
 *           consistent: h(u) <= cost(u,v) + h(v) — the first time the goal is
 *           popped its cost is optimal. Inflate h (weighted A*) and the search
 *           rushes greedily at the goal: fewer expansions, but the path can
 *           be longer than optimal. Games and robots sometimes accept that.
 * Build   : make run P=dsa/13-graphs/cpp/12_astar_grid.cpp
 * Read    : ../README.md and ../notes/06-shortest-paths.md  (section "A*")
 */
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

struct Cell { int r, c; bool operator==(const Cell &o) const { return r == o.r && c == o.c; } };

struct Search { int cost = -1; int expanded = 0; std::vector<std::string> drawing; };

template <class Heuristic>
Search astar(const std::vector<std::string> &grid, Cell s, Cell t, Heuristic h)
{
    const int R = static_cast<int>(grid.size()), C = static_cast<int>(grid[0].size());
    auto id = [C](Cell x) { return x.r * C + x.c; };
    std::vector<int> g(static_cast<size_t>(R) * C, -1), parent(g.size(), -1);
    std::vector<char> closed(g.size(), 0);
    // (f, -g, cell): ties on f prefer larger g (deeper) — fewer expansions
    using Item = std::tuple<int, int, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
    Search out;
    g[id(s)] = 0;
    open.push({ h(s), 0, id(s) });
    while (!open.empty()) {
        auto [f, neg_g, u] = open.top();
        open.pop();
        (void)f;
        if (closed[u]) continue;
        closed[u] = 1;
        ++out.expanded;
        if (u == id(t)) { out.cost = -neg_g; break; }
        const Cell cu{ u / C, u % C };
        const Cell nbr[4] = { {cu.r - 1, cu.c}, {cu.r + 1, cu.c}, {cu.r, cu.c - 1}, {cu.r, cu.c + 1} };
        for (Cell v : nbr) {
            if (v.r < 0 || v.r >= R || v.c < 0 || v.c >= C || grid[v.r][v.c] == '#') continue;
            int ng = -neg_g + 1;
            if (g[id(v)] == -1 || ng < g[id(v)]) {
                g[id(v)] = ng;
                parent[id(v)] = u;
                open.push({ ng + h(v), -ng, id(v) });
            }
        }
    }
    out.drawing = grid;
    for (size_t i = 0; i < closed.size(); i++)
        if (closed[i] && out.drawing[i / C][i % C] == '.') out.drawing[i / C][i % C] = 'o';
    if (out.cost >= 0)
        for (int v = parent[id(t)]; v != -1 && v != id(s); v = parent[v]) out.drawing[v / C][v % C] = '*';
    return out;
}

int bfs(const std::vector<std::string> &grid, Cell s, Cell t)
{
    const int R = static_cast<int>(grid.size()), C = static_cast<int>(grid[0].size());
    std::vector<int> d(static_cast<size_t>(R) * C, -1);
    std::queue<Cell> q;
    d[s.r * C + s.c] = 0;
    q.push(s);
    while (!q.empty()) {
        Cell u = q.front();
        q.pop();
        const Cell nbr[4] = { {u.r - 1, u.c}, {u.r + 1, u.c}, {u.r, u.c - 1}, {u.r, u.c + 1} };
        for (Cell v : nbr)
            if (v.r >= 0 && v.r < R && v.c >= 0 && v.c < C && grid[v.r][v.c] != '#' && d[v.r * C + v.c] < 0) {
                d[v.r * C + v.c] = d[u.r * C + u.c] + 1;
                q.push(v);
            }
    }
    return d[t.r * C + t.c];
}

void show(const char *title, const Search &s)
{
    std::printf("%s: cost %d, expanded %d\n", title, s.cost, s.expanded);
    for (const auto &row : s.drawing) {
        std::printf("    ");
        for (char ch : row) std::printf("%c ", ch);
        std::printf("\n");
    }
}

int main()
{
    // Found by a random search for a map where weighted A* goes wrong:
    // it rushes up toward G's row, gets walled in, and the cells it closed
    // on the way keep their bad g values (A* never reopens closed cells).
    const std::vector<std::string> grid = {
        "#...#.#G",
        ".....#..",
        ".....#..",
        ".....#..",
        "..#..#.#",
        "S.......",
    };
    const Cell s{ 5, 0 }, t{ 0, 7 };
    auto manhattan = [t](Cell x) { return std::abs(x.r - t.r) + std::abs(x.c - t.c); };

    Search dij = astar(grid, s, t, [](Cell) { return 0; });
    Search a = astar(grid, s, t, manhattan);
    Search w = astar(grid, s, t, [&manhattan](Cell x) { return 5 * manhattan(x); });
    show("h = 0 (Dijkstra)", dij);
    show("h = Manhattan (A*)", a);
    show("h = 5 x Manhattan (weighted A*, NOT admissible)", w);

    int opt = bfs(grid, s, t);
    std::printf("BFS optimum = %d\n", opt);
    CHECK(dij.cost == opt && a.cost == opt);
    CHECK(a.expanded < dij.expanded);
    CHECK(w.cost > opt);                         // inadmissible h: suboptimal path
    std::printf("A* is optimal with fewer expansions than Dijkstra; weighted A* returns a path of\n"
                "cost %d instead of %d: an overestimating heuristic breaks the optimality guarantee\n",
                w.cost, opt);

    std::mt19937 rng(2718);
    for (int it = 0; it < 300; it++) {
        int R = 2 + static_cast<int>(rng() % 10), C = 2 + static_cast<int>(rng() % 10);
        std::vector<std::string> gr(R, std::string(C, '.'));
        for (auto &row : gr) for (char &ch : row) ch = rng() % 100 < 28 ? '#' : '.';
        Cell a1{ static_cast<int>(rng() % R), static_cast<int>(rng() % C) };
        Cell b1{ static_cast<int>(rng() % R), static_cast<int>(rng() % C) };
        gr[a1.r][a1.c] = gr[b1.r][b1.c] = '.';
        auto mh = [b1](Cell x) { return std::abs(x.r - b1.r) + std::abs(x.c - b1.c); };
        int ref = bfs(gr, a1, b1);
        CHECK(astar(gr, a1, b1, mh).cost == ref);
        CHECK(astar(gr, a1, b1, [](Cell) { return 0; }).cost == ref);
        int wc = astar(gr, a1, b1, [&mh](Cell x) { return 5 * mh(x); }).cost;
        CHECK(wc == -1 ? ref == -1 : wc >= ref);   // weighted: never better than optimal
    }
    std::printf("random cross-check: 300 mazes, A* == Dijkstra == BFS, weighted A* >= optimum\nOK\n");
    return 0;
}
