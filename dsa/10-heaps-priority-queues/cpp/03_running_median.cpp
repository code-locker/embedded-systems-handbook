/*
 * 03_running_median.cpp — median of a stream with two heaps
 *
 * Concept : Keep the smaller half in a MAX-heap (lo) and the larger half in a
 *           MIN-heap (hi), with size(lo) == size(hi) or size(hi)+1. The median
 *           is lo.top() (odd count) or the average of the two tops (even).
 *           Insert O(log n), median O(1). Used for jitter/latency monitoring,
 *           e.g. the median frame interval of a camera stream.
 * Build   : make run P=dsa/10-heaps-priority-queues/cpp/03_running_median.cpp
 * Read    : ../README.md  (section "Running median")
 */
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <vector>

/*
 *            lo (max-heap)          hi (min-heap)
 *         smaller half, top =     larger half, top =
 *         its LARGEST element     its SMALLEST element
 *
 *      [1 2 3 | 5]  top 5   <=   [7 | 8 9]  top 7
 *                     \             /
 *                      median = 5 (lo has one extra)
 *
 *   Every value in lo <= every value in hi; the median lives at the boundary.
 */
class RunningMedian {
public:
    void add(int x)                         // O(log n)
    {
        // 1. route: goes to lo if it belongs in the smaller half
        if (lo_.empty() || x <= lo_.top()) lo_.push(x);
        else                               hi_.push(x);
        // 2. rebalance: move ONE top across so sizes differ by at most 1
        if (lo_.size() > hi_.size() + 1)  { hi_.push(lo_.top()); lo_.pop(); }
        else if (hi_.size() > lo_.size()) { lo_.push(hi_.top()); hi_.pop(); }
    }

    // Returns the median doubled, so it stays an exact integer (no float rounding).
    long long median_x2() const            // O(1)
    {
        assert(!lo_.empty());
        if (lo_.size() > hi_.size()) return 2LL * lo_.top();
        return static_cast<long long>(lo_.top()) + hi_.top();
    }

    double median() const { return static_cast<double>(median_x2()) / 2.0; }
    std::size_t size() const { return lo_.size() + hi_.size(); }

    bool invariant() const
    {
        if (lo_.size() != hi_.size() && lo_.size() != hi_.size() + 1) return false;
        if (!hi_.empty() && lo_.top() > hi_.top()) return false;
        return true;
    }

private:
    std::priority_queue<int>                                   lo_;  // max-heap
    std::priority_queue<int, std::vector<int>, std::greater<>> hi_;  // min-heap
};

// Reference: keep a sorted vector, insert with lower_bound. O(n) per insert.
static long long ref_median_x2(const std::vector<int>& sorted)
{
    std::size_t n = sorted.size();
    if (n % 2) return 2LL * sorted[n / 2];
    return static_cast<long long>(sorted[n / 2 - 1]) + sorted[n / 2];
}

int main()
{
    RunningMedian rm;
    std::cout << "stream -> median:\n";
    for (int x : {5, 15, 1, 3, 8, 7, 9, 10}) {
        rm.add(x);
        std::cout << "  add " << x << " -> " << rm.median() << '\n';
    }
    assert(rm.median_x2() == 15);   // sorted: 1 3 5 7 8 9 10 15 -> (7+8)/2

    std::mt19937 rng(77);
    for (int round = 0; round < 50; ++round) {
        RunningMedian m;
        std::vector<int> sorted;
        std::uniform_int_distribution<int> d(-500, 500);
        for (int i = 0; i < 1000; ++i) {
            int x = d(rng);
            m.add(x);
            sorted.insert(std::lower_bound(sorted.begin(), sorted.end(), x), x);
            assert(m.invariant());
            assert(m.median_x2() == ref_median_x2(sorted));
        }
    }
    std::cout << "50 random streams x 1000 values: two-heap median == sorted reference\n";
    std::cout << "all checks passed\n";
    return 0;
}
