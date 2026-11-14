/*
 * 03_radix_tree.cpp — compressed trie (radix tree) with std::string edges
 *
 * Concept : C++ version of c/04_radix_tree.c. Edges carry whole strings,
 *           children are a std::map keyed by the FIRST char of their label
 *           (unique among siblings by construction), ownership is unique_ptr.
 *           Insert splits an edge, erase merges a lonely node into its child.
 *           We count nodes against an uncompressed trie and verify everything
 *           against std::set<std::string>, checking the invariant as we go.
 * Build   : make run P=dsa/11-tries/cpp/03_radix_tree.cpp
 * Read    : ../README.md  (section "Compressed trie / radix tree")
 */
#include <cassert>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

class RadixTree {
    struct Node {
        std::string label; // edge INTO this node
        bool end = false;
        std::map<char, std::unique_ptr<Node>> kid; // key = kid->label[0]
    };
    Node root_; // label "" by definition
    std::size_t size_ = 0;

    static std::size_t common(std::string_view a, std::string_view b)
    {
        std::size_t i = 0;
        while (i < a.size() && i < b.size() && a[i] == b[i])
            ++i;
        return i;
    }

    /*
     * Merge a non-word node with its only child (restores the invariant):
     *     [ ]--"ab"-->[ ]--"cd"-->[*]   ==>   [ ]--"abcd"-->[*]
     * We MOVE the child's guts into n; the child's unique_ptr then destroys
     * an empty shell. Note: we must take ownership of the child BEFORE
     * overwriting n.kid, or we'd destroy the child while reading it.
     */
    static void merge(Node& n)
    {
        std::unique_ptr<Node> c = std::move(n.kid.begin()->second);
        n.label += c->label;
        n.end = c->end;
        n.kid = std::move(c->kid);
    } // c (now empty) freed here

    // returns true if found & erased; fixes up the child it descended into
    bool erase_rec(Node& parent, std::string_view w)
    {
        auto it = parent.kid.find(w.front());
        if (it == parent.kid.end())
            return false;
        Node& c = *it->second;
        if (w.substr(0, c.label.size()) != c.label)
            return false;
        w.remove_prefix(c.label.size());
        if (w.empty()) {
            if (!c.end)
                return false;
            c.end = false;
        } else if (!erase_rec(c, w)) {
            return false;
        }
        if (!c.end && c.kid.empty())
            parent.kid.erase(it); // unique_ptr frees c
        else if (!c.end && c.kid.size() == 1)
            merge(c);
        return true;
    }

    template <class F>
    static void dfs(const Node& n, std::string& path, F& f)
    {
        path += n.label;
        if (n.end)
            f(path);
        for (const auto& kv : n.kid)
            dfs(*kv.second, path, f);
        path.resize(path.size() - n.label.size());
    }

    static std::size_t count(const Node& n)
    {
        std::size_t c = 1;
        for (const auto& kv : n.kid)
            c += count(*kv.second);
        return c;
    }

    static void check(const Node& n, bool is_root)
    {
        if (!is_root)
            assert(!n.label.empty() && (n.end || n.kid.size() >= 2));
        for (const auto& [ch, k] : n.kid) {
            assert(k->label.front() == ch);
            check(*k, false);
        }
    }

    static void print(const Node& n, int depth)
    {
        for (const auto& kv : n.kid) {
            std::cout << std::string(static_cast<std::size_t>(depth) * 2 + 2, ' ') << '"'
                      << kv.second->label << '"' << (kv.second->end ? " *" : "") << '\n';
            print(*kv.second, depth + 1);
        }
    }

public:
    // O(L): walk, then possibly split one edge.
    bool insert(std::string_view w)
    {
        Node* cur = &root_;
        for (;;) {
            if (w.empty()) {
                bool fresh = !cur->end;
                cur->end = true;
                size_ += fresh;
                return fresh;
            }
            auto it = cur->kid.find(w.front());
            if (it == cur->kid.end()) { // no edge starts with w[0]: new leaf
                auto leaf = std::make_unique<Node>();
                leaf->label = std::string(w);
                leaf->end = true;
                cur->kid.emplace(w.front(), std::move(leaf));
                ++size_;
                return true;
            }
            Node& c = *it->second;
            std::size_t k = common(c.label, w);
            if (k == c.label.size()) { // whole edge matches: descend
                cur = &c;
                w.remove_prefix(k);
                continue;
            }
            /*
             * Split at k:   cur --"romane"--> c
             *   becomes     cur --"rom"--> mid --"ane"--> c
             *                                  \--"ulus"--> leaf (if w continues)
             */
            auto mid = std::make_unique<Node>();
            mid->label = c.label.substr(0, k);
            std::unique_ptr<Node> old = std::move(it->second);
            old->label.erase(0, k);
            char oldkey = old->label.front();
            mid->kid.emplace(oldkey, std::move(old));
            w.remove_prefix(k);
            if (w.empty()) {
                mid->end = true;
            } else {
                auto leaf = std::make_unique<Node>();
                leaf->label = std::string(w);
                leaf->end = true;
                mid->kid.emplace(w.front(), std::move(leaf));
            }
            it->second = std::move(mid);
            ++size_;
            return true;
        }
    }

    bool contains(std::string_view w) const // O(L)
    {
        const Node* cur = &root_;
        while (!w.empty()) {
            auto it = cur->kid.find(w.front());
            if (it == cur->kid.end())
                return false;
            const std::string& lab = it->second->label;
            if (w.substr(0, lab.size()) != lab)
                return false;
            w.remove_prefix(lab.size());
            cur = it->second.get();
        }
        return cur->end;
    }

    bool erase(std::string_view w) // O(L)
    {
        bool ok;
        if (w.empty()) {
            ok = root_.end;
            root_.end = false;
        } else {
            ok = erase_rec(root_, w);
        }
        size_ -= ok;
        return ok;
    }

    std::vector<std::string> keys() const // sorted
    {
        std::vector<std::string> out;
        std::string path;
        auto f = [&](const std::string& s) { out.push_back(s); };
        dfs(root_, path, f);
        return out;
    }

    std::size_t size() const { return size_; }
    std::size_t node_count() const { return count(root_); }
    void check_invariant() const { check(root_, true); }
    void print() const { print(root_, 0); }
};

// How many nodes an UNcompressed trie would need: 1 + number of distinct
// non-empty prefixes of all keys.
static std::size_t plain_trie_nodes(const std::set<std::string>& keys)
{
    std::set<std::string> prefixes;
    for (const auto& k : keys)
        for (std::size_t i = 1; i <= k.size(); ++i)
            prefixes.insert(k.substr(0, i));
    return 1 + prefixes.size();
}

int main()
{
    RadixTree rt;
    // Long shared prefixes: exactly where compression shines (think file paths
    // or sysfs names).
    const std::set<std::string> paths = {
        "/sys/class/gpio/export",       "/sys/class/gpio/unexport",
        "/sys/class/leds/beaglebone:green:usr0", "/sys/class/leds/beaglebone:green:usr1",
        "/sys/bus/platform/drivers",    "/proc/cpuinfo", "/proc/meminfo"};
    for (const auto& p : paths)
        rt.insert(p);
    std::cout << "radix tree of some sysfs/proc paths:\n";
    rt.print();
    std::cout << "nodes: radix " << rt.node_count() << " vs plain trie "
              << plain_trie_nodes(paths) << '\n';
    rt.check_invariant();
    assert(rt.node_count() < plain_trie_nodes(paths) / 5);
    assert(rt.contains("/proc/cpuinfo") && !rt.contains("/proc/cpu"));
    auto k = rt.keys();
    assert(std::set<std::string>(k.begin(), k.end()) == paths);

    // ---- randomised check vs std::set ----
    std::mt19937 rng(99);
    std::uniform_int_distribution<int> len(0, 7), ch(0, 2), op(0, 2);
    RadixTree r;
    std::set<std::string> ref;
    for (int step = 0; step < 20000; ++step) {
        std::string w;
        for (int i = len(rng); i > 0; --i)
            w.push_back(static_cast<char>('x' + ch(rng)));
        switch (op(rng)) { // note: the empty string is a legal key too
        case 0: assert(r.insert(w) == ref.insert(w).second); break;
        case 1: assert(r.erase(w) == (ref.erase(w) == 1)); break;
        default: assert(r.contains(w) == (ref.count(w) == 1));
        }
        assert(r.size() == ref.size());
        if (step % 200 == 0) {
            r.check_invariant();
            assert(r.keys() == std::vector<std::string>(ref.begin(), ref.end()));
        }
    }
    std::cout << "randomised check: 20000 ops vs std::set, invariant held; final "
              << ref.size() << " keys in " << r.node_count() << " nodes (plain trie "
              << plain_trie_nodes(ref) << ")\n";
    return 0;
}
