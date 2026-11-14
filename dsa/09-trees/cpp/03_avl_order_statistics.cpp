/*
 * 03_avl_order_statistics.cpp — AVL set template with unique_ptr rotations,
 *                               augmented with subtree sizes (k-th / rank)
 *
 * Concept : Rotations are just ownership moves between unique_ptr slots. An
 *           AVL node can carry extra "augmented" data computed from its
 *           children (here: subtree size) as long as every rotation recomputes
 *           it bottom-up. That turns the set into an ORDER-STATISTICS tree:
 *           k-th smallest and rank(x) in O(log n) - which std::set cannot do
 *           (std::distance on a set iterator is O(n)). Checked against
 *           std::set after every operation, with all invariants verified.
 * Build   : make run P=dsa/09-trees/cpp/03_avl_order_statistics.cpp
 * Read    : ../README.md  (sections "AVL tree" and "Augmenting a tree")
 */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <vector>

template <class K, class Compare = std::less<K>>
class AvlSet {
    struct Node {
        K key;
        int height = 1;      // in nodes: leaf = 1
        size_t size = 1;     // nodes in this subtree (the augmentation)
        std::unique_ptr<Node> left, right;
        explicit Node(K k) : key(std::move(k)) {}
    };
    using Ptr = std::unique_ptr<Node>;

public:
    size_t size() const { return sz(root_); }
    int height() const { return ht(root_); }

    bool insert(const K& key)                     // O(log n)
    {
        bool inserted = false;
        insert(root_, key, inserted);
        return inserted;
    }
    bool erase(const K& key)                      // O(log n)
    {
        bool erased = false;
        erase(root_, key, erased);
        return erased;
    }
    bool contains(const K& key) const             // O(log n)
    {
        const Node* n = root_.get();
        while (n) {
            if (less_(key, n->key)) n = n->left.get();
            else if (less_(n->key, key)) n = n->right.get();
            else return true;
        }
        return false;
    }

    // k-th smallest, 0-based. O(log n): at each node the left subtree size
    // tells us whether the answer is left, here, or right.
    //
    //            m (size 7)        kth(4): left size = 3
    //          /     \             4 > 3  -> skip left (3) and m (1)
    //    (size 3)   (size 3)               -> kth(4-3-1 = 0) in right subtree
    std::optional<K> kth(size_t k) const
    {
        const Node* n = root_.get();
        while (n) {
            size_t ls = sz(n->left);
            if (k < ls) n = n->left.get();
            else if (k == ls) return n->key;
            else { k -= ls + 1; n = n->right.get(); }
        }
        return std::nullopt;
    }

    // number of keys strictly less than key. O(log n).
    size_t rank(const K& key) const
    {
        size_t r = 0;
        const Node* n = root_.get();
        while (n) {
            if (less_(n->key, key)) { r += sz(n->left) + 1; n = n->right.get(); }
            else n = n->left.get();
        }
        return r;
    }

    // Verify BST order, stored heights, stored sizes, AVL balance. O(n).
    void check() const { check(root_.get(), nullptr, nullptr); }

    template <class F> void inorder(F&& f) const { inorder(root_.get(), f); }

    static inline long rotations = 0;             // C++17 inline static member

private:
    static int ht(const Ptr& p) { return p ? p->height : 0; }
    static size_t sz(const Ptr& p) { return p ? p->size : 0; }
    static void pull(Node& n)                     // recompute augmentation, O(1)
    {
        n.height = 1 + std::max(ht(n.left), ht(n.right));
        n.size = 1 + sz(n.left) + sz(n.right);
    }
    static int bal(const Ptr& p) { return ht(p->left) - ht(p->right); }

    //   rotate_right(slot):     slot -> y               slot -> x
    //                                  / \                    / \     .
    //                                 x   C       ->         A   y
    //                                / \                        / \   .
    //                               A   B                      B   C
    // Ownership moves: x out of y->left, B from x to y, y into x->right,
    // x into the slot. No node is allocated or freed. O(1).
    static void rotate_right(Ptr& slot)
    {
        Ptr x = std::move(slot->left);
        slot->left = std::move(x->right);
        pull(*slot);                              // y is lower now: first
        x->right = std::move(slot);
        pull(*x);
        slot = std::move(x);
        ++rotations;
    }
    static void rotate_left(Ptr& slot)
    {
        Ptr y = std::move(slot->right);
        slot->right = std::move(y->left);
        pull(*slot);
        y->left = std::move(slot);
        pull(*y);
        slot = std::move(y);
        ++rotations;
    }
    // LL / RR: one rotation; LR / RL: rotate the child first (see README).
    static void rebalance(Ptr& t)
    {
        pull(*t);
        int b = bal(t);
        if (b > 1) {
            if (bal(t->left) < 0) rotate_left(t->left);     // LR -> LL
            rotate_right(t);
        } else if (b < -1) {
            if (bal(t->right) > 0) rotate_right(t->right);  // RL -> RR
            rotate_left(t);
        }
    }

    void insert(Ptr& t, const K& key, bool& inserted)
    {
        if (!t) { t = std::make_unique<Node>(key); inserted = true; return; }
        if (less_(key, t->key)) insert(t->left, key, inserted);
        else if (less_(t->key, key)) insert(t->right, key, inserted);
        else return;                              // duplicate: no rebalance needed
        rebalance(t);
    }

    // Detach and return the minimum node of subtree t (t is non-null).
    static Ptr take_min(Ptr& t)
    {
        if (!t->left) {
            Ptr m = std::move(t);
            t = std::move(m->right);
            return m;
        }
        Ptr m = take_min(t->left);
        rebalance(t);
        return m;
    }

    void erase(Ptr& t, const K& key, bool& erased)
    {
        if (!t) return;
        if (less_(key, t->key)) erase(t->left, key, erased);
        else if (less_(t->key, key)) erase(t->right, key, erased);
        else {
            erased = true;
            if (!t->left || !t->right) {
                Ptr child = std::move(t->left ? t->left : t->right);
                t = std::move(child);             // frees the old node
                if (!t) return;
            } else {
                Ptr succ = take_min(t->right);    // relink successor in place
                succ->left = std::move(t->left);
                succ->right = std::move(t->right);
                t = std::move(succ);              // frees the old node
            }
        }
        rebalance(t);
    }

    template <class F> static void inorder(const Node* n, F& f)
    {
        if (!n) return;
        inorder(n->left.get(), f);
        f(n->key);
        inorder(n->right.get(), f);
    }

    void check(const Node* n, const K* lo, const K* hi) const
    {
        if (!n) return;
        assert(!lo || less_(*lo, n->key));
        assert(!hi || less_(n->key, *hi));
        check(n->left.get(), lo, &n->key);
        check(n->right.get(), &n->key, hi);
        assert(n->height == 1 + std::max(ht(n->left), ht(n->right)));
        assert(n->size == 1 + sz(n->left) + sz(n->right));
        int b = ht(n->left) - ht(n->right);
        assert(b >= -1 && b <= 1);
    }

    Ptr root_;
    Compare less_{};
};

int main()
{
    // Sorted input stays balanced.
    AvlSet<int> s;
    for (int i = 0; i < 100000; ++i) s.insert(i);
    s.check();
    std::printf("100000 sorted inserts -> height %d (log2 = 16.6), rotations %ld\n",
                s.height(), AvlSet<int>::rotations);
    assert(s.height() <= 18);
    assert(*s.kth(0) == 0 && *s.kth(54321) == 54321 && !s.kth(100000));
    assert(s.rank(500) == 500 && s.rank(-5) == 0 && s.rank(1 << 30) == 100000);

    // Median of a stream: kth(n/2) in O(log n) (compare: two heaps, lesson 10).
    AvlSet<int, std::greater<int>> desc;          // custom order: descending
    for (int v : {5, 1, 9, 3, 7}) desc.insert(v);
    std::vector<int> order;
    desc.inorder([&order](int v) { order.push_back(v); });
    assert((order == std::vector<int>{9, 7, 5, 3, 1}));
    assert(*desc.kth(0) == 9);                    // "smallest" under greater<>
    std::printf("descending AvlSet: 9 7 5 3 1, kth(0) = %d\n", *desc.kth(0));

    // Randomised against std::set; every invariant checked after every op.
    std::mt19937 rng(11);
    AvlSet<int> t;
    std::set<int> ref;
    for (int op = 0; op < 12000; ++op) {
        int k = static_cast<int>(rng() % 2000);
        if (rng() % 2) assert(t.insert(k) == ref.insert(k).second);
        else           assert(t.erase(k) == (ref.erase(k) == 1));
        t.check();
        assert(t.size() == ref.size());
        int q = static_cast<int>(rng() % 2000);
        assert(t.contains(q) == (ref.count(q) == 1));
        assert(t.rank(q) == static_cast<size_t>(std::distance(ref.begin(), ref.lower_bound(q))));
        if (!ref.empty()) {
            size_t idx = rng() % ref.size();
            assert(*t.kth(idx) == *std::next(ref.begin(), static_cast<long>(idx)));
        }
    }
    std::printf("random: %zu keys, height %d - all invariants held after every op\n",
                t.size(), t.height());
    std::printf("all AVL order-statistics checks passed\n");
    return 0;
}
