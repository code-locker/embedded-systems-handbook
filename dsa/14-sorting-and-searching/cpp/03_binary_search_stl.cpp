/*
 * 03_binary_search_stl.cpp — lower_bound / upper_bound / equal_range /
 *                            partition_point and a generic "first true" search
 *
 * Concept : The whole family is ONE idea: on a range where a predicate goes
 *           F F F T T T, find the first T.  The STL spells it
 *               std::partition_point(first, last, pred_is_F)
 *           and lower_bound/upper_bound are just partition_point with
 *           "x < value" / "!(value < x)". For answers that are numbers, not
 *           positions, write first_true(lo, hi, pred) once and reuse it.
 *           Real use here: seeking in a media file — find the keyframe at or
 *           before a timestamp (what a demuxer's index lookup does).
 * Build   : make run P=dsa/14-sorting-and-searching/cpp/03_binary_search_stl.cpp
 * Read    : ../README.md  (section "3.8 Binary search and its variants")
 */
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <set>
#include <vector>

// ---------------------------------------------------------------------------
// first_true: smallest x in [lo, hi) with pred(x) == true, or hi if none.
// Precondition: pred is monotone (false...false true...true) on [lo, hi).
// Half-open range + "answer stays in [lo, hi]" invariant = no off-by-one.
// ---------------------------------------------------------------------------
template <class Int, class Pred>
Int first_true(Int lo, Int hi, Pred pred)
{
    while (lo < hi) {
        Int mid = lo + (hi - lo) / 2;
        if (pred(mid)) hi = mid;
        else           lo = mid + 1;
    }
    return lo;
}

// Search on the answer #1: place k sensors on a line at given positions so
// that the MINIMUM gap between neighbours is as LARGE as possible.
// can_place(d) = "k sensors fit with every gap >= d" is T T T F F (monotone
// the other way), so the answer is first_true(!can_place) - 1.
static bool can_place(const std::vector<int> &pos, int k, int d)
{
    int placed = 1, last = pos[0];
    for (size_t i = 1; i < pos.size() && placed < k; ++i)
        if (pos[i] - last >= d) { ++placed; last = pos[i]; }   // greedy: earliest spot
    return placed >= k;
}
static int max_min_gap(std::vector<int> pos, int k)
{
    std::sort(pos.begin(), pos.end());
    int hi = pos.back() - pos.front() + 1;
    return first_true(1, hi + 1, [&](int d) { return !can_place(pos, k, d); }) - 1;
}

// Search on the answer #2 (real numbers): fixed iteration count instead of
// an epsilon test — 100 halvings shrink any double interval below 1 ulp.
static double cube_root(double x)
{
    double lo = 0, hi = std::max(1.0, x);
    for (int it = 0; it < 100; ++it) {
        double mid = (lo + hi) / 2;
        (mid * mid * mid < x ? lo : hi) = mid;
    }
    return lo;
}

int main()
{
    std::cout << "=== Binary search, the STL way ===\n\n";
    const std::vector<int> a{1, 2, 4, 4, 4, 7, 9};

    auto lb = std::lower_bound(a.begin(), a.end(), 4);      // first >= 4
    auto ub = std::upper_bound(a.begin(), a.end(), 4);      // first >  4
    auto [eq_first, eq_last] = std::equal_range(a.begin(), a.end(), 4);
    std::cout << "a = [1 2 4 4 4 7 9]\n"
              << "  lower_bound(4) -> index " << (lb - a.begin()) << '\n'
              << "  upper_bound(4) -> index " << (ub - a.begin()) << '\n'
              << "  equal_range(4) -> [" << (eq_first - a.begin()) << ", " << (eq_last - a.begin())
              << ")  count = " << (eq_last - eq_first) << '\n'
              << "  binary_search(5) -> " << std::boolalpha << std::binary_search(a.begin(), a.end(), 5) << '\n';
    assert(lb - a.begin() == 2 && ub - a.begin() == 5 && eq_last - eq_first == 3);
    assert(!std::binary_search(a.begin(), a.end(), 5));

    // lower_bound IS partition_point with predicate "x < value"
    auto pp = std::partition_point(a.begin(), a.end(), [](int x) { return x < 4; });
    assert(pp == lb);

    // ---- random cross-check against linear scans --------------------------
    std::mt19937 rng(11);
    for (int t = 0; t < 2000; ++t) {
        std::vector<int> v(rng() % 200);
        for (int &x : v) x = static_cast<int>(rng() % 40);
        std::sort(v.begin(), v.end());
        int x = static_cast<int>(rng() % 44) - 2;
        auto lin_lb = std::find_if(v.begin(), v.end(), [&](int y) { return y >= x; });
        auto lin_ub = std::find_if(v.begin(), v.end(), [&](int y) { return y > x; });
        assert(std::lower_bound(v.begin(), v.end(), x) == lin_lb);
        assert(std::upper_bound(v.begin(), v.end(), x) == lin_ub);
        size_t mine = first_true<size_t>(0, v.size(), [&](size_t i) { return v[i] >= x; });
        assert(mine == static_cast<size_t>(lin_lb - v.begin()));
    }
    std::cout << "  STL bounds and first_true == linear scan on 2000 random vectors\n";

    // ---- seeking: keyframe at or before a target timestamp ----------------
    // A demuxer keeps a sorted index of keyframe timestamps (ns). Seeking to
    // t means: decode from the LAST keyframe <= t = prev(upper_bound(t)).
    std::vector<int64_t> keyframes;
    for (int64_t ts = 0; ts < 60'000'000'000; ts += 2'000'000'000) keyframes.push_back(ts);  // every 2 s
    auto seek = [&](int64_t t) {
        auto it = std::upper_bound(keyframes.begin(), keyframes.end(), t);
        assert(it != keyframes.begin());          // t >= first keyframe
        return *std::prev(it);
    };
    std::cout << "\nSeek in a 60 s stream with a keyframe every 2 s:\n"
              << "  seek(13.7 s) -> start decoding at keyframe " << seek(13'700'000'000) / 1e9 << " s\n"
              << "  seek(14.0 s) -> " << seek(14'000'000'000) / 1e9 << " s (exact hit)\n";
    assert(seek(13'700'000'000) == 12'000'000'000);
    assert(seek(14'000'000'000) == 14'000'000'000);

    // ---- std::set: use the MEMBER lower_bound ------------------------------
    // std::lower_bound(set.begin(), set.end(), x) compiles, but set
    // iterators are not random-access, so std::distance/advance are O(n):
    // the "binary search" walks the list -> O(n). set::lower_bound walks the
    // tree -> O(log n).
    std::set<int> s{10, 20, 30, 40};
    assert(*s.lower_bound(25) == 30);
    std::cout << "\nstd::set: s.lower_bound(25) = 30  (O(log n); std::lower_bound on a set is O(n))\n";

    // ---- search on the answer ---------------------------------------------
    std::vector<int> spots{1, 2, 8, 4, 9};
    int best = max_min_gap(spots, 3);
    std::cout << "\nPlace 3 sensors at {1,2,4,8,9}, maximise the smallest gap -> " << best << '\n';
    assert(best == 3);   // 1, 4, 8 (or 1, 4, 9)
    for (int t = 0; t < 300; ++t) {                        // brute force: try every d
        std::vector<int> p(2 + rng() % 12);
        for (int &x : p) x = static_cast<int>(rng() % 100);
        int k = 2 + static_cast<int>(rng() % (p.size() - 1));
        auto sp = p; std::sort(sp.begin(), sp.end());
        int brute = 0;
        for (int d = 1; d <= 100; ++d) if (can_place(sp, k, d)) brute = d;
        assert(max_min_gap(p, k) == brute);
    }
    std::cout << "  matches brute force on 300 random cases\n";

    double c = cube_root(27.0);
    std::cout << "\ncube_root(27) by bisection = " << c << '\n';
    assert(std::fabs(c - 3.0) < 1e-12);
    assert(std::fabs(cube_root(2.0) - std::cbrt(2.0)) < 1e-12);

    // ---- rotated array via partition_point --------------------------------
    std::vector<int> rot{40, 50, 60, 10, 20, 30};
    auto pivot = std::partition_point(rot.begin(), rot.end(), [&](int x) { return x > rot.back(); });
    std::cout << "\nRotated [40 50 60 10 20 30]: minimum at index " << (pivot - rot.begin()) << '\n';
    assert(pivot - rot.begin() == 3);
    auto find_rot = [&](int x) -> long {
        auto first = (x <= rot.back()) ? pivot : rot.begin();
        auto last  = (x <= rot.back()) ? rot.end() : pivot;
        auto it = std::lower_bound(first, last, x);
        return (it != last && *it == x) ? it - rot.begin() : -1;
    };
    assert(find_rot(20) == 4 && find_rot(50) == 1 && find_rot(35) == -1);

    std::cout << "\nAll binary search checks passed.\n";
    return 0;
}
