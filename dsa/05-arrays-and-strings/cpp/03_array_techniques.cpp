/*
 * 03_array_techniques.cpp — the classic array techniques, the C++ way
 *
 * Concept : Same ideas as c/03, c/04, c/05, written as generic C++17 and next
 *           to the <algorithm>/<numeric> call that already does it:
 *             two pointers      -> 3-sum on a sorted vector
 *             sliding window    -> longest window with at most k distinct
 *             prefix sums       -> std::partial_sum; count subarrays summing to k
 *             Kadane            -> max subarray (template over any number type)
 *             reverse / rotate  -> std::reverse, std::rotate
 *             partition         -> std::partition (move zeroes)
 *           Every hand-written version is checked against brute force.
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/03_array_techniques.cpp
 * Read    : ../README.md  (section "Classic techniques")
 */
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

using Vec = std::vector<int>;
using Triple = std::tuple<int, int, int>;

// ---------------------------------------------------------------------------
// 3-sum: all unique triples summing to 0. Sort (O(n log n)), then for each
// i run the two-pointer pair search on the rest: O(n^2) total instead of
// O(n^3). Skipping equal neighbours removes duplicate triples.
//
//   sorted: [-4 -1 -1  0  1  2]
//             i   L           R     -4 + -1 + 2 = -3 < 0  -> L++
// ---------------------------------------------------------------------------
std::set<Triple> three_sum(Vec a)
{
    std::sort(a.begin(), a.end());
    std::set<Triple> out;
    const std::size_t n = a.size();
    for (std::size_t i = 0; i + 2 < n; ++i) {
        if (i > 0 && a[i] == a[i - 1])
            continue;
        std::size_t L = i + 1, R = n - 1;
        while (L < R) {
            int s = a[i] + a[L] + a[R];
            if (s == 0) {
                out.emplace(a[i], a[L], a[R]);
                ++L;
                --R;
            } else if (s < 0) {
                ++L;
            } else {
                --R;
            }
        }
    }
    return out;
}

std::set<Triple> three_sum_brute(Vec a)
{
    std::sort(a.begin(), a.end());
    std::set<Triple> out;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = i + 1; j < a.size(); ++j)
            for (std::size_t k = j + 1; k < a.size(); ++k)
                if (a[i] + a[j] + a[k] == 0)
                    out.emplace(a[i], a[j], a[k]);
    return out;
}

// ---------------------------------------------------------------------------
// Sliding window with a counter: longest subarray with at most k distinct
// values. O(n): each element enters once (R) and leaves once (L).
// ---------------------------------------------------------------------------
std::size_t longest_k_distinct(const Vec &a, std::size_t k)
{
    std::unordered_map<int, int> count; // value -> occurrences in window
    std::size_t best = 0, L = 0;
    for (std::size_t R = 0; R < a.size(); ++R) {
        ++count[a[R]];
        while (count.size() > k) {           // window invalid: shrink
            if (--count[a[L]] == 0)
                count.erase(a[L]);
            ++L;
        }
        best = std::max(best, R - L + 1);
    }
    return best;
}

std::size_t longest_k_distinct_brute(const Vec &a, std::size_t k)
{
    std::size_t best = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::set<int> seen;
        for (std::size_t j = i; j < a.size(); ++j) {
            seen.insert(a[j]);
            if (seen.size() > k)
                break;
            best = std::max(best, j - i + 1);
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Prefix sums + hash map: number of subarrays with sum exactly k.
// sum(l..r) = P[r+1] - P[l] = k  <=>  P[l] = P[r+1] - k.
// So while scanning, count how many earlier prefixes equal (current - k).
// O(n) time, O(n) space. Works with negative numbers (sliding window doesn't).
// ---------------------------------------------------------------------------
long count_subarrays_sum_k(const Vec &a, long k)
{
    std::unordered_map<long, long> seen{{0, 1}}; // empty prefix
    long prefix = 0, total = 0;
    for (int x : a) {
        prefix += x;
        auto it = seen.find(prefix - k);
        if (it != seen.end())
            total += it->second;
        ++seen[prefix];
    }
    return total;
}

long count_subarrays_brute(const Vec &a, long k)
{
    long total = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        long s = 0;
        for (std::size_t j = i; j < a.size(); ++j)
            if ((s += a[j]) == k)
                ++total;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Kadane as a template over any arithmetic type.
// ---------------------------------------------------------------------------
template <typename It>
auto kadane(It first, It last) -> typename std::iterator_traits<It>::value_type
{
    using T = typename std::iterator_traits<It>::value_type;
    T cur = *first, best = *first;
    for (++first; first != last; ++first) {
        cur = std::max(*first, cur + *first); // extend or restart
        best = std::max(best, cur);
    }
    return best;
}

int main()
{
    std::mt19937 rng(42); // fixed seed: reproducible
    auto rand_vec = [&](std::size_t n, int lo, int hi) {
        std::uniform_int_distribution<int> d(lo, hi);
        Vec v(n);
        for (int &x : v)
            x = d(rng);
        return v;
    };

    std::puts("== 1. 3-sum (sort + two pointers) ==");
    auto t = three_sum({-1, 0, 1, 2, -1, -4});
    CHECK(t.size() == 2 && t.count({-1, -1, 2}) && t.count({-1, 0, 1}));
    for (int round = 0; round < 200; ++round) {
        Vec v = rand_vec(rng() % 25, -6, 6);
        CHECK(three_sum(v) == three_sum_brute(v));
    }
    std::puts("  {-1,0,1,2,-1,-4} -> {-1,-1,2},{-1,0,1}; 200 random cases agree");

    std::puts("\n== 2. longest window with <= k distinct ==");
    CHECK(longest_k_distinct({1, 2, 1, 2, 3, 3, 3, 1}, 2) == 4);
    for (int round = 0; round < 300; ++round) {
        Vec v = rand_vec(rng() % 40, 0, 5);
        std::size_t k = 1 + rng() % 4;
        CHECK(longest_k_distinct(v, k) == longest_k_distinct_brute(v, k));
    }
    std::puts("  300 random cases agree with brute force");

    std::puts("\n== 3. prefix sums ==");
    Vec a{3, 1, 4, 1, 5};
    Vec P(a.size() + 1, 0);
    std::partial_sum(a.begin(), a.end(), P.begin() + 1); // P = 0 3 4 8 9 14
    CHECK((P == Vec{0, 3, 4, 8, 9, 14}));
    CHECK(P[4] - P[1] == 1 + 4 + 1);
    CHECK(count_subarrays_sum_k({1, 1, 1}, 2) == 2);
    CHECK(count_subarrays_sum_k({1, -1, 0}, 0) == 3);
    for (int round = 0; round < 300; ++round) {
        Vec v = rand_vec(rng() % 40, -5, 5);
        long k = static_cast<long>(rng() % 11) - 5;
        CHECK(count_subarrays_sum_k(v, k) == count_subarrays_brute(v, k));
    }
    std::puts("  std::partial_sum + hash map counts subarrays with sum k (O(n))");

    std::puts("\n== 4. Kadane (template) ==");
    Vec k1{-2, 1, -3, 4, -1, 2, 1, -5, 4};
    CHECK(kadane(k1.begin(), k1.end()) == 6);
    std::vector<double> kd{-1.5, 2.5, -0.5, 3.0};
    CHECK(kadane(kd.begin(), kd.end()) == 5.0);
    std::puts("  works for int and double: 6 and 5.0");

    std::puts("\n== 5. reverse / rotate / partition from <algorithm> ==");
    Vec r{1, 2, 3, 4, 5, 6, 7};
    std::rotate(r.begin(), r.begin() + 2, r.end()); // left by 2, O(n)
    CHECK((r == Vec{3, 4, 5, 6, 7, 1, 2}));
    std::reverse(r.begin(), r.end());
    CHECK((r == Vec{2, 1, 7, 6, 5, 4, 3}));
    // "Move zeroes to the end, keep order": stable_partition (or the
    // read/write two-pointer loop from c/03).
    Vec z{0, 1, 0, 3, 12};
    std::stable_partition(z.begin(), z.end(), [](int x) { return x != 0; });
    CHECK((z == Vec{1, 3, 12, 0, 0}));
    std::puts("  rotate, reverse, stable_partition: the library already has them");

    std::puts("\nall checks passed");
    return 0;
}
