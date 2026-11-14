/*
 * 07_pattern_catalogue.cpp — one small, checked example per DP pattern not
 *                            covered elsewhere: prefix "best ending here"
 *                            (Kadane), segmentation (word break), counting
 *                            structures (Catalan / unique BSTs), state
 *                            machine (stock with cooldown), digit DP
 *
 * Concept : Most DPs are one of a handful of shapes. Recognising the shape
 *           tells you the state immediately. Each function below names its
 *           pattern, its state and its transition.
 * Build   : make run P=dsa/17-dynamic-programming/cpp/07_pattern_catalogue.cpp
 * Read    : ../README.md  (section "Pattern catalogue")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#define CHECK(cond) assert(cond)

// ------------------------------------------------- 1. "ending at i" (Kadane)
// best_here = best sum of a subarray that ENDS at i
//           = max(a[i], best_here + a[i])   (start fresh, or extend)
// The answer is the max over i. O(n) time, O(1) space.
long long kadane(const std::vector<int> &a)
{
    long long best = LLONG_MIN, here = 0;
    for (int x : a) {
        here = std::max<long long>(x, here + x);
        best = std::max(best, here);
    }
    return best;
}

// ------------------------------------------- 2. segmentation (word break)
// ok[i] = the prefix s[0..i) can be split into dictionary words
// ok[i] = OR over j < i of ok[j] && s[j..i) in dict.  O(n^2 * L) with hashing.
bool word_break(const std::string &s, const std::unordered_set<std::string> &dict)
{
    std::vector<char> ok(s.size() + 1, 0);
    ok[0] = 1;                                        // empty prefix
    for (std::size_t i = 1; i <= s.size(); ++i)
        for (std::size_t j = 0; j < i && !ok[i]; ++j)
            ok[i] = ok[j] && dict.count(s.substr(j, i - j));
    return ok[s.size()];
}

bool word_break_brute(const std::string &s, const std::unordered_set<std::string> &dict)
{
    if (s.empty()) return true;
    for (std::size_t len = 1; len <= s.size(); ++len)
        if (dict.count(s.substr(0, len)) && word_break_brute(s.substr(len), dict)) return true;
    return false;
}

// ------------------------------------- 3. counting structures (Catalan)
// Number of BST shapes on n keys: pick root r, left has r-1 keys, right n-r.
//   T(n) = sum over r of T(r-1) * T(n-r),  T(0) = 1   -> Catalan numbers.
// Same recurrence counts valid parenthesisations, MCM bracketings,
// triangulations of a polygon...
unsigned long long num_bsts(int n)
{
    std::vector<unsigned long long> T(static_cast<std::size_t>(n) + 1, 0);
    T[0] = 1;
    for (int k = 1; k <= n; ++k)
        for (int r = 1; r <= k; ++r) T[k] += T[r - 1] * T[k - r];
    return T[n];
}

// Brute: count balanced bracket strings of length 2n by trying every string.
unsigned long long balanced_brute(int n)
{
    unsigned long long count = 0;
    for (unsigned mask = 0; mask < (1u << (2 * n)); ++mask) {
        int depth = 0;
        bool ok = true;
        for (int i = 0; i < 2 * n && ok; ++i) ok = (depth += (mask >> i & 1u) ? 1 : -1) >= 0;
        if (ok && depth == 0) ++count;
    }
    return count;
}

// ------------------------------------- 4. state machine (stock + cooldown)
// Each day you are in one of three states; the DP is just "best money in
// each state", updated once per day:
//
//            buy               sell
//   [rest] --------> [hold] --------> [cool]
//     ^  \__/          \__/             |
//     |   wait         wait             |  (must wait one day)
//     +---------------------------------+
//
//   hold' = max(hold, rest - price)
//   cool' = hold + price
//   rest' = max(rest, cool)
int stock_cooldown(const std::vector<int> &p)
{
    int hold = INT_MIN / 2, cool = INT_MIN / 2, rest = 0;
    for (int price : p) {
        const int h = std::max(hold, rest - price), c = hold + price, r = std::max(rest, cool);
        hold = h; cool = c; rest = r;                // update all three from OLD values
    }
    return std::max(rest, cool);
}

int stock_brute(const std::vector<int> &p, std::size_t day, bool holding, int bought_at)
{
    if (day >= p.size()) return 0;
    int best = stock_brute(p, day + 1, holding, bought_at);                       // do nothing
    if (holding) best = std::max(best, p[day] - bought_at + stock_brute(p, day + 2, false, 0));
    else best = std::max(best, stock_brute(p, day + 1, true, p[day]));
    return best;
}

// ------------------------------------------------------- 5. digit DP
// How many x in [0, N] have digit sum divisible by k?
// Walk the digits of N from the most significant. State:
//   (position, digit-sum mod k, tight = "prefix equals N's prefix so far")
// While tight, the next digit may go only up to N's digit; once below, any
// digit 0..9 is allowed. memo over (pos, mod) for the non-tight case.
long long digit_sum_divisible(long long N, int k)
{
    const std::string s = std::to_string(N);
    const std::size_t L = s.size();
    // ways[pos][mod] = completions of the remaining L-pos free digits that bring
    // the running sum from `mod` to 0 (mod k). Filled from the back.
    std::vector<std::vector<long long>> ways(L + 1, std::vector<long long>(static_cast<std::size_t>(k), 0));
    ways[L][0] = 1;
    for (std::size_t pos = L; pos-- > 0;)
        for (int m = 0; m < k; ++m)
            for (int d = 0; d <= 9; ++d) ways[pos][m] += ways[pos + 1][(m + d) % k];
    long long total = 0;
    int mod = 0;                                   // digit sum of the tight prefix
    for (std::size_t pos = 0; pos < L; ++pos) {
        const int limit = s[pos] - '0';
        for (int d = 0; d < limit; ++d)            // go strictly below N here: free afterwards
            total += ways[pos + 1][(mod + d) % k];
        mod = (mod + limit) % k;                   // stay tight
    }
    if (mod == 0) ++total;                         // N itself
    return total;
}

long long digit_brute(long long N, int k)
{
    long long c = 0;
    for (long long x = 0; x <= N; ++x) {
        int sum = 0;
        for (long long y = x; y; y /= 10) sum += static_cast<int>(y % 10);
        if (sum % k == 0) ++c;
    }
    return c;
}

int main()
{
    std::mt19937 gen(7);
    auto rnd = [&gen](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); };

    std::cout << "1. Kadane: max subarray of {-2,1,-3,4,-1,2,1,-5,4} = "
              << kadane({-2, 1, -3, 4, -1, 2, 1, -5, 4}) << "  (4 -1 2 1)\n";
    CHECK(kadane({-2, 1, -3, 4, -1, 2, 1, -5, 4}) == 6 && kadane({-3, -1, -2}) == -1);
    for (int t = 0; t < 300; ++t) {
        std::vector<int> a(static_cast<std::size_t>(rnd(1, 20)));
        for (int &x : a) x = rnd(-10, 10);
        long long bf = LLONG_MIN;
        for (std::size_t i = 0; i < a.size(); ++i) {
            long long s = 0;
            for (std::size_t j = i; j < a.size(); ++j) bf = std::max(bf, s += a[j]);
        }
        CHECK(kadane(a) == bf);
    }

    const std::unordered_set<std::string> dict{"gst", "pipe", "line", "pipeline", "src", "sink"};
    std::cout << std::boolalpha << "2. word break: \"gstpipelinesink\" -> " << word_break("gstpipelinesink", dict)
              << ", \"gstpipelin\" -> " << word_break("gstpipelin", dict) << '\n';
    CHECK(word_break("gstpipelinesink", dict) && !word_break("gstpipelin", dict));
    const std::unordered_set<std::string> ab{"a", "ab", "abb", "ba"};
    for (int t = 0; t < 300; ++t) {
        std::string s(static_cast<std::size_t>(rnd(0, 12)), 'a');
        for (char &c : s) c = rnd(0, 1) ? 'a' : 'b';
        CHECK(word_break(s, ab) == word_break_brute(s, ab));
    }

    std::cout << "3. Catalan: BST shapes on 1..10 keys:";
    for (int n = 1; n <= 10; ++n) std::cout << ' ' << num_bsts(n);
    std::cout << '\n';
    for (int n = 0; n <= 9; ++n) CHECK(num_bsts(n) == balanced_brute(n));

    std::cout << "4. stock with cooldown {1,2,3,0,2}: profit " << stock_cooldown({1, 2, 3, 0, 2})
              << "  (buy 1, sell 2, cool, buy 0, sell 2)\n";
    CHECK(stock_cooldown({1, 2, 3, 0, 2}) == 3);
    for (int t = 0; t < 300; ++t) {
        std::vector<int> p(static_cast<std::size_t>(rnd(0, 12)));
        for (int &x : p) x = rnd(0, 20);
        CHECK(stock_cooldown(p) == stock_brute(p, 0, false, 0));
    }

    std::cout << "5. digit DP: numbers in [0, 10^18] with digit sum divisible by 7 = "
              << digit_sum_divisible(1000000000000000000LL, 7) << '\n';
    for (int t = 0; t < 100; ++t) {
        const long long N = rnd(0, 20000);
        const int k = rnd(1, 12);
        CHECK(digit_sum_divisible(N, k) == digit_brute(N, k));
    }
    CHECK(digit_sum_divisible(99, 1) == 100);

    std::cout << "\nevery pattern checked against brute force on random inputs\n";
    std::cout << "all checks passed\n";
    return 0;
}
