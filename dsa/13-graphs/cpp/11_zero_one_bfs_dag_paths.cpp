/*
 * 11_zero_one_bfs_dag_paths.cpp — 0-1 BFS on a grid with std::deque, and
 *                                 DAG shortest / longest paths
 *
 * Concept : 0-1 BFS: weights are only 0 or 1 -> std::deque: push_front for a
 *           0-edge, push_back for a 1-edge. O(V + E), no heap.
 *           Grid example: arrows in cells; following a cell's arrow is free,
 *           changing it costs 1 (a classic "minimum modifications" puzzle).
 *           DAG: relax in topological order; one pass, negative weights OK;
 *           longest path (critical path) = shortest path with negated weights.
 * Build   : make run P=dsa/13-graphs/cpp/11_zero_one_bfs_dag_paths.cpp
 * Read    : ../README.md and ../notes/06-shortest-paths.md
 *
 *   Arrow grid (R=right L=left D=down U=up); start top-left, goal
 *   bottom-right. Moving the way the arrow points costs 0, any other
 *   direction costs 1 (you "repaint" the arrow).
 *
 *        R R R D
 *        L L L D        cheapest: follow R R R, D D, then one repaint
 *        R R R U   <-   (the U at row 2 col 3 must become D): cost 1
 *        U L L L
 */
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

struct Arc { int to, w; };
using Graph = std::vector<std::vector<Arc>>;
constexpr int INF = INT_MAX / 4;

std::vector<int> zero_one_bfs(const Graph &g, int s)
{
    std::vector<int> d(g.size(), INF);
    std::deque<int> dq;
    d[s] = 0;
    dq.push_back(s);
    while (!dq.empty()) {
        int u = dq.front();
        dq.pop_front();
        for (auto [v, w] : g[u])
            if (d[u] + w < d[v]) {
                d[v] = d[u] + w;
                if (w == 0) dq.push_front(v); else dq.push_back(v);
            }
    }
    return d;
}

std::vector<int> dijkstra(const Graph &g, int s)       // reference
{
    std::vector<int> d(g.size(), INF);
    std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<>> pq;
    d[s] = 0;
    pq.push({ 0, s });
    while (!pq.empty()) {
        auto [du, u] = pq.top();
        pq.pop();
        if (du > d[u]) continue;
        for (auto [v, w] : g[u]) if (du + w < d[v]) { d[v] = du + w; pq.push({ d[v], v }); }
    }
    return d;
}

// Grid -> implicit graph built explicitly: cell (r,c) = r*C + c, 4 arcs per cell.
Graph arrow_grid(const std::vector<std::string> &rows)
{
    const int R = static_cast<int>(rows.size()), C = static_cast<int>(rows[0].size());
    const int dr[4] = { 0, 0, 1, -1 }, dc[4] = { 1, -1, 0, 0 };
    const char dir[4] = { 'R', 'L', 'D', 'U' };
    Graph g(static_cast<size_t>(R) * C);
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++)
            for (int k = 0; k < 4; k++) {
                int nr = r + dr[k], nc = c + dc[k];
                if (nr < 0 || nr >= R || nc < 0 || nc >= C) continue;
                g[r * C + c].push_back({ nr * C + nc, rows[r][c] == dir[k] ? 0 : 1 });
            }
    return g;
}

// DAG shortest paths (sign=+1) or longest paths (sign=-1). Kahn for the order.
std::vector<int> dag_paths(const Graph &g, int s, int sign)
{
    std::vector<int> indeg(g.size(), 0), order, d(g.size(), INF);
    for (const auto &out : g) for (auto a : out) ++indeg[a.to];
    std::queue<int> q;
    for (size_t v = 0; v < g.size(); v++) if (!indeg[v]) q.push(static_cast<int>(v));
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        order.push_back(u);
        for (auto a : g[u]) if (--indeg[a.to] == 0) q.push(a.to);
    }
    CHECK(order.size() == g.size());                 // must be a DAG
    d[s] = 0;
    for (int u : order) {
        if (d[u] == INF) continue;
        for (auto [v, w] : g[u]) d[v] = std::min(d[v], d[u] + sign * w);
    }
    if (sign < 0) for (int &x : d) if (x != INF) x = -x;
    return d;
}

int main()
{
    const std::vector<std::string> grid = { "RRRD", "LLLD", "RRRU", "ULLL" };
    Graph g = arrow_grid(grid);
    auto d = zero_one_bfs(g, 0);
    std::printf("arrow grid, cost to reach each cell (repaints needed):\n");
    for (int r = 0; r < 4; r++) {
        std::printf("  ");
        for (int c = 0; c < 4; c++) std::printf("%d ", d[r * 4 + c]);
        std::printf("     %s\n", grid[r].c_str());
    }
    CHECK(d[15] == 1);
    CHECK(d == dijkstra(g, 0));

    std::mt19937 rng(1337);
    const char *dirs = "RLDU";
    for (int t = 0; t < 200; t++) {
        int R = 1 + static_cast<int>(rng() % 8), C = 1 + static_cast<int>(rng() % 8);
        std::vector<std::string> rows(R, std::string(C, 'R'));
        for (auto &row : rows) for (char &ch : row) ch = dirs[rng() % 4];
        Graph rg = arrow_grid(rows);
        CHECK(zero_one_bfs(rg, 0) == dijkstra(rg, 0));
    }
    std::printf("random cross-check: 200 arrow grids, 0-1 BFS == Dijkstra\n");

    // CLRS DAG (r=0 s=1 t=2 x=3 y=4 z=5), from s
    Graph dag(6);
    for (auto [u, v, w] : std::vector<std::tuple<int, int, int>>{
             {0,1,5},{0,2,3},{1,2,2},{1,3,6},{2,3,7},{2,4,4},{2,5,2},{3,4,-1},{3,5,1},{4,5,-2} })
        dag[u].push_back({ v, w });
    auto sp = dag_paths(dag, 1, +1), lp = dag_paths(dag, 1, -1);
    std::printf("\nDAG shortest from s:");
    for (int x : sp) { if (x == INF) std::printf(" inf"); else std::printf(" %d", x); }
    std::printf("\nDAG longest  from s:");
    for (int x : lp) { if (x == INF) std::printf(" inf"); else std::printf(" %d", x); }
    std::printf("\n");
    CHECK((sp == std::vector<int>{INF, 0, 2, 6, 5, 3}));
    CHECK((lp == std::vector<int>{INF, 0, 2, 9, 8, 10}));   // x = 2+7, y = 9-1, z = 9+1
    std::printf("OK\n");
    return 0;
}
