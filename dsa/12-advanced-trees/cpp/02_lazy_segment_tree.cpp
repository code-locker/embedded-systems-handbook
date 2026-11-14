/*
 * 02_lazy_segment_tree.cpp — lazy segment tree: range add + range sum + range min together
 *
 * Concept : Each node stores an aggregate (sum and min of its slice) and a
 *           pending tag (an add its children have not seen yet). A range
 *           update stops at fully-covered nodes and tags them; any later
 *           operation that must go below a tagged node first push()es the
 *           tag down. Both operations stay O(log n).
 *           Rule to remember: "apply to a node = fix its aggregate NOW,
 *           remember the tag for its children LATER".
 * Build   : make run P=dsa/12-advanced-trees/cpp/02_lazy_segment_tree.cpp
 * Read    : ../README.md  (section "Lazy propagation")
 */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <limits>
#include <random>
#include <utility>
#include <vector>

class LazySegTree {
public:
    explicit LazySegTree(const std::vector<long long>& a) : n_(static_cast<int>(a.size())), node_(4 * a.size())
    {
        build(1, 0, n_ - 1, a);
    }

    void add(int l, int r, long long v) { add(1, 0, n_ - 1, l, r, v); }   // O(log n)
    long long sum(int l, int r) { return query(1, 0, n_ - 1, l, r).sum; } // O(log n)
    long long min(int l, int r) { return query(1, 0, n_ - 1, l, r).mn; }  // O(log n)

private:
    struct Agg {
        long long sum = 0;
        long long mn = std::numeric_limits<long long>::max(); // identity for min
    };
    struct Node {
        Agg agg;
        long long tag = 0; // pending add for the children
    };

    static Agg combine(const Agg& a, const Agg& b) { return { a.sum + b.sum, std::min(a.mn, b.mn) }; }

    // O(1): "+v to every element of this node's slice".
    // sum grows by v * length; min grows by v (every element shifts by v).
    void apply(int i, int lo, int hi, long long v)
    {
        node_[i].agg.sum += v * (hi - lo + 1);
        node_[i].agg.mn += v;
        node_[i].tag += v;
    }

    void push(int i, int lo, int hi)
    {
        if (node_[i].tag == 0) return;
        int mid = lo + (hi - lo) / 2;
        apply(2 * i, lo, mid, node_[i].tag);
        apply(2 * i + 1, mid + 1, hi, node_[i].tag);
        node_[i].tag = 0;
    }

    void build(int i, int lo, int hi, const std::vector<long long>& a) // O(n)
    {
        if (lo == hi) {
            node_[i].agg = { a[static_cast<std::size_t>(lo)], a[static_cast<std::size_t>(lo)] };
            return;
        }
        int mid = lo + (hi - lo) / 2;
        build(2 * i, lo, mid, a);
        build(2 * i + 1, mid + 1, hi, a);
        node_[i].agg = combine(node_[2 * i].agg, node_[2 * i + 1].agg);
    }

    void add(int i, int lo, int hi, int l, int r, long long v)
    {
        if (r < lo || hi < l) return;
        if (l <= lo && hi <= r) { apply(i, lo, hi, v); return; }
        push(i, lo, hi);
        int mid = lo + (hi - lo) / 2;
        add(2 * i, lo, mid, l, r, v);
        add(2 * i + 1, mid + 1, hi, l, r, v);
        node_[i].agg = combine(node_[2 * i].agg, node_[2 * i + 1].agg);
    }

    Agg query(int i, int lo, int hi, int l, int r)
    {
        if (r < lo || hi < l) return Agg{};
        if (l <= lo && hi <= r) return node_[i].agg;
        push(i, lo, hi);
        int mid = lo + (hi - lo) / 2;
        return combine(query(2 * i, lo, mid, l, r), query(2 * i + 1, mid + 1, hi, l, r));
    }

    int n_;
    std::vector<Node> node_; // RAII storage, 4n nodes
};

int main()
{
    std::mt19937 rng(7);
    const int N = 600;
    std::vector<long long> ref(N);
    std::uniform_int_distribution<long long> v(-500, 500);
    for (auto& x : ref) x = v(rng);
    LazySegTree t(ref);

    std::uniform_int_distribution<int> idx(0, N - 1);
    for (int op = 0; op < 20000; ++op) {
        int l = idx(rng), r = idx(rng);
        if (l > r) std::swap(l, r);
        if (op % 2) {
            long long d = v(rng);
            for (int i = l; i <= r; ++i) ref[static_cast<std::size_t>(i)] += d; // O(n) reference
            t.add(l, r, d);
        } else {
            auto b = ref.begin() + l, e = ref.begin() + r + 1;
            long long s = 0;
            for (auto it = b; it != e; ++it) s += *it;
            assert(t.sum(l, r) == s);
            assert(t.min(l, r) == *std::min_element(b, e));
        }
    }
    std::printf("random test: 20000 range-add / range-sum / range-min ops match brute force\n");
    std::printf("OK\n");
    return 0;
}
