/*
 * 04_jump_and_gas.cpp — "running maximum / running sum" greedies: jump game and gas station
 *
 * Concept : Both problems look like they need search (try every path, try every
 *           start) but a single left-to-right pass carrying ONE number suffices:
 *             jump game  : the farthest index reachable so far
 *             gas station: the tank since the current candidate start
 *           Written with std::vector / std::accumulate, checked against BFS and
 *           brute-force simulation.
 * Build   : make run P=dsa/16-greedy/cpp/04_jump_and_gas.cpp
 * Read    : ../README.md  (sections "Jump game" and "Gas station")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <vector>

// Jump game II as an implicit BFS: each "level" (indices reachable in k jumps)
// is a contiguous window, so we only track its right end. O(n) time, O(1) space.
static std::optional<int> min_jumps(const std::vector<int>& a)
{
    const int n = static_cast<int>(a.size());
    int jumps = 0, window_end = 0, farthest = 0;
    for (int i = 0; i < n - 1; ++i) {
        if (i > farthest) return std::nullopt;   // stranded before reaching i
        farthest = std::max(farthest, i + a[static_cast<std::size_t>(i)]);
        if (i == window_end) {                   // must jump to go further
            if (farthest == i) return std::nullopt;
            ++jumps;
            window_end = farthest;
        }
    }
    return jumps;
}

// Reference: explicit BFS with a queue, O(n^2) edges.
static std::optional<int> bfs_jumps(const std::vector<int>& a)
{
    const int n = static_cast<int>(a.size());
    std::vector<int> dist(a.size(), -1);
    std::queue<int> q;
    dist[0] = 0;
    q.push(0);
    while (!q.empty()) {
        const int i = q.front();
        q.pop();
        for (int j = i + 1; j <= i + a[static_cast<std::size_t>(i)] && j < n; ++j)
            if (dist[static_cast<std::size_t>(j)] < 0) { dist[static_cast<std::size_t>(j)] = dist[static_cast<std::size_t>(i)] + 1; q.push(j); }
    }
    if (dist.back() < 0) return std::nullopt;
    return dist.back();
}

// Gas station: O(n). Returns the start index or nullopt.
static std::optional<std::size_t> gas_start(const std::vector<int>& gas, const std::vector<int>& cost)
{
    // Feasibility is a one-liner: total gas must cover total cost.
    const long total = std::accumulate(gas.begin(), gas.end(), 0L) - std::accumulate(cost.begin(), cost.end(), 0L);
    if (total < 0) return std::nullopt;
    long tank = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < gas.size(); ++i) {
        tank += gas[i] - cost[i];
        if (tank < 0) { start = i + 1; tank = 0; }   // nobody in [start..i] can work
    }
    return start;  // start < n is guaranteed when total >= 0
}

static bool completes_from(const std::vector<int>& gas, const std::vector<int>& cost, std::size_t s)
{
    long tank = 0;
    for (std::size_t k = 0; k < gas.size(); ++k) {
        const std::size_t i = (s + k) % gas.size();
        tank += gas[i] - cost[i];
        if (tank < 0) return false;
    }
    return true;
}

int main()
{
    assert(min_jumps({2, 3, 1, 1, 4}) == 2);
    assert(!min_jumps({3, 2, 1, 0, 4}).has_value());
    assert(min_jumps({7}) == 0);
    std::cout << "jump [2,3,1,1,4] -> " << *min_jumps({2, 3, 1, 1, 4}) << " jumps\n";

    const std::vector<int> gas{1, 2, 3, 4, 5}, cost{3, 4, 5, 1, 2};
    std::cout << "gas station start -> " << *gas_start(gas, cost) << "\n";
    assert(gas_start(gas, cost) == 3u);

    std::mt19937 rng(1614);
    std::uniform_int_distribution<int> len(1, 40), small(0, 3), g(0, 5);
    for (int t = 0; t < 10000; ++t) {
        std::vector<int> a(static_cast<std::size_t>(len(rng)));
        for (auto& x : a) x = small(rng);
        assert(min_jumps(a) == bfs_jumps(a));

        std::vector<int> ga(static_cast<std::size_t>(len(rng) % 12 + 1)), co(ga.size());
        for (auto& x : ga) x = g(rng);
        for (auto& x : co) x = g(rng);
        bool any = false;
        for (std::size_t s = 0; s < ga.size(); ++s) any = any || completes_from(ga, co, s);
        const auto st = gas_start(ga, co);
        assert(st.has_value() == any);
        if (st) assert(completes_from(ga, co, *st));
    }
    std::cout << "10000 random cases: greedy == BFS / brute force. OK\n";
    return 0;
}
