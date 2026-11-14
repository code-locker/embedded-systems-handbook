/*
 * 01_generic_sorts_templates.cpp — insertion, merge, quick and heap sort as
 *                                  iterator + comparator templates
 *
 * Concept : The C++ answer to qsort's void* + function pointer: templates.
 *           template <class RandomIt, class Compare = std::less<>>
 *           The compiler stamps out a version per element type AND per
 *           comparator type, so a lambda comparator is INLINED — no indirect
 *           call, no memcpy of raw bytes, type-checked. Same algorithms as the
 *           C programs, written once for any container and any ordering.
 *           Every sort is checked against std::sort / std::stable_sort.
 * Build   : make run P=dsa/14-sorting-and-searching/cpp/01_generic_sorts_templates.cpp
 * Read    : ../README.md  (section "3.10 Generic sorting: qsort vs templates")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace mysort {

// ---------------------------------------------------------------------------
// Insertion sort. Uses std::move so it also works for move-only or
// expensive-to-copy types (std::string, std::unique_ptr...).
// Only needs bidirectional iterators in principle; we use random access.
// ---------------------------------------------------------------------------
template <class It, class Cmp = std::less<>>
void insertion_sort(It first, It last, Cmp cmp = Cmp{})
{
    if (first == last) return;
    for (It i = std::next(first); i != last; ++i) {
        auto key = std::move(*i);
        It j = i;
        // cmp(key, prev): strictly less -> shift. Equal stops -> STABLE.
        while (j != first && cmp(key, *std::prev(j))) {
            *j = std::move(*std::prev(j));
            --j;
        }
        *j = std::move(key);
    }
}

// ---------------------------------------------------------------------------
// Merge sort (top-down, stable). The buffer is a std::vector -> RAII frees it
// even if a comparator throws. Merging takes from the LEFT on ties:
//    !cmp(right, left)  ==  left <= right   (only '<' is available!)
// ---------------------------------------------------------------------------
template <class It, class Cmp>
void merge_rec(It first, It last, Cmp &cmp,
               std::vector<typename std::iterator_traits<It>::value_type> &buf)
{
    auto n = std::distance(first, last);
    if (n <= 16) { insertion_sort(first, last, cmp); return; }
    It mid = first + n / 2;
    merge_rec(first, mid, cmp, buf);
    merge_rec(mid, last, cmp, buf);
    if (!cmp(*mid, *std::prev(mid))) return;          // already in order
    buf.clear();
    It i = first, j = mid;
    while (i != mid && j != last)
        buf.push_back(cmp(*j, *i) ? std::move(*j++) : std::move(*i++));
    std::move(i, mid, std::back_inserter(buf));
    std::move(j, last, std::back_inserter(buf));
    std::move(buf.begin(), buf.end(), first);
}

template <class It, class Cmp = std::less<>>
void merge_sort(It first, It last, Cmp cmp = Cmp{})
{
    std::vector<typename std::iterator_traits<It>::value_type> buf;
    buf.reserve(static_cast<size_t>(std::distance(first, last)));
    merge_rec(first, last, cmp, buf);
}

// ---------------------------------------------------------------------------
// Quicksort: random pivot, 3-way partition with std::partition, small side
// recursion. Shows how to build an algorithm out of STL building blocks:
//
//   [ cmp(x,p) | !cmp(x,p) && !cmp(p,x)  (== p) | cmp(p,x) ]
//    first     m1                             m2          last
// ---------------------------------------------------------------------------
template <class It, class Cmp = std::less<>>
void quick_sort(It first, It last, Cmp cmp = Cmp{})
{
    static std::mt19937 rng(12345);
    while (std::distance(first, last) > 16) {
        auto n = std::distance(first, last);
        auto pivot = *(first + std::uniform_int_distribution<long>(0, n - 1)(rng)); // copy!
        It m1 = std::partition(first, last, [&](const auto &x) { return cmp(x, pivot); });
        It m2 = std::partition(m1, last, [&](const auto &x) { return !cmp(pivot, x); });
        // [first,m1) < pivot, [m1,m2) == pivot (final), [m2,last) > pivot
        if (m1 - first < last - m2) { quick_sort(first, m1, cmp); first = m2; }
        else                        { quick_sort(m2, last, cmp);  last = m1; }
    }
    insertion_sort(first, last, cmp);
}

// ---------------------------------------------------------------------------
// Heapsort with a hand-written sift-down (std::make_heap/sort_heap shown after).
// ---------------------------------------------------------------------------
template <class It, class Cmp>
void sift_down(It first, std::ptrdiff_t i, std::ptrdiff_t n, Cmp &cmp)
{
    auto item = std::move(first[i]);
    for (;;) {
        std::ptrdiff_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && cmp(first[c], first[c + 1])) ++c;  // larger child
        if (!cmp(item, first[c])) break;
        first[i] = std::move(first[c]);
        i = c;
    }
    first[i] = std::move(item);
}

template <class It, class Cmp = std::less<>>
void heap_sort(It first, It last, Cmp cmp = Cmp{})
{
    std::ptrdiff_t n = last - first;
    for (std::ptrdiff_t i = n / 2 - 1; i >= 0; --i) sift_down(first, i, n, cmp);
    for (std::ptrdiff_t e = n - 1; e > 0; --e) {
        std::iter_swap(first, first + e);
        sift_down(first, 0, e, cmp);
    }
}

} // namespace mysort

// ---- test harness ---------------------------------------------------------
struct Frame {                 // a video frame descriptor, sorted by timestamp
    int64_t     pts;           // presentation timestamp
    std::string tag;
    int         arrival;       // order of arrival, for stability checks
};

template <class SortFn>
void check_ints(const char *name, SortFn sortfn, unsigned maxn)
{
    std::mt19937 rng(7);
    for (int t = 0; t < 200; ++t) {
        std::vector<int> v(rng() % maxn);
        int range = (t % 3 == 0) ? 4 : 1000000;
        for (int &x : v) x = static_cast<int>(rng() % range) - range / 2;
        auto ref = v;
        std::sort(ref.begin(), ref.end());
        sortfn(v.begin(), v.end(), std::less<>{});
        assert(v == ref);
        sortfn(v.begin(), v.end(), std::greater<>{});      // other direction
        std::sort(ref.begin(), ref.end(), std::greater<>{});
        assert(v == ref);
    }
    std::cout << "  " << name << ": ints asc/desc == std::sort on 200 vectors (n < " << maxn << ")\n";
}

int main()
{
    std::cout << "=== Generic sorts as C++ templates ===\n\n";

    auto ins   = [](auto f, auto l, auto c) { mysort::insertion_sort(f, l, c); };
    auto mrg   = [](auto f, auto l, auto c) { mysort::merge_sort(f, l, c); };
    auto qck   = [](auto f, auto l, auto c) { mysort::quick_sort(f, l, c); };
    auto hp    = [](auto f, auto l, auto c) { mysort::heap_sort(f, l, c); };
    check_ints("insertion_sort", ins, 300);    // O(n^2): keep n small
    check_ints("merge_sort    ", mrg, 2000);
    check_ints("quick_sort    ", qck, 2000);
    check_ints("heap_sort     ", hp, 2000);

    // Same templates, different container and element type: std::deque<string>
    std::deque<std::string> words{"queue", "pad", "caps", "bin", "element", "bus", "src", "sink"};
    auto wref = words;
    std::sort(wref.begin(), wref.end());
    mysort::quick_sort(words.begin(), words.end());
    assert(std::equal(words.begin(), words.end(), wref.begin()));
    std::cout << "\nstd::deque<std::string> sorted with the same quick_sort: ";
    for (const auto &w : words) std::cout << w << ' ';
    std::cout << '\n';

    // Sort by length, then alphabetically — a lambda with two keys via tuple
    mysort::merge_sort(words.begin(), words.end(), [](const std::string &a, const std::string &b) {
        return std::make_tuple(a.size(), a) < std::make_tuple(b.size(), b);
    });
    std::cout << "by (length, text): ";
    for (const auto &w : words) std::cout << w << ' ';
    std::cout << '\n';
    assert(words.front() == "bin" && words.back() == "element");

    // Stability with records: frames arriving out of order, many equal pts.
    std::mt19937 rng(99);
    std::vector<Frame> frames;
    for (int i = 0; i < 1000; ++i)
        frames.push_back({static_cast<int64_t>(rng() % 50) * 40000, "f" + std::to_string(i), i});
    auto by_pts = [](const Frame &a, const Frame &b) { return a.pts < b.pts; };

    auto f1 = frames, f2 = frames;
    mysort::merge_sort(f1.begin(), f1.end(), by_pts);
    std::stable_sort(f2.begin(), f2.end(), by_pts);
    for (size_t i = 0; i < f1.size(); ++i) assert(f1[i].arrival == f2[i].arrival);
    std::cout << "\nmerge_sort on 1000 frames == std::stable_sort (same order of equal pts)\n";

    auto f3 = frames;
    mysort::heap_sort(f3.begin(), f3.end(), by_pts);
    assert(std::is_sorted(f3.begin(), f3.end(), by_pts));
    bool heap_kept_order = true;
    for (size_t i = 1; i < f3.size(); ++i)
        if (f3[i - 1].pts == f3[i].pts && f3[i - 1].arrival > f3[i].arrival) heap_kept_order = false;
    std::cout << "heap_sort: sorted, but equal-pts frames kept arrival order? "
              << (heap_kept_order ? "yes (luck)" : "NO -> heapsort is unstable") << '\n';
    assert(!heap_kept_order);

    // The STL heap primitives do the same job as our heap_sort.
    std::vector<int> h{4, 10, 3, 5, 1, 8, 7};
    std::make_heap(h.begin(), h.end());        // O(n) bottom-up build
    std::sort_heap(h.begin(), h.end());        // n pops -> ascending
    assert(std::is_sorted(h.begin(), h.end()));
    std::cout << "std::make_heap + std::sort_heap: ";
    for (int x : h) std::cout << x << ' ';
    std::cout << "\n\nAll template sort checks passed.\n";
    return 0;
}
