/*
 * 03_fenwick_tree.cpp — Fenwick tree template + three classic tricks
 *
 * Concept : 1. Fenwick<T>: point add / prefix sum in O(log n) via lowbit.
 *           2. Range add + range sum with TWO Fenwicks (the "B1, B2" trick).
 *           3. find_kth: walk down powers of two to find the smallest i with
 *              prefix(i) >= k in O(log n) — an order-statistic set of small ints.
 *           4. Counting inversions in O(n log n) — a classic interview use.
 * Build   : make run P=dsa/12-advanced-trees/cpp/03_fenwick_tree.cpp
 * Read    : ../README.md  (section "Fenwick tree")
 */
#include <cassert>
#include <cstdio>
#include <iterator>
#include <utility>
#include <random>
#include <set>
#include <vector>

template <typename T>
class Fenwick {
public:
    explicit Fenwick(std::size_t n) : bit_(n + 1, T{}) {}

    std::size_t size() const { return bit_.size() - 1; }

    // a[i] += d, i is 1-based. O(log n): climb by adding the lowest set bit.
    void add(std::size_t i, T d)
    {
        for (; i < bit_.size(); i += lowbit(i)) bit_[i] += d;
    }

    // a[1] + ... + a[i]. O(log n): descend by stripping the lowest set bit.
    T prefix(std::size_t i) const
    {
        T s{};
        for (; i > 0; i -= lowbit(i)) s += bit_[i];
        return s;
    }

    T range(std::size_t l, std::size_t r) const { return prefix(r) - prefix(l - 1); }

    // Smallest i with prefix(i) >= k (all values must be >= 0). O(log n).
    // Binary lifting: try jumps of 2^j from the top; bit_[pos + 2^j] is exactly
    // the sum of the block (pos, pos + 2^j], so we can skip it in one step.
    std::size_t find_kth(T k) const
    {
        std::size_t pos = 0, step = 1;
        while (step * 2 <= size()) step *= 2;
        for (; step > 0; step /= 2) {
            if (pos + step <= size() && bit_[pos + step] < k) {
                pos += step;
                k -= bit_[pos];
            }
        }
        return pos + 1;
    }

private:
    static std::size_t lowbit(std::size_t i) { return i & (~i + 1); } // == i & -i, but unsigned-safe
    std::vector<T> bit_;
};

/*
 * Range add / range sum.  Keep two Fenwicks B1, B2 such that
 *     prefix_sum(i) = B1.prefix(i) * i - B2.prefix(i)
 * Adding v to [l, r]:
 *     B1: +v at l, -v at r+1             (a difference array of the slope)
 *     B2: +v*(l-1) at l, -v*r at r+1     (correction so prefix is exact)
 */
class RangeFenwick {
public:
    explicit RangeFenwick(std::size_t n) : b1_(n), b2_(n) {}
    void add(std::size_t l, std::size_t r, long long v) // O(log n)
    {
        auto L = static_cast<long long>(l), R = static_cast<long long>(r);
        b1_.add(l, v);
        b1_.add(r + 1, -v);
        b2_.add(l, v * (L - 1));
        b2_.add(r + 1, -v * R);
    }
    long long prefix(std::size_t i) const
    {
        return b1_.prefix(i) * static_cast<long long>(i) - b2_.prefix(i);
    }
    long long range(std::size_t l, std::size_t r) const { return prefix(r) - prefix(l - 1); }

private:
    Fenwick<long long> b1_, b2_; // b1_ has n+1 slots, so add(r+1) with r == n is a no-op: fine
};

// Inversions = pairs i < j with a[i] > a[j]. Scan right to left; for each
// value ask "how many already-seen values are smaller?". O(n log V).
static long long count_inversions(const std::vector<int>& a, int maxv)
{
    Fenwick<long long> seen(static_cast<std::size_t>(maxv));
    long long inv = 0;
    for (auto it = a.rbegin(); it != a.rend(); ++it) {
        inv += seen.prefix(static_cast<std::size_t>(*it - 1));
        seen.add(static_cast<std::size_t>(*it), 1);
    }
    return inv;
}

int main()
{
    std::mt19937 rng(99);

    // --- 1 + 2: range add / range sum vs brute force ---
    const std::size_t N = 500;
    RangeFenwick rf(N);
    std::vector<long long> ref(N + 1, 0);
    std::uniform_int_distribution<std::size_t> idx(1, N);
    std::uniform_int_distribution<long long> val(-100, 100);
    for (int op = 0; op < 20000; ++op) {
        std::size_t l = idx(rng), r = idx(rng);
        if (l > r) std::swap(l, r);
        if (op % 2) {
            long long v = val(rng);
            for (std::size_t i = l; i <= r; ++i) ref[i] += v;
            rf.add(l, r, v);
        } else {
            long long s = 0;
            for (std::size_t i = l; i <= r; ++i) s += ref[i];
            assert(rf.range(l, r) == s);
        }
    }
    std::printf("range-add/range-sum Fenwick: 20000 ops match brute force\n");

    // --- 3: find_kth as an order-statistic multiset of small ints vs std::multiset ---
    const std::size_t V = 1000;
    Fenwick<int> cnt(V);
    std::multiset<std::size_t> ms;
    std::uniform_int_distribution<std::size_t> vv(1, V);
    for (int op = 0; op < 20000; ++op) {
        if (ms.empty() || op % 3) {
            std::size_t x = vv(rng);
            cnt.add(x, 1);
            ms.insert(x);
        } else {
            std::uniform_int_distribution<std::size_t> kd(1, ms.size());
            std::size_t k = kd(rng);
            auto it = ms.begin();
            std::advance(it, static_cast<std::ptrdiff_t>(k - 1)); // O(k) reference
            assert(cnt.find_kth(static_cast<int>(k)) == *it);
            if (op % 2) { // erase it from both
                cnt.add(*it, -1);
                ms.erase(it);
            }
        }
    }
    std::printf("find_kth: matches std::multiset order statistics\n");

    // --- 4: inversions vs O(n^2) ---
    std::vector<int> a(2000);
    std::uniform_int_distribution<int> av(1, 300);
    for (auto& x : a) x = av(rng);
    long long brute = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = i + 1; j < a.size(); ++j)
            if (a[i] > a[j]) ++brute;
    long long fast = count_inversions(a, 300);
    std::printf("inversions: Fenwick %lld == brute force %lld\n", fast, brute);
    assert(fast == brute);
    std::printf("OK\n");
    return 0;
}
