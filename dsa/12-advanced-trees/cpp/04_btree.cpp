/*
 * 04_btree.cpp — B-tree template (key -> value) with unique_ptr ownership
 *
 * Concept : Same algorithm as c/04_btree.c (pre-emptive split on the way
 *           down), written as a generic map: BTree<Key, Value, T>. Children
 *           are std::unique_ptr, so destroying the root frees the whole tree
 *           with no hand-written free. Keys sit in a std::vector per node,
 *           and lookups inside a node use std::lower_bound (binary search),
 *           which is what a real database does with a 4 KiB page of keys.
 * Build   : make run P=dsa/12-advanced-trees/cpp/04_btree.cpp
 * Read    : ../README.md  (section "B-tree")
 */
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

template <typename Key, typename Value, int T = 4> // T = minimum degree: T-1 .. 2T-1 keys per node
class BTree {
    static_assert(T >= 2, "minimum degree must be at least 2");
    static constexpr std::size_t kMax = 2 * T - 1;

    struct Node {
        std::vector<std::pair<Key, Value>> kv;          // sorted by key
        std::vector<std::unique_ptr<Node>> child;        // empty for a leaf, else kv.size()+1
        bool leaf() const { return child.empty(); }
    };

public:
    // O(log_T n) node visits, O(log T) compares in each (binary search).
    const Value* find(const Key& k) const
    {
        const Node* x = root_.get();
        while (x) {
            auto it = lower(x, k);
            if (it != x->kv.end() && !(k < it->first)) return &it->second;
            if (x->leaf()) return nullptr;
            x = x->child[static_cast<std::size_t>(it - x->kv.begin())].get();
        }
        return nullptr;
    }

    // Insert or overwrite. O(T log_T n): one root-to-leaf pass, splits on the way.
    void insert(const Key& k, Value v)
    {
        if (!root_) root_ = std::make_unique<Node>();
        if (Value* existing = find_mut(k)) { *existing = std::move(v); return; }
        if (root_->kv.size() == kMax) {
            // Tree grows UPWARD: new root, old root becomes its only child, then split.
            auto s = std::make_unique<Node>();
            s->child.push_back(std::move(root_));
            root_ = std::move(s);
            split_child(*root_, 0);
        }
        Node* x = root_.get();
        while (!x->leaf()) {
            auto i = static_cast<std::size_t>(lower(x, k) - x->kv.begin());
            if (x->child[i]->kv.size() == kMax) {
                split_child(*x, i);
                if (x->kv[i].first < k) ++i;   // median moved up: choose the right half
            }
            x = x->child[i].get();
        }
        x->kv.insert(lower(x, k), { k, std::move(v) }); // O(T) shift inside the leaf
        ++size_;
    }

    // In-order visit of keys in [lo, hi]. O(log_T n + k). Prunes subtrees outside the range.
    template <typename F>
    void for_range(const Key& lo, const Key& hi, F&& f) const { visit(root_.get(), lo, hi, f); }

    std::size_t size() const { return size_; }
    int height() const { int h = 0; for (const Node* x = root_.get(); x; x = x->leaf() ? nullptr : x->child[0].get()) ++h; return h; }

    // Invariant check: key counts, sortedness, children count, equal leaf depth.
    void check() const { if (root_) { int d = -1; check(root_.get(), true, 0, d); } }

private:
    using It = typename std::vector<std::pair<Key, Value>>::const_iterator;
    static It lower(const Node* x, const Key& k)
    {
        return std::lower_bound(x->kv.begin(), x->kv.end(), k,
                                [](const std::pair<Key, Value>& p, const Key& key) { return p.first < key; });
    }

    Value* find_mut(const Key& k) { return const_cast<Value*>(static_cast<const BTree*>(this)->find(k)); }

    /*
     * x.child[i] is full (2T-1). Split around its median, O(T):
     *
     *   x: [ a  .  b ]              x: [ a  m  b ]
     *          |           ---->          /   \
     *   y: [ lo..  m  ..hi ]          y:[lo..] z:[..hi]
     */
    static void split_child(Node& x, std::size_t i)
    {
        Node& y = *x.child[i];
        auto z = std::make_unique<Node>();
        z->kv.assign(std::make_move_iterator(y.kv.begin() + T), std::make_move_iterator(y.kv.end()));
        if (!y.leaf()) {
            z->child.assign(std::make_move_iterator(y.child.begin() + T), std::make_move_iterator(y.child.end()));
            y.child.resize(T);
        }
        auto median = std::move(y.kv[T - 1]);
        y.kv.resize(T - 1);
        x.kv.insert(x.kv.begin() + static_cast<std::ptrdiff_t>(i), std::move(median));
        x.child.insert(x.child.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(z));
    }

    template <typename F>
    static void visit(const Node* x, const Key& lo, const Key& hi, F& f)
    {
        if (!x) return;
        for (std::size_t i = 0; i <= x->kv.size(); ++i) {
            bool left_ok = (i == 0) || x->kv[i - 1].first < hi;   // child i holds keys > kv[i-1]
            bool right_ok = (i == x->kv.size()) || lo < x->kv[i].first; // and keys < kv[i]
            if (!x->leaf() && left_ok && right_ok) visit(x->child[i].get(), lo, hi, f);
            if (i < x->kv.size() && !(x->kv[i].first < lo) && !(hi < x->kv[i].first))
                f(x->kv[i].first, x->kv[i].second);
        }
    }

    static void check(const Node* x, bool is_root, int depth, int& leaf_depth)
    {
        assert(x->kv.size() <= kMax);
        assert(is_root || x->kv.size() >= static_cast<std::size_t>(T - 1));
        assert(std::is_sorted(x->kv.begin(), x->kv.end(),
                              [](const auto& a, const auto& b) { return a.first < b.first; }));
        if (x->leaf()) {
            if (leaf_depth == -1) leaf_depth = depth;
            assert(leaf_depth == depth);
            return;
        }
        assert(x->child.size() == x->kv.size() + 1);
        for (std::size_t i = 0; i < x->child.size(); ++i) {
            const Node* c = x->child[i].get();
            if (i > 0) assert(x->kv[i - 1].first < c->kv.front().first);
            if (i < x->kv.size()) assert(c->kv.back().first < x->kv[i].first);
            check(c, false, depth + 1, leaf_depth);
        }
    }

    std::unique_ptr<Node> root_; // owns everything below it
    std::size_t size_ = 0;
};

int main()
{
    // A tiny "inode table": inode number -> file name.
    BTree<int, std::string, 2> small; // T=2 is the 2-3-4 tree (the ancestor of red-black trees)
    const char* names[] = { "init", "bash", "ls", "cat", "vim", "gcc", "make", "gst-launch" };
    for (int i = 0; i < 8; ++i) small.insert(i * 10, names[i]);
    assert(small.find(40) && *small.find(40) == "vim" && !small.find(41));
    std::printf("T=2 (2-3-4 tree): 8 keys, height %d; inode 40 -> %s\n", small.height(), small.find(40)->c_str());
    std::printf("range [15, 55]:");
    small.for_range(15, 55, [](int k, const std::string& v) { std::printf(" %d=%s", k, v.c_str()); });
    std::printf("\n");

    // Randomised: compare against std::map (a red-black tree) on the same operations.
    std::mt19937 rng(1234);
    std::uniform_int_distribution<int> key(0, 50000);
    BTree<int, int, 8> bt;
    std::map<int, int> ref;
    for (int op = 0; op < 60000; ++op) {
        int k = key(rng);
        if (op % 3) {
            bt.insert(k, op);
            ref[k] = op;
        } else {
            const int* got = bt.find(k);
            auto it = ref.find(k);
            assert((got != nullptr) == (it != ref.end()));
            if (got) assert(*got == it->second);
        }
    }
    bt.check();
    assert(bt.size() == ref.size());

    // range scan must equal std::map's lower_bound..upper_bound walk
    std::vector<int> a, b;
    bt.for_range(1000, 2000, [&](int k, int) { a.push_back(k); });
    for (auto it = ref.lower_bound(1000); it != ref.end() && it->first <= 2000; ++it) b.push_back(it->first);
    assert(a == b);
    std::printf("T=8: %zu keys, height %d (a binary tree would need >= %d levels)\n",
                bt.size(), bt.height(), 1 + static_cast<int>(std::log2(static_cast<double>(bt.size()))));
    std::printf("random test vs std::map: find, overwrite and range scan agree\n");
    std::printf("OK\n");
    return 0;
}
