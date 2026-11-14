/*
 * 02_fractional_knapsack.cpp — fractional knapsack, returning WHAT was taken
 *
 * Concept : Sort by value density (value/weight), take whole items while they
 *           fit, then a fraction of the next. Returns the plan (item, fraction)
 *           not just the number, because real schedulers need the plan.
 *           Reference: an exact check that tries every item as the "split" item
 *           over every subset of whole items (exponential, small n).
 * Build   : make run P=dsa/16-greedy/cpp/02_fractional_knapsack.cpp
 * Read    : ../README.md  (section "Fractional knapsack")
 */
#include <algorithm>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

struct Item {
    double value;
    double weight;  // > 0
};

struct Take {
    std::size_t index;  // into the caller's vector
    double fraction;    // 0 < fraction <= 1
};

// O(n log n). We sort INDICES, not items, so the plan refers to the caller's order.
static std::vector<Take> fractional_knapsack(const std::vector<Item>& items, double capacity)
{
    std::vector<std::size_t> idx(items.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) {
        // a before b  <=>  v_a/w_a > v_b/w_b  <=>  v_a*w_b > v_b*w_a  (weights > 0)
        return items[a].value * items[b].weight > items[b].value * items[a].weight;
    });
    std::vector<Take> plan;
    for (std::size_t i : idx) {
        if (capacity <= 0) break;
        const double f = std::min(1.0, capacity / items[i].weight);
        plan.push_back({i, f});
        capacity -= f * items[i].weight;
    }
    return plan;
}

static double plan_value(const std::vector<Item>& items, const std::vector<Take>& plan)
{
    double v = 0;
    for (const auto& t : plan) v += items[t.index].value * t.fraction;
    return v;
}

/*
 * Exact reference. Fact (LP theory): some optimal solution takes every item
 * either fully, not at all, or — for at most ONE item — partially. So try every
 * subset S of whole items that fits, and every item j outside S as the partial
 * one filling the leftover. O(2^n * n^2).
 */
static double exact(const std::vector<Item>& items, double capacity)
{
    const std::size_t n = items.size();
    double best = 0;
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        double w = 0, v = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (mask >> i & 1u) { w += items[i].weight; v += items[i].value; }
        if (w > capacity) continue;
        best = std::max(best, v);
        const double left = capacity - w;
        for (std::size_t j = 0; j < n; ++j)
            if (!(mask >> j & 1u))
                best = std::max(best, v + items[j].value * std::min(1.0, left / items[j].weight));
    }
    return best;
}

int main()
{
    const std::vector<Item> items{{60, 10}, {100, 20}, {120, 30}};
    const auto plan = fractional_knapsack(items, 50);
    std::cout << std::fixed << std::setprecision(3);
    for (const auto& t : plan)
        std::cout << "take item " << t.index << " (v=" << items[t.index].value << ", w="
                  << items[t.index].weight << ") fraction " << t.fraction << "\n";
    std::cout << "total value = " << plan_value(items, plan) << "\n";
    assert(std::fabs(plan_value(items, plan) - 240.0) < 1e-9);
    assert(plan.size() == 3 && std::fabs(plan[2].fraction - 2.0 / 3.0) < 1e-9);

    std::mt19937 rng(1612);
    std::uniform_int_distribution<int> val(1, 100), wt(1, 30), cap(0, 90), cnt(1, 10);
    for (int t = 0; t < 2000; ++t) {
        std::vector<Item> v(static_cast<std::size_t>(cnt(rng)));
        for (auto& it : v) { it.value = val(rng); it.weight = wt(rng); }
        const double c = cap(rng);
        const auto p = fractional_knapsack(v, c);
        double used = 0;
        for (const auto& tk : p) used += tk.fraction * v[tk.index].weight;
        assert(used <= c + 1e-9);                                    // plan is feasible
        assert(std::fabs(plan_value(v, p) - exact(v, c)) < 1e-6);    // and optimal
    }
    std::cout << "2000 random instances: greedy == exact. OK\n";
    return 0;
}
