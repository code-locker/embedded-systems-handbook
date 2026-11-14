/*
 * 06_skip_list.cpp — skip list template with clear ownership, iteration and rank stats
 *
 * Concept : Same structure as c/05_skip_list.c, as a generic SkipList<K>.
 *           Ownership rule: level 0 OWNS each node (std::unique_ptr chain);
 *           the higher lanes are plain non-owning shortcuts. Destruction is
 *           done iteratively, because a 100 000-long unique_ptr chain would
 *           recurse 100 000 deep and overflow the stack.
 *           Also shows: ordered iteration and lower_bound (what Redis ZRANGE
 *           and LevelDB iterators need), checked against std::set.
 * Build   : make run P=dsa/12-advanced-trees/cpp/06_skip_list.cpp
 * Read    : ../README.md  (section "Skip list")
 */
#include <array>
#include <cassert>
#include <cstdio>
#include <iterator>
#include <memory>
#include <random>
#include <set>
#include <vector>

template <typename K, int MaxLevel = 20>
class SkipList {
    struct Node {
        explicit Node(const K& k, int lvl) : key(k), level(lvl) {}
        K key;
        int level;
        std::unique_ptr<Node> owner;          // == lane 0 "next": owns the successor
        std::array<Node*, MaxLevel> next{};   // next[0] mirrors owner.get(); lanes 1.. are views
    };

public:
    SkipList() : head_(std::make_unique<Node>(K{}, MaxLevel)) {}
    ~SkipList()
    {
        // Iterative teardown: detach each successor before its owner dies,
        // so no destructor ever recurses more than one level.
        std::unique_ptr<Node> cur = std::move(head_->owner);
        while (cur) cur = std::move(cur->owner);
    }
    SkipList(const SkipList&) = delete;            // owning structure: no accidental copies
    SkipList& operator=(const SkipList&) = delete;

    // Expected O(log n).
    bool contains(const K& k) const
    {
        const Node* x = descend(k, nullptr);
        x = x->next[0];
        return x && !(k < x->key);
    }

    // Expected O(log n). Returns false on duplicate.
    bool insert(const K& k)
    {
        std::array<Node*, MaxLevel> update{};
        Node* x = descend(k, &update);
        if (x->next[0] && !(k < x->next[0]->key)) return false;
        int lvl = random_level();
        if (lvl > level_) {
            for (int i = level_; i < lvl; ++i) update[static_cast<std::size_t>(i)] = head_.get();
            level_ = lvl;
        }
        auto n = std::make_unique<Node>(k, lvl);
        Node* raw = n.get();
        for (int i = 1; i < lvl; ++i) {                  // shortcut lanes: raw splice
            auto u = static_cast<std::size_t>(i);
            raw->next[u] = update[u]->next[u];
            update[u]->next[u] = raw;
        }
        Node* prev = update[0];                          // lane 0: ownership transfer
        raw->owner = std::move(prev->owner);             //   new node now owns the old successor
        raw->next[0] = raw->owner.get();
        prev->owner = std::move(n);                      //   predecessor now owns the new node
        prev->next[0] = raw;
        ++size_;
        return true;
    }

    // Expected O(log n).
    bool erase(const K& k)
    {
        std::array<Node*, MaxLevel> update{};
        descend(k, &update);
        Node* victim = update[0]->next[0];
        if (!victim || k < victim->key) return false;
        for (int i = 1; i < victim->level; ++i) {
            auto u = static_cast<std::size_t>(i);
            update[u]->next[u] = victim->next[u];
        }
        Node* prev = update[0];
        std::unique_ptr<Node> dying = std::move(prev->owner); // take ownership of the victim
        prev->owner = std::move(dying->owner);                // predecessor adopts victim's successor
        prev->next[0] = prev->owner.get();
        // `dying` goes out of scope here and frees exactly one node
        while (level_ > 1 && !head_->next[static_cast<std::size_t>(level_ - 1)]) --level_;
        --size_;
        return true;
    }

    // First key >= k, or nullptr. Expected O(log n).
    const K* lower_bound(const K& k) const
    {
        const Node* x = descend(k, nullptr)->next[0];
        return x ? &x->key : nullptr;
    }

    // In-order walk along lane 0: O(n), already sorted — no stack, no recursion.
    std::vector<K> to_vector() const
    {
        std::vector<K> out;
        for (const Node* x = head_->next[0]; x; x = x->next[0]) out.push_back(x->key);
        return out;
    }

    std::size_t size() const { return size_; }

private:
    // Walk from the top lane down; returns the last node < k on lane 0.
    // If update is given, records the last node < k on every lane.
    Node* descend(const K& k, std::array<Node*, MaxLevel>* update) const
    {
        Node* x = head_.get();
        for (int i = level_ - 1; i >= 0; --i) {
            auto u = static_cast<std::size_t>(i);
            while (x->next[u] && x->next[u]->key < k) x = x->next[u];
            if (update) (*update)[u] = x;
        }
        return x;
    }

    int random_level()
    {
        int lvl = 1;
        while (lvl < MaxLevel && coin_(rng_)) ++lvl; // p = 1/2 (Redis uses p = 1/4)
        return lvl;
    }

    std::unique_ptr<Node> head_;
    int level_ = 1;
    std::size_t size_ = 0;
    std::mt19937 rng_{2025};
    std::bernoulli_distribution coin_{0.5};
};

int main()
{
    SkipList<int> s;
    std::set<int> ref; // std::set is a red-black tree: same interface, different engine
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> key(0, 20000);
    for (int op = 0; op < 60000; ++op) {
        int k = key(rng);
        switch (op % 4) {
        case 0:
        case 1: assert(s.insert(k) == ref.insert(k).second); break;
        case 2: assert(s.erase(k) == (ref.erase(k) == 1)); break;
        default: {
            assert(s.contains(k) == (ref.count(k) == 1));
            const int* lb = s.lower_bound(k);
            auto it = ref.lower_bound(k);
            assert((lb == nullptr) == (it == ref.end()));
            if (lb) assert(*lb == *it);
        }
        }
    }
    assert(s.size() == ref.size());
    assert(s.to_vector() == std::vector<int>(ref.begin(), ref.end()));
    std::printf("random test: 60000 ops; contains/insert/erase/lower_bound/iteration match std::set (%zu keys)\n",
                s.size());

    // Long list: proves the iterative destructor (a recursive one would blow the stack under ASan).
    {
        SkipList<int> big;
        for (int i = 0; i < 200000; ++i) big.insert(i);
        std::printf("built and destroyed a 200000-node skip list without recursion\n");
    }
    std::printf("OK\n");
    return 0;
}
