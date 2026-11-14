/*
 * 05_bitmask_dp.cpp — bitmask DP in C++: TSP with tour reconstruction,
 *                     minimum bins via submask enumeration (O(3^n)),
 *                     partition into k equal-sum subsets
 *
 * Concept : The state is a SET encoded as an unsigned int. Three standard
 *           moves: add one element (mask | 1<<i), test membership
 *           (mask >> i & 1), and walk all submasks of a mask with
 *           sub = (sub - 1) & mask. Summed over all masks, submask walking
 *           costs 3^n, because each element is in "not in mask",
 *           "in mask, not in sub" or "in sub".
 * Build   : make run P=dsa/17-dynamic-programming/cpp/05_bitmask_dp.cpp
 * Read    : ../README.md  (section "Bitmask DP")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

#define CHECK(cond) assert(cond)

using Matrix = std::vector<std::vector<int>>;
constexpr int INF = INT_MAX / 4;

struct Tour {
    int length;
    std::vector<int> order;   // starts at 0, every city once (return to 0 implied)
};

// Held-Karp with a parent table so the actual tour can be printed.
Tour tsp(const Matrix &d)
{
    const int n = static_cast<int>(d.size());
    if (n == 1) return {0, {0}};
    const unsigned full = (1u << n) - 1;
    Matrix dp(full + 1, std::vector<int>(static_cast<std::size_t>(n), INF));
    Matrix par(full + 1, std::vector<int>(static_cast<std::size_t>(n), -1));
    dp[1][0] = 0;
    for (unsigned mask = 1; mask <= full; mask += 2)          // odd masks only: contain city 0
        for (int v = 0; v < n; ++v) {
            if (!(mask >> v & 1u) || dp[mask][v] >= INF) continue;
            for (int u = 0; u < n; ++u) {
                if (mask >> u & 1u) continue;
                const unsigned nm = mask | 1u << u;
                if (dp[mask][v] + d[v][u] < dp[nm][u]) {
                    dp[nm][u] = dp[mask][v] + d[v][u];
                    par[nm][u] = v;                          // came to u from v
                }
            }
        }
    int best = INF, last = -1;
    for (int v = 1; v < n; ++v)
        if (dp[full][v] + d[v][0] < best) { best = dp[full][v] + d[v][0]; last = v; }
    // Walk the parents back: (full, last) -> (full without last, par) -> ...
    std::vector<int> order;
    for (unsigned mask = full; last != -1;) {
        order.push_back(last);
        const int prev = par[mask][last];
        mask &= ~(1u << last);
        last = prev;
    }
    std::reverse(order.begin(), order.end());
    return {best, order};
}

int tour_length(const Matrix &d, const std::vector<int> &order)
{
    int len = 0;
    for (std::size_t i = 0; i < order.size(); ++i) len += d[order[i]][order[(i + 1) % order.size()]];
    return len;
}

int tsp_brute(const Matrix &d)
{
    std::vector<int> perm(d.size());
    std::iota(perm.begin(), perm.end(), 0);
    int best = INF;
    do best = std::min(best, tour_length(d, perm));
    while (std::next_permutation(perm.begin() + 1, perm.end()));   // city 0 fixed first
    return best;
}

// Min bins of capacity C holding all items.
//   fits[s]  = sum of items in s <= C            (precomputed for all 2^n sets)
//   bins[m]  = min over non-empty submasks s of m with fits[s]: bins[m ^ s] + 1
// Submask walk:  for (s = m; s; s = (s - 1) & m)  visits every non-empty
// submask in decreasing order. Total over all m: O(3^n).
int min_bins(const std::vector<int> &w, int C)
{
    const unsigned n = static_cast<unsigned>(w.size()), full = (1u << n) - 1;
    std::vector<int> sum(full + 1, 0), bins(full + 1, INF);
    for (unsigned m = 1; m <= full; ++m) {
        const unsigned low = static_cast<unsigned>(__builtin_ctz(m));   // lowest set bit index
        sum[m] = sum[m & (m - 1)] + w[low];          // reuse the set minus its lowest bit
    }
    bins[0] = 0;
    for (unsigned m = 1; m <= full; ++m)
        for (unsigned s = m; s; s = (s - 1) & m)
            if (sum[s] <= C && bins[m ^ s] + 1 < bins[m]) bins[m] = bins[m ^ s] + 1;
    return bins[full];
}

// Brute force: put each item in bin 0..n-1 (n^n assignments), count used bins.
int bins_brute(const std::vector<int> &w, int C, std::size_t i, std::vector<int> &load)
{
    if (i == w.size())
        return static_cast<int>(std::count_if(load.begin(), load.end(), [](int x) { return x > 0; }));
    int best = INF;
    bool tried_empty = false;
    for (int &b : load) {
        if (b == 0) {                                // all empty bins are equivalent
            if (tried_empty) continue;
            tried_empty = true;
        }
        if (b + w[i] > C) continue;
        b += w[i];
        best = std::min(best, bins_brute(w, C, i + 1, load));
        b -= w[i];
    }
    return best;
}

// k equal-sum subsets. dp[mask] = (filled so far) % target, or -1 if this set
// of used items cannot be arranged into full buckets + one partial bucket.
// Items are only added to the CURRENT bucket while they fit: the order of
// buckets does not matter, so the set alone determines the state.
bool can_k_partition(const std::vector<int> &a, int k)
{
    const int total = std::accumulate(a.begin(), a.end(), 0);
    if (k <= 0 || total % k) return false;
    const int target = total / k;
    const unsigned full = (1u << a.size()) - 1;
    std::vector<int> dp(full + 1, -1);
    dp[0] = 0;
    for (unsigned m = 0; m <= full; ++m) {
        if (dp[m] < 0) continue;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (!(m >> i & 1u) && dp[m] + a[i] <= target)
                dp[m | 1u << i] = (dp[m] + a[i]) % target;
    }
    return dp[full] == 0;
}

bool k_partition_brute(const std::vector<int> &a, int k, std::size_t i, std::vector<int> &sums, int target)
{
    if (i == a.size())
        return std::all_of(sums.begin(), sums.end(), [target](int s) { return s == target; });
    for (int &s : sums) {
        if (s + a[i] > target) continue;
        s += a[i];
        const bool ok = k_partition_brute(a, k, i + 1, sums, target);
        s -= a[i];
        if (ok) return true;
    }
    return false;
}

int main()
{
    std::cout << "== TSP with reconstruction ==\n";
    const Matrix d{{0, 10, 15, 20}, {10, 0, 35, 25}, {15, 35, 0, 30}, {20, 25, 30, 0}};
    const Tour t = tsp(d);
    std::cout << "length " << t.length << ", tour:";
    for (int c : t.order) std::cout << ' ' << c;
    std::cout << " 0\n";
    CHECK(t.length == 80 && tour_length(d, t.order) == 80);

    std::mt19937 gen(2718);
    auto rnd = [&gen](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); };
    for (int it = 0; it < 50; ++it) {
        const std::size_t n = static_cast<std::size_t>(rnd(1, 8));
        Matrix m(n, std::vector<int>(n, 0));
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t j = 0; j < n; ++j) m[i][j] = i == j ? 0 : rnd(1, 40);
        const Tour r = tsp(m);
        CHECK(r.length == tsp_brute(m));
        CHECK(r.order.size() == n && tour_length(m, r.order) == r.length);
    }
    std::cout << "50 random instances: Held-Karp == next_permutation brute force; tours valid\n";

    std::cout << "\n== Min bins (submask enumeration) ==\n";
    const std::vector<int> items{4, 8, 1, 4, 2, 1};
    std::cout << "items {4,8,1,4,2,1}, capacity 10 -> " << min_bins(items, 10) << " bins  ({8,2},{4,4,1,1})\n";
    CHECK(min_bins(items, 10) == 2);
    for (int it = 0; it < 150; ++it) {
        std::vector<int> w(static_cast<std::size_t>(rnd(1, 7)));
        for (int &x : w) x = rnd(1, 10);
        std::vector<int> load(w.size(), 0);
        CHECK(min_bins(w, 10) == bins_brute(w, 10, 0, load));
    }
    std::cout << "150 random instances: bitmask DP == brute force\n";

    std::cout << "\n== Partition into k equal-sum subsets ==\n";
    const std::vector<int> p{4, 3, 2, 3, 5, 2, 1};
    std::cout << "{4,3,2,3,5,2,1} into 4 groups of 5? " << (can_k_partition(p, 4) ? "yes" : "no")
              << "   into 3? " << (can_k_partition(p, 3) ? "yes" : "no") << '\n';
    CHECK(can_k_partition(p, 4) && !can_k_partition(p, 3));
    for (int it = 0; it < 300; ++it) {
        std::vector<int> a(static_cast<std::size_t>(rnd(1, 9)));
        for (int &x : a) x = rnd(1, 6);
        const int k = rnd(1, 4), total = std::accumulate(a.begin(), a.end(), 0);
        std::vector<int> sums(static_cast<std::size_t>(k), 0);
        const bool bf = total % k == 0 && k_partition_brute(a, k, 0, sums, total / k);
        CHECK(can_k_partition(a, k) == bf);
    }
    std::cout << "300 random instances: bitmask DP == brute force\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
