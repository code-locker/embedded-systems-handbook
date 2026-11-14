/*
 * 05_bplus_tree.cpp — B+ tree: insert, point lookup, and fast range scan via the leaf chain
 *
 * Concept : A B+ tree is a B-tree where ALL records live in the leaves.
 *           Internal nodes hold only copies of keys ("signposts"), so they are
 *           small and many fit in cache/RAM. Leaves are linked left-to-right,
 *           so a range query = one descent + a sequential walk. That is why
 *           databases (InnoDB, SQLite indexes, PostgreSQL nbtree) and
 *           filesystems (ext4 htree, XFS, Btrfs, NTFS) are built on it.
 * Build   : make run P=dsa/12-advanced-trees/cpp/05_bplus_tree.cpp
 * Read    : ../README.md  (section "B+ tree")
 */
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

/*
 *                        [ 30 | 60 ]                 internal: signposts only
 *                 /           |          \
 *        [10 | 20]       [40 | 50]       [70 | 80]
 *        /   |   \       /   |   \       /   |   \
 *      [1 5][10 15][20 25][30 35][40 45][50 55][60 65][70 75][80 90]
 *        --->  --->   --->  --->   --->  --->  --->  --->   (leaf chain)
 *
 * Rule: in an internal node, keys >= signpost go to the RIGHT of it.
 * Leaf split   : the right half's FIRST key is COPIED up (it stays in the leaf).
 * Internal split: the middle signpost is MOVED up (it leaves the node).
 */
template <typename Key, typename Value, std::size_t M = 4> // M = max keys per node
class BPlusTree {
    struct Node {
        bool leaf = true;
        std::vector<Key> keys;
        std::vector<Value> vals;                     // leaves only
        std::vector<std::unique_ptr<Node>> child;    // internal only: keys.size()+1
        Node* next = nullptr;                        // leaves only; NON-owning link
    };
    using Split = std::optional<std::pair<Key, std::unique_ptr<Node>>>; // (separator, new right node)

public:
    BPlusTree() : root_(std::make_unique<Node>()) {}

    // O(log_M n): descend to the one leaf that could hold k.
    std::optional<Value> find(const Key& k) const
    {
        const Node* x = leaf_for(k);
        auto it = std::lower_bound(x->keys.begin(), x->keys.end(), k);
        if (it != x->keys.end() && !(k < *it)) return x->vals[static_cast<std::size_t>(it - x->keys.begin())];
        return std::nullopt;
    }

    // O(M log_M n). Recursive insert; a full node splits on the way BACK UP
    // (the "bottom-up" style, the alternative to c/04_btree.c's top-down splits).
    void insert(const Key& k, const Value& v)
    {
        Split s = insert(root_.get(), k, v);
        if (s) { // root split: tree grows one level at the top
            auto r = std::make_unique<Node>();
            r->leaf = false;
            r->keys.push_back(s->first);
            r->child.push_back(std::move(root_));
            r->child.push_back(std::move(s->second));
            root_ = std::move(r);
        }
    }

    // Range scan [lo, hi]: O(log_M n + k). The leaf chain means we never go back up.
    template <typename F>
    void range(const Key& lo, const Key& hi, F&& f) const
    {
        for (const Node* x = leaf_for(lo); x; x = x->next)
            for (std::size_t i = 0; i < x->keys.size(); ++i) {
                if (hi < x->keys[i]) return;
                if (!(x->keys[i] < lo)) f(x->keys[i], x->vals[i]);
            }
    }

    std::size_t size() const { return size_; }
    int height() const { int h = 1; for (const Node* x = root_.get(); !x->leaf; x = x->child[0].get()) ++h; return h; }

    // Walk the leaf chain from the far left: must visit every key once, sorted.
    std::vector<Key> leaf_chain_keys() const
    {
        const Node* x = root_.get();
        while (!x->leaf) x = x->child[0].get();
        std::vector<Key> out;
        for (; x; x = x->next) out.insert(out.end(), x->keys.begin(), x->keys.end());
        return out;
    }

    void check() const { int d = -1; check(root_.get(), true, 0, d, nullptr, nullptr); }

private:
    const Node* leaf_for(const Key& k) const
    {
        const Node* x = root_.get();
        while (!x->leaf) {
            // upper_bound: equal keys go RIGHT of a signpost
            auto i = std::upper_bound(x->keys.begin(), x->keys.end(), k) - x->keys.begin();
            x = x->child[static_cast<std::size_t>(i)].get();
        }
        return x;
    }

    Split insert(Node* x, const Key& k, const Value& v)
    {
        if (x->leaf) {
            auto it = std::lower_bound(x->keys.begin(), x->keys.end(), k);
            auto pos = it - x->keys.begin();
            if (it != x->keys.end() && !(k < *it)) { x->vals[static_cast<std::size_t>(pos)] = v; return std::nullopt; }
            x->keys.insert(it, k);
            x->vals.insert(x->vals.begin() + pos, v);
            ++size_;
            if (x->keys.size() <= M) return std::nullopt;
            return split_leaf(x);
        }
        auto i = static_cast<std::size_t>(std::upper_bound(x->keys.begin(), x->keys.end(), k) - x->keys.begin());
        Split s = insert(x->child[i].get(), k, v);
        if (!s) return std::nullopt;
        // child i split: its separator and new right sibling slot in at i / i+1
        x->keys.insert(x->keys.begin() + static_cast<std::ptrdiff_t>(i), s->first);
        x->child.insert(x->child.begin() + static_cast<std::ptrdiff_t>(i) + 1, std::move(s->second));
        if (x->keys.size() <= M) return std::nullopt;
        return split_internal(x);
    }

    /*
     * Leaf with M+1 keys, e.g. M=4:  [10 20 30 40 50]
     *   left [10 20] -> right [30 40 50], separator 30 is COPIED up.
     *   Chain: left.next = right, right.next = old left.next.
     */
    static Split split_leaf(Node* x)
    {
        auto r = std::make_unique<Node>();
        std::size_t mid = x->keys.size() / 2;
        r->keys.assign(x->keys.begin() + static_cast<std::ptrdiff_t>(mid), x->keys.end());
        r->vals.assign(x->vals.begin() + static_cast<std::ptrdiff_t>(mid), x->vals.end());
        x->keys.resize(mid);
        x->vals.resize(mid);
        r->next = x->next;
        x->next = r.get();
        Key sep = r->keys.front();
        return std::make_pair(sep, std::move(r));
    }

    /*
     * Internal with M+1 keys, e.g. [10 20 30 40 50] and 6 children:
     *   left [10 20] (3 children)   30 MOVES up   right [40 50] (3 children)
     */
    static Split split_internal(Node* x)
    {
        auto r = std::make_unique<Node>();
        r->leaf = false;
        std::size_t mid = x->keys.size() / 2;
        Key up = x->keys[mid];
        r->keys.assign(x->keys.begin() + static_cast<std::ptrdiff_t>(mid) + 1, x->keys.end());
        r->child.assign(std::make_move_iterator(x->child.begin() + static_cast<std::ptrdiff_t>(mid) + 1),
                        std::make_move_iterator(x->child.end()));
        x->keys.resize(mid);
        x->child.resize(mid + 1);
        return std::make_pair(up, std::move(r));
    }

    // lo/hi: bounds from ancestors (lo <= every key < hi).
    static void check(const Node* x, bool is_root, int depth, int& leaf_depth, const Key* lo, const Key* hi)
    {
        assert(x->keys.size() <= M);
        assert(is_root || x->keys.size() >= M / 2);
        assert(std::is_sorted(x->keys.begin(), x->keys.end()));
        for (const Key& k : x->keys) {
            if (lo) assert(!(k < *lo));
            if (hi) assert(k < *hi);
        }
        if (x->leaf) {
            assert(x->vals.size() == x->keys.size());
            if (leaf_depth == -1) leaf_depth = depth;
            assert(leaf_depth == depth);
            return;
        }
        assert(x->child.size() == x->keys.size() + 1);
        for (std::size_t i = 0; i < x->child.size(); ++i)
            check(x->child[i].get(), false, depth + 1, leaf_depth,
                  i == 0 ? lo : &x->keys[i - 1], i == x->keys.size() ? hi : &x->keys[i]);
    }

    std::unique_ptr<Node> root_; // owns every node; leaf->next pointers are just views
    std::size_t size_ = 0;
};

int main()
{
    BPlusTree<int, int> t;
    for (int k : { 10, 20, 30, 40, 50, 60, 70, 80, 90, 5, 15, 25, 35 }) t.insert(k, k * 100);
    t.check();
    std::printf("13 keys, M=4: height %d; find(35) = %d\n", t.height(), *t.find(35));
    std::printf("range [12, 55] via leaf chain:");
    t.range(12, 55, [](int k, int) { std::printf(" %d", k); });
    std::printf("\n");
    assert(!t.find(36));

    // randomised vs std::map
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> key(0, 100000);
    BPlusTree<int, int, 16> big;
    std::map<int, int> ref;
    for (int op = 0; op < 50000; ++op) {
        int k = key(rng);
        big.insert(k, op);
        ref[k] = op;
        if (op % 7 == 0) {
            int q = key(rng);
            auto got = big.find(q);
            auto it = ref.find(q);
            assert(got.has_value() == (it != ref.end()));
            if (got) assert(*got == it->second);
        }
    }
    big.check();
    assert(big.size() == ref.size());
    std::vector<int> want;
    for (auto& kv : ref) want.push_back(kv.first);
    assert(big.leaf_chain_keys() == want); // the leaf chain IS the sorted order

    long long s1 = 0, s2 = 0;
    big.range(40000, 41000, [&](int, int v) { s1 += v; });
    for (auto it = ref.lower_bound(40000); it != ref.end() && it->first <= 41000; ++it) s2 += it->second;
    assert(s1 == s2);
    std::printf("M=16: %zu keys, height %d; leaf chain, lookups and range sums match std::map\n",
                big.size(), big.height());
    std::printf("OK\n");
    return 0;
}
