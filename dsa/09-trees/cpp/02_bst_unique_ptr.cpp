/*
 * 02_bst_unique_ptr.cpp — a template BST map with std::unique_ptr ownership
 *
 * Concept : BstMap<K, V, Compare> owns its nodes through unique_ptr, so there
 *           is no delete anywhere and no leak is possible. Deletion of a node
 *           with two children RELINKS the successor node (ownership is moved
 *           with std::move) instead of copying keys, so references to other
 *           elements stay valid - the same guarantee std::map gives.
 *           200000 random operations are checked against std::map.
 * Build   : make run P=dsa/09-trees/cpp/02_bst_unique_ptr.cpp
 * Read    : ../README.md  (sections "Binary search tree" and "C vs C++")
 */
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

template <class K, class V, class Compare = std::less<K>>
class BstMap {
public:
    struct Node {
        K key;
        V value;
        std::unique_ptr<Node> left, right;   // ownership flows parent -> child
        Node(K k, V v) : key(std::move(k)), value(std::move(v)) {}
    };

    BstMap() = default;
    explicit BstMap(Compare c) : less_(std::move(c)) {}
    ~BstMap() { clear(); }
    BstMap(const BstMap&) = delete;              // deep copy not needed here
    BstMap& operator=(const BstMap&) = delete;
    BstMap(BstMap&&) noexcept = default;         // moving = moving the root
    BstMap& operator=(BstMap&&) noexcept = default;

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    // O(h). Returns false (and leaves the value alone) if key exists.
    bool insert(K key, V value)
    {
        std::unique_ptr<Node>* link = &root_;     // pointer to the owning slot
        while (*link) {
            if (less_(key, (*link)->key)) link = &(*link)->left;
            else if (less_((*link)->key, key)) link = &(*link)->right;
            else return false;
        }
        *link = std::make_unique<Node>(std::move(key), std::move(value));
        ++size_;
        return true;
    }

    // O(h). Only `less` is used: a == b is spelled !(a<b) && !(b<a)
    // ("equivalence"), exactly like the STL, so K needs no operator==.
    V* find(const K& key)
    {
        Node* n = root_.get();
        while (n) {
            if (less_(key, n->key)) n = n->left.get();
            else if (less_(n->key, key)) n = n->right.get();
            else return &n->value;
        }
        return nullptr;
    }
    const V* find(const K& key) const { return const_cast<BstMap*>(this)->find(key); }

    // O(h). Three cases; nodes are relinked, never copied.
    bool erase(const K& key)
    {
        std::unique_ptr<Node>* link = &root_;
        while (*link) {
            if (less_(key, (*link)->key)) link = &(*link)->left;
            else if (less_((*link)->key, key)) link = &(*link)->right;
            else break;
        }
        if (!*link) return false;
        Node* victim = link->get();
        if (victim->left && victim->right) {
            //  case 3:      [victim]                 [succ]
            //               /      \                 /    \     .
            //              L       ...      ->      L     ...
            //                      /                      /
            //                  [succ]                   succ's old
            //                      \                    right child
            //                      SR
            std::unique_ptr<Node>* slink = &victim->right;
            while ((*slink)->left) slink = &(*slink)->left;
            std::unique_ptr<Node> succ = std::move(*slink);  // detach successor
            *slink = std::move(succ->right);                 // its right child moves up
            succ->left = std::move(victim->left);            // adopt victim's children
            succ->right = std::move(victim->right);
            *link = std::move(succ);       // victim (now childless) is freed here
        } else {
            // case 1/2: move the only child (or nullptr) into the slot. Take it
            // into a local first: assigning *link destroys victim, and the child
            // pointer lives inside victim.
            std::unique_ptr<Node> child =
                std::move(victim->left ? victim->left : victim->right);
            *link = std::move(child);
        }
        --size_;
        return true;
    }

    // smallest key strictly greater than `key` (std::map::upper_bound). O(h)
    std::optional<K> successor(const K& key) const
    {
        const Node* n = root_.get();
        const Node* best = nullptr;
        while (n) {
            if (less_(key, n->key)) { best = n; n = n->left.get(); }
            else n = n->right.get();
        }
        return best ? std::optional<K>(best->key) : std::nullopt;
    }

    // smallest key >= key (std::map::lower_bound). O(h)
    std::optional<K> lower_bound(const K& key) const
    {
        const Node* n = root_.get();
        const Node* best = nullptr;
        while (n) {
            if (!less_(n->key, key)) { best = n; n = n->left.get(); }
            else n = n->right.get();
        }
        return best ? std::optional<K>(best->key) : std::nullopt;
    }

    std::optional<K> min() const
    {
        const Node* n = root_.get();
        if (!n) return std::nullopt;
        while (n->left) n = n->left.get();
        return n->key;
    }
    std::optional<K> max() const
    {
        const Node* n = root_.get();
        if (!n) return std::nullopt;
        while (n->right) n = n->right.get();
        return n->key;
    }

    // In-order visit; f(key, value). O(n) time, O(h) explicit stack.
    template <class F> void for_each(F&& f) const
    {
        std::vector<const Node*> st;
        const Node* n = root_.get();
        while (n || !st.empty()) {
            for (; n; n = n->left.get()) st.push_back(n);
            n = st.back();
            st.pop_back();
            f(n->key, n->value);
            n = n->right.get();
        }
    }

    // Count keys in [lo, hi): prunes subtrees outside the range,
    // O(h + number of keys reported).
    size_t count_range(const K& lo, const K& hi) const { return count_rec(root_.get(), lo, hi); }

    // BST invariant via (lo, hi) bounds; O(n).
    bool valid() const { return valid_rec(root_.get(), nullptr, nullptr) == size_; }

    int height() const { return height_rec(root_.get()); }

    // Iterative teardown: no recursion depth problem for degenerate trees.
    void clear()
    {
        while (root_) {
            if (root_->left) {                    // rotate right until no left
                std::unique_ptr<Node> l = std::move(root_->left);
                root_->left = std::move(l->right);
                l->right = std::move(root_);
                root_ = std::move(l);
            } else {
                root_ = std::move(root_->right);  // frees old root
            }
        }
        size_ = 0;
    }

private:
    size_t count_rec(const Node* n, const K& lo, const K& hi) const
    {
        if (!n) return 0;
        if (less_(n->key, lo)) return count_rec(n->right.get(), lo, hi);  // all left too small
        if (!less_(n->key, hi)) return count_rec(n->left.get(), lo, hi);  // all right too big
        return 1 + count_rec(n->left.get(), lo, hi) + count_rec(n->right.get(), lo, hi);
    }
    // returns node count if valid, or size_t(-1) on violation
    size_t valid_rec(const Node* n, const K* lo, const K* hi) const
    {
        if (!n) return 0;
        if (lo && !less_(*lo, n->key)) return size_t(-1);
        if (hi && !less_(n->key, *hi)) return size_t(-1);
        size_t l = valid_rec(n->left.get(), lo, &n->key);
        size_t r = valid_rec(n->right.get(), &n->key, hi);
        if (l == size_t(-1) || r == size_t(-1)) return size_t(-1);
        return 1 + l + r;
    }
    int height_rec(const Node* n) const
    {
        if (!n) return -1;
        return 1 + std::max(height_rec(n->left.get()), height_rec(n->right.get()));
    }

    std::unique_ptr<Node> root_;
    size_t size_ = 0;
    Compare less_{};
};

// A comparator object: case-insensitive string order.
struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const
    {
        size_t n = std::min(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) {
            int ca = std::tolower(static_cast<unsigned char>(a[i]));
            int cb = std::tolower(static_cast<unsigned char>(b[i]));
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    }
};

int main()
{
    // ---- small demo with strings -----------------------------------------
    BstMap<std::string, int, CaseInsensitiveLess> devs;
    for (auto& [name, irq] : std::vector<std::pair<std::string, int>>{
             {"mmc0", 32}, {"eth0", 45}, {"UART0", 20}, {"i2c1", 50}, {"gpu", 70}})
        assert(devs.insert(name, irq));
    assert(!devs.insert("Eth0", 99));                  // same key, case-insensitively
    assert(devs.find("ETH0") && *devs.find("ETH0") == 45);
    std::printf("devices in order:");
    devs.for_each([](const std::string& k, int v) { std::printf(" %s=%d", k.c_str(), v); });
    std::printf("\n");
    assert(devs.erase("uart0") && !devs.find("UART0") && devs.size() == 4);
    assert(devs.valid());

    // ---- the three delete cases on the classic example ---------------------
    BstMap<int, int> m;
    for (int k : {8, 3, 10, 1, 6, 14, 4, 7, 13}) m.insert(k, k * 10);
    int* seven = m.find(7);            // pointer to a value we keep using
    assert(m.erase(4));                // leaf
    assert(m.erase(10));               // one child
    assert(m.erase(3));                // two children: successor 6 relinked
    assert(m.erase(8));                // root with two children: successor 13 (via 14's left)
    assert(seven == m.find(7) && *seven == 70);   // still valid: nothing copied
    std::vector<int> keys;
    m.for_each([&keys](int k, int) { keys.push_back(k); });
    assert((keys == std::vector<int>{1, 6, 7, 13, 14}));
    assert(m.valid() && *m.min() == 1 && *m.max() == 14);
    std::printf("after deleting 4, 10, 3, 8:");
    for (int k : keys) std::printf(" %d", k);
    std::printf("  (pointer to 7's value survived)\n");

    // ---- randomised: BstMap vs std::map ------------------------------------
    std::mt19937 rng(2025);
    std::uniform_int_distribution<int> key(0, 1999);
    BstMap<int, int> t;
    std::map<int, int> ref;
    for (int op = 0; op < 200000; ++op) {
        int k = key(rng);
        switch (rng() % 4) {
        case 0: {
            bool ins = t.insert(k, op);
            bool rins = ref.emplace(k, op).second;
            assert(ins == rins);
            break;
        }
        case 1:
            assert(t.erase(k) == (ref.erase(k) == 1));
            break;
        case 2: {
            const int* v = t.find(k);
            auto it = ref.find(k);
            assert((v != nullptr) == (it != ref.end()));
            if (v) assert(*v == it->second);
            break;
        }
        default: {
            auto ub = ref.upper_bound(k);
            auto s = t.successor(k);
            assert(s.has_value() == (ub != ref.end()));
            if (s) assert(*s == ub->first);
            auto lb = ref.lower_bound(k);
            auto l = t.lower_bound(k);
            assert(l.has_value() == (lb != ref.end()));
            if (l) assert(*l == lb->first);
            int hi = k + 100;
            assert(t.count_range(k, hi) ==
                   static_cast<size_t>(std::distance(ref.lower_bound(k), ref.lower_bound(hi))));
        }
        }
        assert(t.size() == ref.size());
        if (op % 10000 == 0) {
            assert(t.valid());
            auto it = ref.begin();
            t.for_each([&it](int kk, int vv) {
                assert(kk == it->first && vv == it->second);
                ++it;
            });
            assert(it == ref.end());
        }
    }
    std::printf("random: %zu keys, height %d\n", t.size(), t.height());

    // ---- degenerate input: clear() must not recurse ------------------------
    BstMap<int, int> chain;
    for (int i = 0; i < 5000; ++i) chain.insert(i, i);   // O(n^2) total: a list!
    assert(chain.height() == 4999);
    chain.clear();
    assert(chain.empty());
    std::printf("5000 sorted inserts -> height 4999; cleared iteratively\n");

    std::printf("all BstMap checks passed (200000 ops vs std::map)\n");
    return 0;
}
