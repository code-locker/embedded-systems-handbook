/*
 * 01_trie_unique_ptr.cpp — array-of-children trie with RAII ownership
 *
 * Concept : Same structure as c/01_trie_array.c, expressed in modern C++:
 *           each node owns its children through std::array<std::unique_ptr>,
 *           so destroying the root frees the whole tree — no trie_free().
 *           Templated on the alphabet (first char + size) so the same code
 *           handles 'a'..'z', '0'..'9' (phone numbers), etc.
 *           Verified against std::set<std::string> with random operations.
 * Build   : make run P=dsa/11-tries/cpp/01_trie_unique_ptr.cpp
 * Read    : ../README.md  (sections "How it works" and "C vs C++")
 */
#include <array>
#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

template <char First, std::size_t Alpha>
class Trie {
    struct Node {
        // unique_ptr: exactly one owner per child. Destroying a Node destroys
        // its children recursively (depth = word length, so no stack worry).
        std::array<std::unique_ptr<Node>, Alpha> kid{};
        bool end = false;
        std::size_t pass = 0; // words in this subtree (prefix count)
    };

    Node root_;

    static std::size_t index(char c)
    {
        auto i = static_cast<std::size_t>(c - First);
        assert(i < Alpha && "character outside the alphabet");
        return i;
    }

    // O(L): follow the path; nullptr if it breaks.
    const Node* walk(std::string_view s) const
    {
        const Node* cur = &root_;
        for (char c : s) {
            cur = cur->kid[index(c)].get();
            if (!cur)
                return nullptr;
        }
        return cur;
    }

    // Recursive erase; returns true if `n` became useless and can be dropped.
    static bool erase_rec(Node& n, std::string_view w)
    {
        --n.pass;
        if (w.empty()) {
            n.end = false;
        } else {
            auto& child = n.kid[index(w.front())];
            if (erase_rec(*child, w.substr(1)))
                child.reset(); // frees the child node right here — RAII
        }
        return n.pass == 0; // nothing below and not a word => prune
    }

    template <class F>
    static void dfs(const Node& n, std::string& path, F&& f)
    {
        if (n.end)
            f(path);
        for (std::size_t i = 0; i < Alpha; ++i)
            if (n.kid[i]) {
                path.push_back(static_cast<char>(First + static_cast<char>(i)));
                dfs(*n.kid[i], path, f);
                path.pop_back();
            }
    }

public:
    // O(L). Returns false if already present.
    bool insert(std::string_view w)
    {
        if (contains(w))
            return false;
        Node* cur = &root_;
        ++cur->pass;
        for (char c : w) {
            auto& slot = cur->kid[index(c)];
            if (!slot)
                slot = std::make_unique<Node>();
            cur = slot.get();
            ++cur->pass;
        }
        cur->end = true;
        return true;
    }

    bool contains(std::string_view w) const // O(L)
    {
        const Node* n = walk(w);
        return n && n->end;
    }

    std::size_t count_prefix(std::string_view p) const // O(P) thanks to `pass`
    {
        const Node* n = walk(p);
        return n ? n->pass : 0;
    }

    bool erase(std::string_view w) // O(L * Alpha) worst case
    {
        if (!contains(w))
            return false;
        // root is a member, never freed; only its children can be pruned
        erase_rec(root_, w);
        return true;
    }

    std::size_t size() const { return root_.pass; }

    // O(P + subtree): words with prefix p, in sorted order.
    std::vector<std::string> with_prefix(std::string_view p) const
    {
        std::vector<std::string> out;
        if (const Node* n = walk(p)) {
            std::string path(p);
            dfs(*n, path, [&](const std::string& s) { out.push_back(s); });
        }
        return out;
    }
};

// STL reference: all keys with prefix p are a CONTIGUOUS range of a sorted
// set, starting at lower_bound(p). O(log n + P*k).
static std::vector<std::string> set_prefix(const std::set<std::string>& s, const std::string& p)
{
    std::vector<std::string> out;
    for (auto it = s.lower_bound(p); it != s.end() && it->compare(0, p.size(), p) == 0; ++it)
        out.push_back(*it);
    return out;
}

int main()
{
    Trie<'a', 26> t;
    for (const char* w : {"car", "cart", "care", "cat", "dog", "do"})
        t.insert(w);
    assert(t.contains("car") && !t.contains("ca"));
    assert(t.count_prefix("car") == 3);
    auto v = t.with_prefix("ca");
    std::cout << "with_prefix(\"ca\"):";
    for (auto& s : v)
        std::cout << ' ' << s;
    std::cout << '\n';
    assert((v == std::vector<std::string>{"car", "care", "cart", "cat"}));

    // Different alphabet, same code: phone-number digits.
    Trie<'0', 10> phone;
    phone.insert("0049301234");
    phone.insert("0049891111");
    phone.insert("0033155555");
    std::cout << "numbers starting with 0049: " << phone.count_prefix("0049") << '\n';
    assert(phone.count_prefix("0049") == 2);

    // ---- randomised check vs std::set ----
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> len(1, 6), ch(0, 3), op(0, 3);
    Trie<'a', 26> tr;
    std::set<std::string> ref;
    for (int step = 0; step < 20000; ++step) {
        std::string w;
        for (int i = len(rng); i > 0; --i)
            w.push_back(static_cast<char>('a' + ch(rng)));
        switch (op(rng)) {
        case 0:
            assert(tr.insert(w) == ref.insert(w).second);
            break;
        case 1:
            assert(tr.erase(w) == (ref.erase(w) == 1));
            break;
        case 2:
            assert(tr.contains(w) == (ref.count(w) == 1));
            break;
        default: {
            std::string p = w.substr(0, 2);
            assert(tr.with_prefix(p) == set_prefix(ref, p));
            assert(tr.count_prefix(p) == set_prefix(ref, p).size());
        }
        }
        assert(tr.size() == ref.size());
    }
    std::cout << "randomised check: 20000 ops vs std::set OK (final size " << ref.size()
              << ")\n";
    return 0; // tr, t, phone destroyed here: every node freed by unique_ptr
}
