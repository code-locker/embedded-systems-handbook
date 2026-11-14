/*
 * 01_segment_tree.cpp — generic iterative (bottom-up) segment tree over any monoid
 *
 * Concept : A segment tree works for ANY associative "combine" with an
 *           identity (a monoid): +/0, min/+inf, max/-inf, gcd/0, matrix
 *           multiply/I... One template covers them all. This version is the
 *           compact iterative form: leaves live at t[n..2n-1], node i's
 *           children are 2i and 2i+1, and both update and query are short
 *           loops — no recursion, 2n memory instead of 4n.
 * Build   : make run P=dsa/12-advanced-trees/cpp/01_segment_tree.cpp
 * Read    : ../README.md  (section "Segment tree")
 */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>
#include <random>
#include <vector>

/*
 * Bottom-up layout for n = 8 (t[0] unused):
 *
 *                    t[1]
 *            t[2]              t[3]
 *        t[4]    t[5]      t[6]    t[7]
 *       t8 t9  t10 t11   t12 t13  t14 t15      <- leaves = a[0..7]
 *
 * Query [l, r): start with l += n, r += n and move both ends UP.
 * If l is a RIGHT child (odd), its parent covers too much on the left, so
 * take t[l] now and step l right. Symmetric for r. O(log n) iterations.
 */
template <typename T, typename Op>
class SegmentTree {
public:
    // O(n) build: fill leaves then each parent once, right to left.
    SegmentTree(const std::vector<T>& a, T identity, Op op = Op{})
        : n_(a.size()), id_(identity), op_(op), t_(2 * a.size(), identity)
    {
        std::copy(a.begin(), a.end(), t_.begin() + static_cast<std::ptrdiff_t>(n_));
        for (std::size_t i = n_ - 1; i >= 1; --i)
            t_[i] = op_(t_[2 * i], t_[2 * i + 1]);
    }

    // O(log n): set the leaf, then recompute every ancestor.
    void set(std::size_t pos, T value)
    {
        std::size_t i = pos + n_;
        t_[i] = value;
        for (i /= 2; i >= 1; i /= 2)
            t_[i] = op_(t_[2 * i], t_[2 * i + 1]);
    }

    // O(log n) query of the half-open range [l, r).
    // Left and right partial results are kept apart so non-commutative ops
    // (e.g. matrix product, string concat) still combine in the right order.
    T query(std::size_t l, std::size_t r) const
    {
        T left = id_, right = id_;
        for (l += n_, r += n_; l < r; l /= 2, r /= 2) {
            if (l & 1) left = op_(left, t_[l++]);
            if (r & 1) right = op_(t_[--r], right);
        }
        return op_(left, right);
    }

private:
    std::size_t n_;
    T id_;
    Op op_;
    std::vector<T> t_; // RAII: vector owns all nodes, nothing to free by hand
};

// Function objects for the "combine" step (std::plus<long> is used for sums).
struct MinOp { int operator()(int a, int b) const { return std::min(a, b); } };
struct GcdOp { long operator()(long a, long b) const { return std::gcd(a, b); } };

int main()
{
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> val(-1000, 1000);

    const std::size_t N = 777; // not a power of two on purpose: bottom-up form handles any n
    std::vector<long> a(N);
    std::vector<int> b(N);
    std::vector<long> g(N);
    for (std::size_t i = 0; i < N; ++i) {
        a[i] = val(rng);
        b[i] = val(rng);
        g[i] = 6 * (1 + std::abs(val(rng)) % 50);
    }

    SegmentTree<long, std::plus<long>> sum(a, 0L);
    SegmentTree<int, MinOp> mn(b, std::numeric_limits<int>::max());
    SegmentTree<long, GcdOp> gcd(g, 0L);

    std::printf("sum [0,777) = %ld, min [0,777) = %d, gcd [0,777) = %ld\n",
                sum.query(0, N), mn.query(0, N), gcd.query(0, N));

    std::uniform_int_distribution<std::size_t> idx(0, N - 1);
    for (int op = 0; op < 20000; ++op) {
        std::size_t l = idx(rng), r = idx(rng);
        if (l > r) std::swap(l, r);
        ++r; // half-open
        if (op % 2) {
            std::size_t p = idx(rng);
            a[p] = val(rng);
            b[p] = val(rng);
            g[p] = 6 * (1 + std::abs(val(rng)) % 50);
            sum.set(p, a[p]);
            mn.set(p, b[p]);
            gcd.set(p, g[p]);
        } else {
            // STL reference implementations, O(n) each
            auto L = static_cast<std::ptrdiff_t>(l), R = static_cast<std::ptrdiff_t>(r);
            assert(sum.query(l, r) == std::accumulate(a.begin() + L, a.begin() + R, 0L));
            assert(mn.query(l, r) == *std::min_element(b.begin() + L, b.begin() + R));
            long gg = 0;
            for (std::size_t i = l; i < r; ++i) gg = std::gcd(gg, g[i]);
            assert(gcd.query(l, r) == gg);
        }
    }
    std::printf("random test: 20000 ops on sum/min/gcd trees match STL brute force\n");
    std::printf("OK\n");
    return 0;
}
