/*
 * 02_knapsack_and_coins.cpp — the knapsack family in C++: 0/1, unbounded,
 *                             bounded (k copies), coin change min/ways
 *
 * Concept : One idea, five problems. A 1-D array indexed by capacity; the
 *           loop direction decides "each item once" (downward) vs "any number
 *           of times" (upward). Coin change is unbounded knapsack with a
 *           different combine operator (min of counts, or sum of ways), so
 *           one template parameterised by the combine step covers both.
 * Build   : make run P=dsa/17-dynamic-programming/cpp/02_knapsack_and_coins.cpp
 * Read    : ../README.md  (sections "0/1 and unbounded knapsack", "Coin change")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <optional>
#include <random>
#include <vector>

#define CHECK(cond) assert(cond)

struct Item {
    int weight;
    int value;
};

// 0/1 knapsack, O(W) space. Downward loop: dp[w - wt] is still the value
// WITHOUT this item, so each item is used at most once.
int knapsack01(const std::vector<Item> &items, int W)
{
    std::vector<int> dp(static_cast<std::size_t>(W) + 1, 0);
    for (const Item &it : items)
        for (int w = W; w >= it.weight; --w)
            dp[w] = std::max(dp[w], dp[w - it.weight] + it.value);
    return dp[W];
}

// Unbounded: upward loop, dp[w - wt] may already contain this item.
int knapsack_unbounded(const std::vector<Item> &items, int W)
{
    std::vector<int> dp(static_cast<std::size_t>(W) + 1, 0);
    for (const Item &it : items)
        for (int w = it.weight; w <= W; ++w)
            dp[w] = std::max(dp[w], dp[w - it.weight] + it.value);
    return dp[W];
}

// Bounded: item i may be used up to count[i] times. Binary splitting turns
// "up to c copies" into 0/1 items of size 1,2,4,...,rest: any number 0..c
// is a sum of a subset of those pieces. O(W * sum(log c)) instead of O(W*sum c).
//   c = 10  ->  pieces 1, 2, 4, 3   (1+2+4+3 = 10; every 0..10 reachable)
int knapsack_bounded(const std::vector<Item> &items, const std::vector<int> &count, int W)
{
    std::vector<Item> pieces;
    for (std::size_t i = 0; i < items.size(); ++i) {
        int left = count[i];
        for (int k = 1; left > 0; k *= 2) {
            const int take = std::min(k, left);
            pieces.push_back({items[i].weight * take, items[i].value * take});
            left -= take;
        }
    }
    return knapsack01(pieces, W);
}

// Coin change as a template over the "combine" rule.
//   Min coins : identity = INF, base = 0, combine(cur, prev) = min(cur, prev+1)
//   Num ways  : identity = 0,   base = 1, combine(cur, prev) = cur + prev
// Coins outer + amounts inner (upward) = unbounded, order-insensitive.
template <class T, class Combine>
std::vector<T> coin_dp(const std::vector<int> &coins, int amount, T identity, T base, Combine combine)
{
    std::vector<T> dp(static_cast<std::size_t>(amount) + 1, identity);
    dp[0] = base;
    for (int c : coins)
        for (int a = c; a <= amount; ++a) dp[a] = combine(dp[a], dp[a - c]);
    return dp;
}

std::optional<int> min_coins(const std::vector<int> &coins, int amount)
{
    constexpr int INF = INT_MAX / 2;
    auto dp = coin_dp<int>(coins, amount, INF, 0,
                           [](int cur, int prev) { return std::min(cur, prev + 1); });
    if (dp[amount] >= INF) return std::nullopt;   // optional says "impossible" clearly
    return dp[amount];
}

long long coin_ways(const std::vector<int> &coins, int amount)
{
    return coin_dp<long long>(coins, amount, 0, 1,
                              [](long long cur, long long prev) { return cur + prev; })[amount];
}

// ----------------------------------------------------------- brute force --
int brute_bounded(const std::vector<Item> &items, const std::vector<int> &count, std::size_t i, int W)
{
    if (i == items.size()) return 0;
    int best = 0;
    for (int k = 0; k <= count[i] && k * items[i].weight <= W; ++k)
        best = std::max(best, k * items[i].value +
                                  brute_bounded(items, count, i + 1, W - k * items[i].weight));
    return best;
}

long long brute_ways(const std::vector<int> &coins, std::size_t i, int amount, int used, int &best)
{
    if (amount == 0) { best = std::min(best, used); return 1; }
    if (i == coins.size()) return 0;
    long long total = 0;
    for (int k = 0; k * coins[i] <= amount; ++k)
        total += brute_ways(coins, i + 1, amount - k * coins[i], used + k, best);
    return total;
}

int main()
{
    std::cout << "== Knapsack family, items (wt,val) = (1,1) (3,4) (4,5) (5,7), W = 7 ==\n";
    const std::vector<Item> items{{1, 1}, {3, 4}, {4, 5}, {5, 7}};
    std::cout << "0/1       : " << knapsack01(items, 7) << '\n';
    std::cout << "unbounded : " << knapsack_unbounded(items, 7) << '\n';
    std::cout << "bounded {2,0,0,1} copies: " << knapsack_bounded(items, {2, 0, 0, 1}, 7)
              << "  (1+1+5 -> 1+1+7)\n";
    CHECK(knapsack01(items, 7) == 9);
    CHECK(knapsack_unbounded(items, 7) == 9);
    CHECK(knapsack_bounded(items, {2, 0, 0, 1}, 7) == 9);

    std::cout << "\n== Coin change ==\n";
    std::cout << "min coins {1,2,5} -> 11 : " << *min_coins({1, 2, 5}, 11) << '\n';
    std::cout << "min coins {2}     -> 3  : " << (min_coins({2}, 3) ? "possible" : "impossible") << '\n';
    std::cout << "ways      {1,2,5} -> 5  : " << coin_ways({1, 2, 5}, 5) << '\n';
    CHECK(min_coins({1, 2, 5}, 11) == 3);
    CHECK(!min_coins({2}, 3));
    CHECK(coin_ways({1, 2, 5}, 5) == 4);

    std::mt19937 gen(123);
    auto rnd = [&gen](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(gen); };
    for (int t = 0; t < 300; ++t) {
        const int n = rnd(1, 5), W = rnd(0, 30);
        std::vector<Item> its;
        std::vector<int> cnt, ones;
        for (int i = 0; i < n; ++i) {
            its.push_back({rnd(1, 8), rnd(0, 20)});
            cnt.push_back(rnd(0, 4));
            ones.push_back(1);
        }
        CHECK(knapsack_bounded(its, cnt, W) == brute_bounded(its, cnt, 0, W));
        CHECK(knapsack01(its, W) == brute_bounded(its, ones, 0, W));
        CHECK(knapsack_unbounded(its, W) == brute_bounded(its, std::vector<int>(its.size(), 40), 0, W));

        std::vector<int> coins(static_cast<std::size_t>(rnd(1, 4)));
        for (int &c : coins) c = rnd(1, 9);
        const int amount = rnd(0, 25);
        int best = INT_MAX;
        const long long ways = brute_ways(coins, 0, amount, 0, best);
        CHECK(coin_ways(coins, amount) == ways);
        const auto mc = min_coins(coins, amount);
        CHECK(mc ? *mc == best : best == INT_MAX);
    }
    std::cout << "300 random instances: all five DPs == brute force\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
