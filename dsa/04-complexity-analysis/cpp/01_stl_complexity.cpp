/*
 * 01_stl_complexity.cpp — the STL's complexity guarantees, verified by counting comparisons
 *
 * Concept : The C++ standard specifies complexity for every algorithm and
 *           container operation (e.g. std::sort: O(n log n) comparisons,
 *           std::lower_bound: at most log2(n) + O(1)). A comparator that
 *           COUNTS its calls lets you check those promises yourself, and shows
 *           what each container really costs.
 * Build   : make run P=dsa/04-complexity-analysis/cpp/01_stl_complexity.cpp
 * Read    : ../README.md  (section "5. C vs C++" and the complexity table)
 */
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <unordered_map>
#include <vector>

static long g_cmp = 0;
struct CountingLess {
    bool operator()(int a, int b) const { ++g_cmp; return a < b; }
};

template <typename F>
static long count(F &&f)
{
    g_cmp = 0;
    f();
    return g_cmp;
}

int main()
{
    std::mt19937 rng(12345);
    std::printf("%-34s %8s %12s %12s\n", "operation", "n", "comparisons", "reference");
    for (int n : {1 << 10, 1 << 14, 1 << 17}) {
        std::vector<int> v(n);
        std::iota(v.begin(), v.end(), 0);
        std::shuffle(v.begin(), v.end(), rng);
        double nlogn = n * std::log2(n);

        long c_sort = count([&] { std::sort(v.begin(), v.end(), CountingLess{}); });
        std::printf("%-34s %8d %12ld %12.0f  (n log2 n)\n", "std::sort", n, c_sort, nlogn);
        assert(c_sort < 3 * nlogn);

        long c_lb = count([&] { (void)std::lower_bound(v.begin(), v.end(), n / 3, CountingLess{}); });
        std::printf("%-34s %8d %12ld %12.0f  (log2 n)\n", "std::lower_bound (binary search)", n, c_lb, std::log2(n));
        assert(c_lb <= std::log2(n) + 2);

        long c_find = count([&] {
            auto it = std::find_if(v.begin(), v.end(), [&](int x) { ++g_cmp; return x == n - 1; });
            assert(it != v.end());
        });
        std::printf("%-34s %8d %12ld %12d  (n)\n", "std::find (linear, worst case)", n, c_find, n);
        assert(c_find == n);

        std::set<int, CountingLess> s;
        long c_set = count([&] { for (int i = 0; i < n; i++) s.insert(static_cast<int>(rng() % (4 * n))); });
        std::printf("%-34s %8d %12ld %12.0f  (n log2 n)\n", "std::set insert x n (red-black)", n, c_set, nlogn);
        assert(c_set < 3 * nlogn);
        std::puts("");
    }

    // Containers: what "insert at front" costs (moves, not comparisons).
    const int n = 20000;
    std::vector<int> vec;
    std::deque<int> dq;
    std::list<int> lst;
    long vec_moves = 0;
    for (int i = 0; i < n; i++) {
        vec_moves += static_cast<long>(vec.size());  // every existing element shifts right: O(n)
        vec.insert(vec.begin(), i);
        dq.push_front(i);                            // O(1)
        lst.push_front(i);                           // O(1)
    }
    std::printf("insert at front x %d: vector shifted %ld elements (~n^2/2), deque/list: 0\n", n, vec_moves);
    assert(vec_moves == static_cast<long>(n) * (n - 1) / 2);
    assert(vec.front() == dq.front() && dq.front() == lst.front());

    // Hash map: O(1) average lookups. Count probes with a counting hasher.
    static long g_hash = 0;
    struct CountingHash { std::size_t operator()(int x) const { ++g_hash; return std::hash<int>{}(x); } };
    std::unordered_map<int, int, CountingHash> um;
    std::map<int, int, CountingLess> om;
    for (int i = 0; i < n; i++) { um[i] = i; om[i] = i; }
    g_hash = 0;
    g_cmp = 0;
    long sum = 0;
    for (int i = 0; i < n; i++) sum += um.at(i) + om.at(i);
    std::printf("%d lookups: unordered_map %ld hash calls (1 each, O(1) avg), map %ld comparisons (~log2 n each = %.1f)\n",
                n, g_hash, g_cmp, static_cast<double>(g_cmp) / n);
    assert(g_hash == n && g_cmp < n * (std::log2(n) + 3) && sum > 0);
    std::printf("All checks passed.\n");
    return 0;
}
