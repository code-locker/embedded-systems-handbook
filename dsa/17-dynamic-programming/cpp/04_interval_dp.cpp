/*
 * 04_interval_dp.cpp — interval DP in C++: matrix chain (top-down memo),
 *                      longest palindromic subsequence, minimum palindrome
 *                      cuts, burst balloons
 *
 * Concept : State = a range [i, j]. Answer for a range = best over a split
 *           point k (or over shrinking the ends). Fill by increasing length,
 *           or let memoised recursion find the order for you. Burst balloons
 *           shows the key trick: choose the element handled LAST in the
 *           range, so the two sides become independent.
 * Build   : make run P=dsa/17-dynamic-programming/cpp/04_interval_dp.cpp
 * Read    : ../README.md  (sections "Matrix chain multiplication",
 *           "Longest palindromic subsequence", "Pattern catalogue")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#define CHECK(cond) assert(cond)

using Table = std::vector<std::vector<long long>>;

// Matrix chain, TOP-DOWN. -1 marks "not computed". The recursion naturally
// visits shorter ranges first, so no fill order has to be worked out.
long long mcm_memo(const std::vector<int> &dims)
{
    const std::size_t n = dims.size() - 1;               // number of matrices
    Table memo(n + 1, std::vector<long long>(n + 1, -1));
    std::function<long long(std::size_t, std::size_t)> go = [&](std::size_t i, std::size_t j) {
        if (i == j) return 0LL;
        long long &m = memo[i][j];                        // reference into the table
        if (m != -1) return m;
        m = LLONG_MAX;
        for (std::size_t k = i; k < j; ++k)
            m = std::min(m, go(i, k) + go(k + 1, j) + 1LL * dims[i - 1] * dims[k] * dims[j]);
        return m;
    };
    return go(1, n);
}

long long mcm_brute(const std::vector<int> &dims, std::size_t i, std::size_t j)
{
    if (i == j) return 0;
    long long best = LLONG_MAX;
    for (std::size_t k = i; k < j; ++k)
        best = std::min(best, mcm_brute(dims, i, k) + mcm_brute(dims, k + 1, j) +
                                  1LL * dims[i - 1] * dims[k] * dims[j]);
    return best;
}

// LPS bottom-up by length, rows of the table kept as std::vector<int>.
int lps(const std::string &s)
{
    const std::size_t n = s.size();
    if (n == 0) return 0;
    std::vector<std::vector<int>> dp(n, std::vector<int>(n, 0));
    for (std::size_t i = 0; i < n; ++i) dp[i][i] = 1;
    for (std::size_t len = 2; len <= n; ++len)
        for (std::size_t i = 0, j = len - 1; j < n; ++i, ++j)
            dp[i][j] = s[i] == s[j] ? 2 + (len == 2 ? 0 : dp[i + 1][j - 1])
                                    : std::max(dp[i + 1][j], dp[i][j - 1]);
    return dp[0][n - 1];
}

// Minimum cuts so every piece is a palindrome. "aab" -> "aa|b" = 1 cut.
// Two DPs stacked: pal[i][j] (interval DP) then cuts[j] (1-D DP over ends).
//   cuts[j] = min cuts for s[0..j] = 0 if s[0..j] is a palindrome,
//             else min over i of cuts[i-1] + 1 where s[i..j] is a palindrome
// O(n^2) time and space.
int min_pal_cuts(const std::string &s)
{
    const std::size_t n = s.size();
    if (n == 0) return 0;
    std::vector<std::vector<char>> pal(n, std::vector<char>(n, 0));
    std::vector<int> cuts(n);
    for (std::size_t j = 0; j < n; ++j) {
        cuts[j] = static_cast<int>(j);                         // worst: cut every char
        for (std::size_t i = 0; i <= j; ++i)
            if (s[i] == s[j] && (j - i < 2 || pal[i + 1][j - 1])) {
                pal[i][j] = 1;
                cuts[j] = i == 0 ? 0 : std::min(cuts[j], cuts[i - 1] + 1);
            }
    }
    return cuts[n - 1];
}

int pal_cuts_brute(const std::string &s)                     // try every cut set
{
    const std::size_t n = s.size();
    if (n == 0) return 0;
    int best = INT_MAX;
    for (unsigned mask = 0; mask < (1u << (n - 1)); ++mask) {   // bit k: cut after k
        bool ok = true;
        std::size_t start = 0;
        for (std::size_t k = 0; k < n && ok; ++k)
            if (k == n - 1 || (mask >> k & 1u)) {
                const std::string piece = s.substr(start, k - start + 1);
                ok = std::equal(piece.begin(), piece.end(), piece.rbegin());
                start = k + 1;
            }
        if (ok) best = std::min(best, __builtin_popcount(mask));
    }
    return best;
}

// Burst balloons: bursting i earns left*a[i]*right where left/right are the
// CURRENT neighbours. Choosing what to burst FIRST couples the two sides
// (neighbours change). Choosing what to burst LAST in (l, r) does not: while
// k is still alive, (l..k) and (k..r) never see each other.
//
//   padded: 1 [3 1 5 8] 1      dp[l][r] = best for balloons strictly inside
//   dp[l][r] = max over l<k<r of dp[l][k] + dp[k][r] + v[l]*v[k]*v[r]
int burst_balloons(const std::vector<int> &a)
{
    std::vector<int> v{1};
    v.insert(v.end(), a.begin(), a.end());
    v.push_back(1);
    const std::size_t m = v.size();
    std::vector<std::vector<int>> dp(m, std::vector<int>(m, 0));
    for (std::size_t len = 2; len < m; ++len)
        for (std::size_t l = 0; l + len < m; ++l) {
            const std::size_t r = l + len;
            for (std::size_t k = l + 1; k < r; ++k)
                dp[l][r] = std::max(dp[l][r], dp[l][k] + dp[k][r] + v[l] * v[k] * v[r]);
        }
    return dp[0][m - 1];
}

int burst_brute(std::vector<int> a)                           // try every order
{
    if (a.empty()) return 0;
    int best = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const int left = i ? a[i - 1] : 1, right = i + 1 < a.size() ? a[i + 1] : 1;
        std::vector<int> rest(a);
        rest.erase(rest.begin() + static_cast<long>(i));
        best = std::max(best, left * a[i] * right + burst_brute(rest));
    }
    return best;
}

int lps_brute(const std::string &s)
{
    int best = 0;
    for (unsigned mask = 0; mask < (1u << s.size()); ++mask) {
        std::string t;
        for (std::size_t i = 0; i < s.size(); ++i)
            if (mask >> i & 1u) t += s[i];
        if (std::equal(t.begin(), t.end(), t.rbegin())) best = std::max(best, static_cast<int>(t.size()));
    }
    return best;
}

int main()
{
    std::cout << "== Matrix chain (top-down) ==\n";
    const std::vector<int> clrs{30, 35, 15, 5, 10, 20, 25};
    std::cout << "CLRS chain: " << mcm_memo(clrs) << " scalar multiplications\n";
    CHECK(mcm_memo(clrs) == 15125);

    std::cout << "\n== Palindromes ==\n";
    std::cout << "LPS(\"bbbab\") = " << lps("bbbab") << ", min cuts(\"aab\") = " << min_pal_cuts("aab")
              << ", min cuts(\"banana\") = " << min_pal_cuts("banana") << "  (b|anana)\n";
    CHECK(lps("bbbab") == 4 && min_pal_cuts("aab") == 1 && min_pal_cuts("banana") == 1);

    std::cout << "\n== Burst balloons [3,1,5,8] ==\n";
    std::cout << "max coins = " << burst_balloons({3, 1, 5, 8}) << "  (burst 1,5,3,8: 15+120+24+8)\n";
    CHECK(burst_balloons({3, 1, 5, 8}) == 167);

    std::mt19937 gen(99);
    auto rnd = [&gen](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); };
    for (int t = 0; t < 200; ++t) {
        std::vector<int> dims(static_cast<std::size_t>(rnd(2, 8)));
        for (int &d : dims) d = rnd(1, 15);
        CHECK(mcm_memo(dims) == mcm_brute(dims, 1, dims.size() - 1));

        std::string s(static_cast<std::size_t>(rnd(0, 11)), 'a');
        for (char &c : s) c = static_cast<char>('a' + rnd(0, 2));
        CHECK(lps(s) == lps_brute(s));
        CHECK(min_pal_cuts(s) == pal_cuts_brute(s));

        std::vector<int> b(static_cast<std::size_t>(rnd(0, 6)));
        for (int &x : b) x = rnd(0, 9);
        CHECK(burst_balloons(b) == burst_brute(b));
    }
    std::cout << "\n200 random cases: MCM, LPS, palindrome cuts, burst balloons == brute force\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
