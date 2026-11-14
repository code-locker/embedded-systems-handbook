/*
 * 05_min_platforms_rooms.cpp — minimum platforms / meeting rooms, and WHICH room each gets
 *
 * Concept : Two views of the same greedy:
 *             (a) event sweep: +1 at arrival, -1 at departure, answer = peak
 *             (b) room assignment: process trains by arrival; reuse the room
 *                 that frees up EARLIEST (a min-heap of end times). If even that
 *                 room is still busy, open a new one.
 *           (b) also tells you the assignment — how a scheduler hands out
 *           channels/DMA buffers/IRQ vectors. Both checked against brute force.
 * Build   : make run P=dsa/16-greedy/cpp/05_min_platforms_rooms.cpp
 * Read    : ../README.md  (section "Minimum platforms")
 */
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <utility>
#include <vector>

struct Train {
    int arr;
    int dep;  // occupies the platform over the CLOSED interval [arr, dep]
};

// (a) Sweep over 2n events. Encoding: (time, +1 arrival / -1 departure).
// Sorting pairs puts -1 before +1 at equal times, which would let a train leave
// and another arrive "at the same instant" on one platform. We need the
// opposite (closed intervals), so sort with arrivals first on ties.
static int peak_by_sweep(const std::vector<Train>& ts)
{
    std::vector<std::pair<int, int>> ev;
    for (const auto& t : ts) { ev.emplace_back(t.arr, +1); ev.emplace_back(t.dep, -1); }
    std::sort(ev.begin(), ev.end(), [](const auto& x, const auto& y) {
        return x.first != y.first ? x.first < y.first : x.second > y.second;  // +1 first
    });
    int cur = 0, peak = 0;
    for (const auto& e : ev) peak = std::max(peak, cur += e.second);
    return peak;
}

/*
 * (b) Min-heap of (end time, room id).
 *
 *   trains by arrival:  A[900,910] B[940,1120] C[950,1130] D[1100,1200]
 *   A: heap empty               -> room 0, heap {(910,0)}
 *   B: earliest end 910 < 940   -> reuse room 0, heap {(1120,0)}
 *   C: earliest end 1120 >= 950 -> new room 1, heap {(1120,0),(1130,1)}
 *   D: earliest end 1120 >= 1100-> new room 2
 *
 * Returns room id per train (indexed like the input). O(n log n).
 */
static std::vector<int> assign_rooms(const std::vector<Train>& ts, int& rooms_used)
{
    std::vector<std::size_t> order(ts.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ts[a].arr < ts[b].arr; });

    using Busy = std::pair<int, int>;  // (dep time, room)
    std::priority_queue<Busy, std::vector<Busy>, std::greater<Busy>> busy;  // min-heap
    std::vector<int> room(ts.size(), -1);
    rooms_used = 0;
    for (std::size_t i : order) {
        if (!busy.empty() && busy.top().first < ts[i].arr) {  // strictly free before we arrive
            room[i] = busy.top().second;
            busy.pop();
        } else {
            room[i] = rooms_used++;
        }
        busy.emplace(ts[i].dep, room[i]);
    }
    return room;
}

static int brute_peak(const std::vector<Train>& ts)
{
    int best = 0;
    for (const auto& k : ts) {
        int c = 0;
        for (const auto& t : ts) c += (t.arr <= k.arr && k.arr <= t.dep);
        best = std::max(best, c);
    }
    return best;
}

static bool assignment_valid(const std::vector<Train>& ts, const std::vector<int>& room)
{
    for (std::size_t i = 0; i < ts.size(); ++i)
        for (std::size_t j = i + 1; j < ts.size(); ++j)
            if (room[i] == room[j] && ts[i].arr <= ts[j].dep && ts[j].arr <= ts[i].dep) return false;
    return true;
}

int main()
{
    const std::vector<Train> day{{900, 910}, {940, 1120}, {950, 1130}, {1100, 1200}, {1500, 1900}, {1800, 2000}};
    int used = 0;
    const auto room = assign_rooms(day, used);
    for (std::size_t i = 0; i < day.size(); ++i)
        std::cout << "train [" << day[i].arr << "," << day[i].dep << "] -> platform " << room[i] << "\n";
    std::cout << "platforms: sweep=" << peak_by_sweep(day) << " heap=" << used << "\n";
    assert(peak_by_sweep(day) == 3 && used == 3 && assignment_valid(day, room));

    std::mt19937 rng(1615);
    std::uniform_int_distribution<int> n(1, 30), at(0, 60), stay(0, 15);
    for (int t = 0; t < 4000; ++t) {
        std::vector<Train> ts(static_cast<std::size_t>(n(rng)));
        for (auto& tr : ts) { tr.arr = at(rng); tr.dep = tr.arr + stay(rng); }
        const int ref = brute_peak(ts);
        int u = 0;
        const auto r = assign_rooms(ts, u);
        assert(peak_by_sweep(ts) == ref);
        assert(u == ref);  // heap greedy uses exactly the lower bound -> optimal
        assert(assignment_valid(ts, r));
    }
    std::cout << "4000 random timetables: sweep == heap == brute force, assignments valid. OK\n";
    return 0;
}
