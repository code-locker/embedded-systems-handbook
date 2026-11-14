/*
 * 06_greedy_vs_dp.cpp — counter-examples: where the obvious greedy is WRONG
 *
 * Concept : A greedy is only correct with a proof. Three tempting greedies that
 *           fail, each found automatically by comparing with an exact method:
 *             1. coin change, largest coin first      ({1,3,4}, amount 6)
 *             2. weighted interval scheduling, earliest finish (ignores weights)
 *             3. 0/1 knapsack, best value/weight first
 *           The fix in every case is dynamic programming (lesson 17).
 * Build   : make run P=dsa/16-greedy/cpp/06_greedy_vs_dp.cpp
 * Read    : ../README.md  (section "When greedy fails")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <random>
#include <vector>

// ---------------- 1. coin change ----------------
static int greedy_coins(std::vector<int> coins, int amount)
{
    std::sort(coins.rbegin(), coins.rend());  // largest first
    int n = 0;
    for (int c : coins) { n += amount / c; amount %= c; }
    return amount == 0 ? n : INT_MAX;
}

static int dp_coins(const std::vector<int>& coins, int amount)
{
    std::vector<int> dp(static_cast<std::size_t>(amount) + 1, INT_MAX);
    dp[0] = 0;
    for (int a = 1; a <= amount; ++a)
        for (int c : coins)
            if (c <= a && dp[static_cast<std::size_t>(a - c)] != INT_MAX)
                dp[static_cast<std::size_t>(a)] = std::min(dp[static_cast<std::size_t>(a)], dp[static_cast<std::size_t>(a - c)] + 1);
    return dp[static_cast<std::size_t>(amount)];
}

// ---------------- 2. weighted interval scheduling ----------------
struct Job {
    int start, end, weight;  // [start, end)
};

static long greedy_weighted(std::vector<Job> v)
{
    std::sort(v.begin(), v.end(), [](const Job& a, const Job& b) { return a.end < b.end; });
    long total = 0;
    int last = INT_MIN;
    for (const auto& j : v)
        if (j.start >= last) { total += j.weight; last = j.end; }
    return total;
}

/* DP: sort by end; best[i] = max(best[i-1], w_i + best[p(i)]) where p(i) is the
 * number of jobs ending <= start_i (found by binary search). O(n log n). */
static long dp_weighted(std::vector<Job> v)
{
    std::sort(v.begin(), v.end(), [](const Job& a, const Job& b) { return a.end < b.end; });
    std::vector<int> ends;
    for (const auto& j : v) ends.push_back(j.end);
    std::vector<long> best(v.size() + 1, 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto p = static_cast<std::size_t>(std::upper_bound(ends.begin(), ends.begin() + static_cast<long>(i), v[i].start) - ends.begin());
        best[i + 1] = std::max(best[i], v[i].weight + best[p]);
    }
    return best.back();
}

// ---------------- 3. 0/1 knapsack ----------------
struct Item {
    int value, weight;
};

static int greedy_01(std::vector<Item> v, int cap)
{
    std::sort(v.begin(), v.end(), [](const Item& a, const Item& b) { return a.value * b.weight > b.value * a.weight; });
    int total = 0;
    for (const auto& it : v)
        if (it.weight <= cap) { total += it.value; cap -= it.weight; }
    return total;
}

static int dp_01(const std::vector<Item>& v, int cap)
{
    std::vector<int> dp(static_cast<std::size_t>(cap) + 1, 0);
    for (const auto& it : v)
        for (int c = cap; c >= it.weight; --c)  // backwards: each item used at most once
            dp[static_cast<std::size_t>(c)] = std::max(dp[static_cast<std::size_t>(c)], dp[static_cast<std::size_t>(c - it.weight)] + it.value);
    return dp[static_cast<std::size_t>(cap)];
}

int main()
{
    // 1.
    const std::vector<int> odd{1, 3, 4};
    std::cout << "coins {1,3,4}, amount 6: greedy " << greedy_coins(odd, 6) << " coins (4+1+1), optimal "
              << dp_coins(odd, 6) << " (3+3)\n";
    assert(greedy_coins(odd, 6) == 3 && dp_coins(odd, 6) == 2);

    // 2. A heavy long job vs two light short ones.
    //    [0====================10) w=100
    //    [0==3) w=1   [4==7) w=1
    const std::vector<Job> jobs{{0, 10, 100}, {0, 3, 1}, {4, 7, 1}};
    std::cout << "weighted intervals: greedy " << greedy_weighted(jobs) << ", optimal " << dp_weighted(jobs) << "\n";
    assert(greedy_weighted(jobs) == 2 && dp_weighted(jobs) == 100);

    // 3. The ratio greedy grabs the small dense item and leaves capacity unused.
    const std::vector<Item> items{{10, 1}, {90, 10}};  // cap 10
    std::cout << "0/1 knapsack cap 10: greedy " << greedy_01(items, 10) << ", optimal " << dp_01(items, 10) << "\n";
    assert(greedy_01(items, 10) == 10 && dp_01(items, 10) == 90);

    // How often does each greedy fail on random input? Greedy never BEATS the
    // exact answer (it is always a feasible solution), it just falls short.
    std::mt19937 rng(1616);
    int coin_fail = 0, wis_fail = 0, ks_fail = 0;
    const int trials = 2000;
    for (int t = 0; t < trials; ++t) {
        std::vector<int> coins{1};
        for (int k = 0; k < 3; ++k) coins.push_back(std::uniform_int_distribution<int>(2, 15)(rng));
        const int amt = std::uniform_int_distribution<int>(1, 60)(rng);
        assert(greedy_coins(coins, amt) >= dp_coins(coins, amt));
        coin_fail += greedy_coins(coins, amt) != dp_coins(coins, amt);

        std::vector<Job> js(8);
        for (auto& j : js) {
            j.start = std::uniform_int_distribution<int>(0, 20)(rng);
            j.end = j.start + std::uniform_int_distribution<int>(1, 8)(rng);
            j.weight = std::uniform_int_distribution<int>(1, 20)(rng);
        }
        assert(greedy_weighted(js) <= dp_weighted(js));
        wis_fail += greedy_weighted(js) != dp_weighted(js);

        std::vector<Item> its(6);
        for (auto& it : its) {
            it.value = std::uniform_int_distribution<int>(1, 50)(rng);
            it.weight = std::uniform_int_distribution<int>(1, 20)(rng);
        }
        const int cap = std::uniform_int_distribution<int>(5, 50)(rng);
        assert(greedy_01(its, cap) <= dp_01(its, cap));
        ks_fail += greedy_01(its, cap) != dp_01(its, cap);
    }
    std::cout << "random failure rate over " << trials << " trials: coins " << coin_fail << ", weighted intervals "
              << wis_fail << ", 0/1 knapsack " << ks_fail << "\n";
    assert(coin_fail > 0 && wis_fail > 0 && ks_fail > 0);
    return 0;
}
