/*
 * 02_std_priority_queue_comparators.cpp — std::priority_queue with custom ordering
 *
 * Concept : std::priority_queue<T, Container, Compare> is a container ADAPTER
 *           over std::vector using make_heap/push_heap/pop_heap. The single
 *           most confusing fact: Compare is "less than", and the top is the
 *           element that is LARGEST under it. So std::greater => min-heap, and
 *           a lambda that says "a.deadline > b.deadline" => earliest deadline on top.
 * Build   : make run P=dsa/10-heaps-priority-queues/cpp/02_std_priority_queue_comparators.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <queue>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

struct Job {
    int           prio;      // bigger = more important
    std::uint64_t seq;       // arrival order, to break ties FIFO
    std::string   name;
};

// A comparator as a struct: stateless, zero-size, inlinable.
// Returns true when a should be BELOW b (i.e. "a has lower priority than b").
struct JobLower {
    bool operator()(const Job& a, const Job& b) const
    {
        if (a.prio != b.prio) return a.prio < b.prio;   // higher prio on top
        return a.seq > b.seq;                           // equal prio: OLDER on top (FIFO)
    }
};

template <class PQ>
std::vector<typename PQ::value_type> drain(PQ pq)        // O(n log n), takes a copy
{
    std::vector<typename PQ::value_type> out;
    while (!pq.empty()) { out.push_back(pq.top()); pq.pop(); }
    return out;
}

int main()
{
    const std::vector<int> data{5, 1, 9, 3, 7};

    // 1) default: max-heap
    std::priority_queue<int> maxq(data.begin(), data.end());   // O(n) range ctor
    assert((drain(maxq) == std::vector<int>{9, 7, 5, 3, 1}));

    // 2) min-heap: std::greater<> ("transparent" greater, C++14)
    std::priority_queue<int, std::vector<int>, std::greater<>> minq(data.begin(), data.end());
    assert((drain(minq) == std::vector<int>{1, 3, 5, 7, 9}));
    std::cout << "max-heap and min-heap via std::greater ok\n";

    // 3) struct comparator with FIFO tie-break (a stable priority queue)
    std::priority_queue<Job, std::vector<Job>, JobLower> jobs;
    std::uint64_t seq = 0;
    for (auto [p, n] : std::vector<std::pair<int, std::string>>{
             {1, "log-a"}, {5, "audio"}, {1, "log-b"}, {5, "video"}, {3, "net"}}) {
        jobs.push(Job{p, seq++, n});
    }
    std::vector<std::string> order;
    std::cout << "jobs:";
    for (const Job& j : drain(jobs)) { std::cout << ' ' << j.name; order.push_back(j.name); }
    std::cout << '\n';
    assert((order == std::vector<std::string>{"audio", "video", "net", "log-a", "log-b"}));

    // 4) lambda comparator: the TYPE goes in the template, the OBJECT in the ctor.
    //    Earliest deadline first (EDF), like SCHED_DEADLINE picks tasks.
    struct Req { std::uint64_t deadline_ns; int id; };
    auto later = [](const Req& a, const Req& b) { return a.deadline_ns > b.deadline_ns; };
    std::priority_queue<Req, std::vector<Req>, decltype(later)> edf(later);
    edf.push({3000, 1}); edf.push({1000, 2}); edf.push({2000, 3});
    assert(edf.top().id == 2); edf.pop();
    assert(edf.top().id == 3); edf.pop();
    assert(edf.top().id == 1);
    std::cout << "EDF lambda comparator ok\n";

    // 5) pairs / tuples compare lexicographically, so you often need no comparator.
    //    (dist, node) min-heap is the classic Dijkstra frontier.
    using DN = std::pair<int, int>;
    std::priority_queue<DN, std::vector<DN>, std::greater<DN>> frontier;
    frontier.emplace(7, 0); frontier.emplace(2, 3); frontier.emplace(2, 1);
    assert(frontier.top() == DN(2, 1));                  // dist 2, then smaller node id
    std::cout << "pair min-heap ok (top = (2,1))\n";

    // 6) Trick: negate keys to get a min-heap from the default max-heap.
    //    Works for ints, but -INT_MIN overflows: prefer std::greater.
    std::priority_queue<int> neg;
    for (int x : data) neg.push(-x);
    assert(-neg.top() == 1);

    // 7) Pitfall demo: a comparator that is not a strict weak ordering (<=)
    //    is undefined behaviour in the STL. We only show the correct one.
    //    Pitfall 2: there is no decrease-key and no iteration in priority_queue;
    //    if you need those, use std::set (see ../../09-trees) or an indexed heap
    //    (06_indexed_heap_dijkstra.cpp).

    // 8) Access the underlying container: derive and use the protected member c.
    struct Inspectable : std::priority_queue<int> {
        const std::vector<int>& raw() const { return c; }
    } ins;
    for (int x : data) ins.push(x);
    const auto& raw = ins.raw();
    for (std::size_t i = 1; i < raw.size(); ++i) assert(raw[(i - 1) / 2] >= raw[i]);
    std::cout << "underlying vector is a valid max-heap:";
    for (int x : raw) std::cout << ' ' << x;
    std::cout << '\n';

    std::cout << "all checks passed\n";
    return 0;
}
