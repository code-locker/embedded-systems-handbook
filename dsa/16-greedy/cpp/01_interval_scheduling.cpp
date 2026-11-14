/*
 * 01_interval_scheduling.cpp — the interval-greedy family with std::sort + lambdas
 *
 * Concept : One idea, three problems. Sort intervals by END; sweep keeping the
 *           end of the last interval you kept.
 *             1. max non-overlapping intervals      (activity selection)
 *             2. min intervals to remove            (= n - answer of 1)
 *             3. min "arrows"/points that stab all  (same sweep, closed intervals)
 *           Each checked against brute force over all subsets on random input.
 * Build   : make run P=dsa/16-greedy/cpp/01_interval_scheduling.cpp
 * Read    : ../README.md  (section "Activity selection")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <iostream>
#include <random>
#include <vector>

struct Interval {
    int start;
    int end;
};

// Half-open [start, end): an interval ending at 5 and one starting at 5 do NOT clash.
static bool overlap_half_open(const Interval& a, const Interval& b)
{
    return a.start < b.end && b.start < a.end;
}

// O(n log n). Takes by value: we sort a copy, the caller's order is untouched.
static std::vector<Interval> max_non_overlapping(std::vector<Interval> v)
{
    std::sort(v.begin(), v.end(), [](const Interval& a, const Interval& b) { return a.end < b.end; });
    std::vector<Interval> kept;
    int last_end = INT_MIN;
    for (const auto& iv : v) {
        if (iv.start >= last_end) {   // compatible with everything kept so far
            kept.push_back(iv);
            last_end = iv.end;
        }
    }
    return kept;
}

// "Erase overlap intervals": keeping the maximum is the same as removing the minimum.
static int min_removals(const std::vector<Interval>& v)
{
    return static_cast<int>(v.size() - max_non_overlapping(v).size());
}

/*
 * Minimum arrows: each arrow is a point x; it bursts every CLOSED interval [s, e]
 * with s <= x <= e. Greedy: shoot at the end of the earliest-ending balloon —
 * the rightmost point that still hits it, so it also hits as many others as possible.
 *
 *   [1======6]
 *       [2=====8]
 *              [7=====12]
 *                  [10=====16]
 *   arrow at 6  -> bursts [1,6],[2,8]
 *   arrow at 12 -> bursts [7,12],[10,16]       answer 2
 */
static int min_arrows(std::vector<Interval> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end(), [](const Interval& a, const Interval& b) { return a.end < b.end; });
    int arrows = 1;
    int x = v[0].end;
    for (const auto& iv : v) {
        if (iv.start > x) {   // closed intervals: start == x is still hit
            ++arrows;
            x = iv.end;
        }
    }
    return arrows;
}

// ---------------- brute-force references (n <= 12) ----------------
static int brute_max_non_overlapping(const std::vector<Interval>& v)
{
    const int n = static_cast<int>(v.size());
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        bool ok = true;
        for (int i = 0; i < n && ok; ++i)
            for (int j = i + 1; j < n && ok; ++j)
                if ((mask >> i & 1u) && (mask >> j & 1u) && overlap_half_open(v[i], v[j])) ok = false;
        if (ok) best = std::max(best, __builtin_popcount(mask));
    }
    return best;
}

// Candidate arrow positions: interval endpoints suffice (any arrow can slide right to an end).
static int brute_min_arrows(const std::vector<Interval>& v)
{
    std::vector<int> pts;
    for (const auto& iv : v) pts.push_back(iv.end);
    const int m = static_cast<int>(pts.size());
    int best = m;
    for (unsigned mask = 0; mask < (1u << m); ++mask) {
        bool all = true;
        for (const auto& iv : v) {
            bool hit = false;
            for (int k = 0; k < m && !hit; ++k)
                if ((mask >> k & 1u) && iv.start <= pts[k] && pts[k] <= iv.end) hit = true;
            if (!hit) { all = false; break; }
        }
        if (all) best = std::min(best, __builtin_popcount(mask));
    }
    return best;
}

int main()
{
    const std::vector<Interval> demo{{0, 2}, {1, 4}, {3, 5}, {4, 7}, {6, 8}, {2, 9}};
    const auto kept = max_non_overlapping(demo);
    std::cout << "kept:";
    for (const auto& iv : kept) std::cout << " [" << iv.start << "," << iv.end << ")";
    std::cout << "  -> " << kept.size() << " intervals, remove " << min_removals(demo) << "\n";
    assert(kept.size() == 3 && min_removals(demo) == 3);

    const std::vector<Interval> balloons{{10, 16}, {2, 8}, {1, 6}, {7, 12}};
    std::cout << "min arrows for balloons = " << min_arrows(balloons) << "\n";
    assert(min_arrows(balloons) == 2);

    std::mt19937 rng(1611);
    std::uniform_int_distribution<int> start(0, 20), len(1, 8), size(1, 11);
    for (int t = 0; t < 1500; ++t) {
        std::vector<Interval> v(static_cast<size_t>(size(rng)));
        for (auto& iv : v) { iv.start = start(rng); iv.end = iv.start + len(rng); }
        assert(static_cast<int>(max_non_overlapping(v).size()) == brute_max_non_overlapping(v));
        assert(min_arrows(v) == brute_min_arrows(v));
    }
    std::cout << "1500 random instances: all three greedies == brute force. OK\n";
    return 0;
}
