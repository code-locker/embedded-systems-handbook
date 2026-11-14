/*
 * 05_bitmask_search.cpp — bitmasks as state: N-Queens with three bitmasks,
 *                         and Travelling Salesman with bitmask DP, each
 *                         checked against brute force
 *
 * Concept : When a state is "which of n things are used/attacked", store it
 *           in one integer. Then "first free slot" = lowest set bit of the
 *           free mask, "mark used" = OR, and the whole state can be an array
 *           index (dp[mask][i]). This turns n! permutation searches into
 *           2^n * n^2 DP (TSP n=15: 15! = 1.3e12 vs 2^15*225 = 7.4e6).
 * Build   : make run P=dsa/18-bit-manipulation/cpp/05_bitmask_search.cpp
 * Read    : ../README.md  (section "3.9 Subset enumeration with bitmasks")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// N-Queens with bitmasks. Place queens row by row. Three masks of attacked
// columns for the CURRENT row:
//   cols : columns already taken
//   d1   : "\" diagonals — shift LEFT when moving down one row
//   d2   : "/" diagonals — shift RIGHT when moving down one row
//
//   row r:    . Q . .        cols = 0100
//   row r+1:  attacked by Q: column (same), col-1 and col+1 (diagonals)
//             d1<<1 = 1000, d2>>1 = 0010, cols = 0100 -> free = 0001
// free = ~(cols|d1|d2) & full ; take free bits one at a time with x & -x.
// ---------------------------------------------------------------------------
static long queens_bits(int n, std::uint32_t cols, std::uint32_t d1, std::uint32_t d2) {
    const std::uint32_t full = (1u << n) - 1u;
    if (cols == full) return 1;                          // all n rows placed
    long count = 0;
    std::uint32_t free = ~(cols | d1 | d2) & full;
    while (free) {
        std::uint32_t bit = free & (0u - free);          // lowest free column
        free ^= bit;                                     // remove it from free
        count += queens_bits(n, cols | bit, ((d1 | bit) << 1) & full, (d2 | bit) >> 1);
    }
    return count;
}

// Brute-force reference: try every permutation of columns (one queen per
// row and per column automatically) and check the diagonals. O(n! * n^2).
static long queens_brute(int n) {
    std::vector<int> col(static_cast<std::size_t>(n));
    std::iota(col.begin(), col.end(), 0);
    long count = 0;
    do {
        bool ok = true;
        for (int i = 0; i < n && ok; ++i)
            for (int j = i + 1; j < n && ok; ++j)
                if (std::abs(col[static_cast<std::size_t>(i)] - col[static_cast<std::size_t>(j)]) == j - i) ok = false;
        count += ok;
    } while (std::next_permutation(col.begin(), col.end()));
    return count;
}

// ---------------------------------------------------------------------------
// TSP (shortest Hamiltonian cycle starting and ending at city 0).
// dp[mask][i] = cheapest path that starts at 0, visits exactly the cities in
// mask, and ends at i (i in mask). Transition: extend to a city j not in mask.
//   dp[mask | 1<<j][j] = min(dp[mask][i] + d[i][j])
// Masks are processed in increasing numeric order, which is a valid
// topological order because mask | bit > mask.
// Memory: vector of 2^n * n ints, owned by the vector (RAII).
// ---------------------------------------------------------------------------
using Matrix = std::vector<std::vector<int>>;
constexpr int INF = std::numeric_limits<int>::max() / 2;

static int tsp_dp(const Matrix& d) {
    const int n = static_cast<int>(d.size());
    const std::size_t N = static_cast<std::size_t>(n);
    std::vector<std::vector<int>> dp(std::size_t{1} << N, std::vector<int>(N, INF));
    dp[1][0] = 0;                                        // only city 0 visited, standing at 0
    for (std::uint32_t mask = 1; mask < (1u << n); ++mask) {
        if (!(mask & 1u)) continue;                      // every path contains city 0
        for (int i = 0; i < n; ++i) {
            int cur = dp[mask][static_cast<std::size_t>(i)];
            if (cur >= INF) continue;
            std::uint32_t todo = ~mask & ((1u << n) - 1u);
            while (todo) {                               // iterate unvisited cities via lowest bit
                int j = __builtin_ctz(todo);
                todo &= todo - 1u;
                auto& nxt = dp[mask | (1u << j)][static_cast<std::size_t>(j)];
                nxt = std::min(nxt, cur + d[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]);
            }
        }
    }
    int best = INF;
    std::uint32_t all = (1u << n) - 1u;
    for (int i = 1; i < n; ++i)
        best = std::min(best, dp[all][static_cast<std::size_t>(i)] + d[static_cast<std::size_t>(i)][0]);
    return best;
}

static int tsp_brute(const Matrix& d) {
    const int n = static_cast<int>(d.size());
    std::vector<int> perm;
    for (int i = 1; i < n; ++i) perm.push_back(i);
    int best = INF;
    do {
        int cost = d[0][static_cast<std::size_t>(perm[0])];
        for (std::size_t k = 0; k + 1 < perm.size(); ++k)
            cost += d[static_cast<std::size_t>(perm[k])][static_cast<std::size_t>(perm[k + 1])];
        cost += d[static_cast<std::size_t>(perm.back())][0];
        best = std::min(best, cost);
    } while (std::next_permutation(perm.begin(), perm.end()));
    return best;
}

int main() {
    std::cout << "== 1. N-Queens: bitmask backtracking vs permutation brute force ==\n";
    const long known[] = { 0, 1, 0, 0, 2, 10, 4, 40, 92, 352, 724, 2680, 14200 };
    for (int n = 1; n <= 12; ++n) {
        long c = queens_bits(n, 0, 0, 0);
        assert(c == known[n]);
        if (n <= 8) assert(c == queens_brute(n));
        std::cout << "  n=" << n << " solutions=" << c << "\n";
    }
    std::cout << "bitmask == brute force (n<=8) == known OEIS A000170 (n<=12): OK\n";

    std::cout << "\n== 2. TSP: bitmask DP vs brute-force permutations ==\n";
    std::mt19937 rng(11);
    for (int trial = 0; trial < 30; ++trial) {
        int n = 2 + trial % 8;                           // 2..9 cities
        Matrix d(static_cast<std::size_t>(n), std::vector<int>(static_cast<std::size_t>(n), 0));
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
                if (i != j) d[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = 1 + static_cast<int>(rng() % 100);
        assert(tsp_dp(d) == tsp_brute(d));
    }
    std::cout << "30 random asymmetric instances (2..9 cities): DP == brute force: OK\n";

    Matrix big(15, std::vector<int>(15, 0));
    for (std::size_t i = 0; i < 15; ++i)
        for (std::size_t j = 0; j < 15; ++j)
            big[i][j] = (i == j) ? 0 : static_cast<int>((i > j ? i - j : j - i));   // cities on a line
    int best = tsp_dp(big);
    std::cout << "15 cities on a line: optimal tour = " << best << " (go out and back: 2*14)\n";
    assert(best == 28);

    std::cout << "\nall checks passed\n";
    return 0;
}
