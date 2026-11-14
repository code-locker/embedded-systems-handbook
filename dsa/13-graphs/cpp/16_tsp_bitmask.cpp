/*
 * 16_tsp_bitmask.cpp — Held-Karp TSP with a vector-based DP table, compared
 *                      to std::next_permutation brute force
 *
 * Concept : dp[mask][v] = cheapest way to start at 0, visit exactly `mask`,
 *           and stand at v. Order inside `mask` no longer matters — that
 *           collapse from (n-1)! orders to 2^n * n states is the whole trick.
 *           std::next_permutation is the idiomatic brute force: it walks
 *           every ordering of the remaining cities in lexicographic order.
 *           Also timed: n = 16 with Held-Karp (~1.7e7 steps) is instant;
 *           brute force would need 15! = 1.3e12 tours.
 * Build   : make run P=dsa/13-graphs/cpp/16_tsp_bitmask.cpp
 * Read    : ../README.md and ../notes/08-advanced.md  (section "TSP")
 */
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using Matrix = std::vector<std::vector<int>>;
constexpr int INF = INT_MAX / 4;

struct Tour { int cost; std::vector<int> order; };

Tour held_karp(const Matrix &d)
{
    const int n = static_cast<int>(d.size());
    if (n == 1) return { 0, {0, 0} };
    const size_t full = (size_t{1} << n) - 1;
    std::vector<std::vector<int>> dp(full + 1, std::vector<int>(n, INF)), par(full + 1, std::vector<int>(n, -1));
    dp[1][0] = 0;
    for (size_t mask = 1; mask <= full; mask += 2)        // odd masks: contain city 0
        for (int v = 0; v < n; v++) {
            if (!(mask >> v & 1) || dp[mask][v] >= INF) continue;
            for (int w = 0; w < n; w++) {
                if (mask >> w & 1) continue;
                size_t nm = mask | size_t{1} << w;
                if (dp[mask][v] + d[v][w] < dp[nm][w]) { dp[nm][w] = dp[mask][v] + d[v][w]; par[nm][w] = v; }
            }
        }
    Tour best{ INF, {} };
    int last = -1;
    for (int v = 1; v < n; v++)
        if (dp[full][v] + d[v][0] < best.cost) { best.cost = dp[full][v] + d[v][0]; last = v; }
    size_t mask = full;
    for (int v = last; v != -1;) {                        // walk parents back to 0
        best.order.push_back(v);
        int p = par[mask][v];
        mask &= ~(size_t{1} << v);
        v = p;
    }
    std::reverse(best.order.begin(), best.order.end());
    best.order.push_back(0);
    return best;
}

int brute(const Matrix &d)
{
    const int n = static_cast<int>(d.size());
    std::vector<int> rest(n - 1);
    std::iota(rest.begin(), rest.end(), 1);
    int best = INF;
    do {
        int c = 0, cur = 0;
        for (int v : rest) { c += d[cur][v]; cur = v; }
        best = std::min(best, c + d[cur][0]);
    } while (std::next_permutation(rest.begin(), rest.end()));
    return best;
}

int main()
{
    const Matrix d = { {0,10,15,20}, {10,0,35,25}, {15,35,0,30}, {20,25,30,0} };
    Tour t = held_karp(d);
    std::printf("4 cities: optimal tour");
    for (int v : t.order) std::printf(" %d", v);
    std::printf(" = %d (brute force %d)\n", t.cost, brute(d));
    CHECK(t.cost == 80 && brute(d) == 80);
    CHECK(t.order.front() == 0 && t.order.back() == 0 && t.order.size() == 5);

    std::mt19937 rng(31415);
    for (int it = 0; it < 150; it++) {
        int n = 2 + static_cast<int>(rng() % 7);
        Matrix m(n, std::vector<int>(n, 0));
        for (int i = 0; i < n; i++) for (int j = 0; j < i; j++) m[i][j] = m[j][i] = 1 + static_cast<int>(rng() % 100);
        Tour h = held_karp(m);
        CHECK(h.cost == brute(m));
        int c = 0;
        std::vector<int> seen(n, 0);
        for (size_t i = 0; i + 1 < h.order.size(); i++) { c += m[h.order[i]][h.order[i + 1]]; ++seen[h.order[i]]; }
        CHECK(c == h.cost && std::all_of(seen.begin(), seen.end(), [](int s) { return s == 1; }));
    }
    std::printf("random cross-check: 150 instances (n <= 8), Held-Karp == next_permutation brute force\n");

    // n = 16 random cities on a plane (Manhattan distances)
    const int n = 16;
    std::vector<std::pair<int, int>> pts(n);
    for (auto &p : pts) p = { static_cast<int>(rng() % 100), static_cast<int>(rng() % 100) };
    Matrix big(n, std::vector<int>(n));
    for (int i = 0; i < n; i++) for (int j = 0; j < n; j++)
        big[i][j] = std::abs(pts[i].first - pts[j].first) + std::abs(pts[i].second - pts[j].second);
    auto t0 = std::chrono::steady_clock::now();
    Tour bt = held_karp(big);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("n = 16: optimal tour cost %d found in %lld ms (2^16 * 16 states)\n", bt.cost, static_cast<long long>(ms));
    CHECK(bt.order.size() == static_cast<size_t>(n) + 1);
    std::printf("OK\n");
    return 0;
}
