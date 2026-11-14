/*
 * 04_quickselect_topk.cpp — quickselect as a template, std::nth_element,
 *                           and three ways to get the top-k
 *
 * Concept : "k-th smallest" needs only ONE side of each partition -> O(n) avg.
 *           Top-k has three standard answers; pick by situation:
 *             nth_element + sort first k : O(n + k log k), all data in memory
 *             partial_sort               : O(n log k), simple
 *             min-heap of size k         : O(n log k), STREAMING — sees each
 *                                          item once, O(k) memory (sensor data,
 *                                          packet flows, log processing)
 *           Plus a 1-D median filter (salt-and-pepper noise removal), the
 *           classic image-processing use of selection.
 * Build   : make run P=dsa/14-sorting-and-searching/cpp/04_quickselect_topk.cpp
 * Read    : ../README.md  (section "3.9 Selection: quickselect and nth_element")
 */
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <queue>
#include <random>
#include <vector>

// ---------------------------------------------------------------------------
// quickselect: after the call, *nth is what would be there if [first,last)
// were sorted, everything before is !(nth < x), everything after !(x < nth).
// Same contract as std::nth_element. Iterative: one side kept each round.
// ---------------------------------------------------------------------------
template <class It, class Cmp = std::less<>>
void quickselect(It first, It nth, It last, Cmp cmp = Cmp{})
{
    static std::mt19937 rng(42);
    while (last - first > 1) {
        auto pivot = *(first + std::uniform_int_distribution<long>(0, (last - first) - 1)(rng));
        // 3-way split:  [ < pivot | == pivot | > pivot ]
        It m1 = std::partition(first, last, [&](const auto &x) { return cmp(x, pivot); });
        It m2 = std::partition(m1, last, [&](const auto &x) { return !cmp(pivot, x); });
        if (nth < m1)       last = m1;          // answer among the smaller ones
        else if (nth < m2)  return;             // nth landed in the == block: done
        else                first = m2;         // answer among the larger ones
    }
}

// Streaming top-k: keep the k LARGEST seen so far in a MIN-heap; the root is
// the smallest of the current top-k, i.e. the bar a newcomer must beat.
//     heap (k=3): root=7 {7, 9, 12}   new 10 > 7 -> pop 7, push 10 -> {9,10,12}
template <class T>
std::vector<T> topk_stream(const std::vector<T> &stream, size_t k)
{
    std::priority_queue<T, std::vector<T>, std::greater<T>> heap;   // min-heap
    for (const T &x : stream) {
        if (heap.size() < k) heap.push(x);
        else if (k && heap.top() < x) { heap.pop(); heap.push(x); }   // O(log k)
    }
    std::vector<T> out;
    while (!heap.empty()) { out.push_back(heap.top()); heap.pop(); }
    std::reverse(out.begin(), out.end());   // largest first
    return out;
}

// Median filter, window 2r+1. Edges: clamp the index (replicate border).
static std::vector<int> median_filter(const std::vector<int> &in, int r)
{
    std::vector<int> out(in.size()), win;
    const long n = static_cast<long>(in.size());
    for (long i = 0; i < n; ++i) {
        win.clear();
        for (long j = i - r; j <= i + r; ++j) win.push_back(in[static_cast<size_t>(std::clamp(j, 0L, n - 1))]);
        std::nth_element(win.begin(), win.begin() + r, win.end());   // O(window)
        out[static_cast<size_t>(i)] = win[static_cast<size_t>(r)];
    }
    return out;
}

int main()
{
    std::cout << "=== Quickselect, nth_element and top-k ===\n\n";
    std::mt19937 rng(3);

    // ---- quickselect vs nth_element vs sort ----------------------------------
    for (int t = 0; t < 500; ++t) {
        std::vector<int> v(1 + rng() % 2000);
        int range = (t & 1) ? 5 : 1'000'000;
        for (int &x : v) x = static_cast<int>(rng() % range);
        auto sorted = v; std::sort(sorted.begin(), sorted.end());
        size_t k = rng() % v.size();
        auto a = v, b = v;
        quickselect(a.begin(), a.begin() + static_cast<long>(k), a.end());
        std::nth_element(b.begin(), b.begin() + static_cast<long>(k), b.end());
        assert(a[k] == sorted[k] && b[k] == sorted[k]);
        for (size_t i = 0; i < k; ++i) assert(a[i] <= a[k]);
        for (size_t i = k + 1; i < a.size(); ++i) assert(a[i] >= a[k]);
    }
    std::cout << "quickselect == std::nth_element == sort()[k] on 500 vectors (with duplicates)\n";

    // ---- top-k three ways ----------------------------------------------------
    std::vector<int> data(100000);
    for (int &x : data) x = static_cast<int>(rng() % 1'000'000);
    const size_t k = 10;

    auto v1 = data;   // (a) nth_element (descending) then sort the first k
    std::nth_element(v1.begin(), v1.begin() + k - 1, v1.end(), std::greater<>{});
    std::sort(v1.begin(), v1.begin() + k, std::greater<>{});
    v1.resize(k);

    auto v2 = data;   // (b) partial_sort
    std::partial_sort(v2.begin(), v2.begin() + k, v2.end(), std::greater<>{});
    v2.resize(k);

    auto v3 = topk_stream(data, k);   // (c) streaming min-heap

    auto ref = data; std::sort(ref.begin(), ref.end(), std::greater<>{}); ref.resize(k);
    assert(v1 == ref && v2 == ref && v3 == ref);
    std::cout << "top-" << k << " of 100000 (nth_element / partial_sort / streaming heap agree):\n  ";
    for (int x : v3) std::cout << x << ' ';
    std::cout << '\n';
    assert(topk_stream(std::vector<int>{3, 1, 2}, 5) == (std::vector<int>{3, 2, 1}));  // k > n
    assert(topk_stream(std::vector<int>{3, 1, 2}, 0).empty());

    // ---- median filter removes impulse noise ---------------------------------
    //   clean ramp:   10 11 12 13 14 15 16 17
    //   noisy:        10 11 255 13 14 0 16 17    (two "dead pixels")
    //   median r=1:   10 11 13 14 13 14 16 17    spikes gone, edges kept
    std::vector<int> noisy{10, 11, 255, 13, 14, 0, 16, 17};
    auto filt = median_filter(noisy, 1);
    std::cout << "\nmedian filter (window 3):\n  in : ";
    for (int x : noisy) std::cout << x << ' ';
    std::cout << "\n  out: ";
    for (int x : filt) std::cout << x << ' ';
    std::cout << '\n';
    assert((filt == std::vector<int>{10, 11, 13, 14, 13, 14, 16, 17}));
    for (int x : filt) assert(x > 5 && x < 50);      // no spike survives

    std::cout << "\nAll selection checks passed.\n";
    return 0;
}
