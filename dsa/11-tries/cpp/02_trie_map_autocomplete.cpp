/*
 * 02_trie_map_autocomplete.cpp — map-of-children trie + top-k autocomplete
 *
 * Concept : Children kept in std::map<char, unique_ptr<Node>>: memory grows
 *           only with the children that exist, any char is allowed, and the
 *           map is ORDERED so a DFS still yields sorted words. Child lookup
 *           costs O(log sigma) instead of O(1). On top we build autocomplete:
 *           frequency per word, k best completions selected with a size-k
 *           min-heap (std::priority_queue) in O(m log k).
 *           Verified against std::map<std::string,int> with random data.
 * Build   : make run P=dsa/11-tries/cpp/02_trie_map_autocomplete.cpp
 * Read    : ../README.md  (sections "Node layouts" and "Autocomplete")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class AutoComplete {
    struct Node {
        std::map<char, std::unique_ptr<Node>> kid; // ordered => sorted DFS
        int freq = 0;                               // 0 = not a word
    };
    Node root_;

    const Node* walk(std::string_view p) const // O(P log sigma)
    {
        const Node* cur = &root_;
        for (char c : p) {
            auto it = cur->kid.find(c);
            if (it == cur->kid.end())
                return nullptr;
            cur = it->second.get();
        }
        return cur;
    }

    template <class F>
    static void dfs(const Node& n, std::string& path, F& f) // O(subtree)
    {
        if (n.freq > 0)
            f(path, n.freq);
        for (const auto& [c, child] : n.kid) { // map iterates in key order
            path.push_back(c);
            dfs(*child, path, f);
            path.pop_back();
        }
    }

public:
    using Hit = std::pair<std::string, int>; // word, frequency

    void add(std::string_view w, int times = 1) // O(L log sigma)
    {
        Node* cur = &root_;
        for (char c : w) {
            auto& slot = cur->kid[c]; // operator[] inserts an empty unique_ptr
            if (!slot)
                slot = std::make_unique<Node>();
            cur = slot.get();
        }
        cur->freq += times;
    }

    std::vector<Hit> all(std::string_view prefix) const // sorted by word
    {
        std::vector<Hit> out;
        if (const Node* n = walk(prefix)) {
            std::string path(prefix);
            auto f = [&](const std::string& s, int fr) { out.emplace_back(s, fr); };
            dfs(*n, path, f);
        }
        return out;
    }

    /*
     * top-k: keep a MIN-heap of the best k seen so far. The heap top is the
     * WORST of the current best k, so a new candidate only has to beat it.
     *
     *   candidates: git(50) gcc(40) gzip(5) grep(45), k = 2
     *   push git      heap {git}
     *   push gcc      heap {gcc, git}           (top = gcc, the worst)
     *   gzip(5)  < gcc(40)  -> skip
     *   grep(45) > gcc(40)  -> pop gcc, push grep -> {grep, git}
     *
     * O(P + S + m log k) instead of O(m log m) for a full sort.
     */
    std::vector<Hit> top_k(std::string_view prefix, std::size_t k) const
    {
        // "better" = higher freq, then alphabetically smaller.
        auto better = [](const Hit& a, const Hit& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        };
        // priority_queue puts the element that is "largest" per the comparator
        // on top; with `better` as comparator the top is the WORST hit.
        std::priority_queue<Hit, std::vector<Hit>, decltype(better)> heap(better);
        if (const Node* n = walk(prefix)) {
            std::string path(prefix);
            auto f = [&](const std::string& s, int fr) {
                Hit h{s, fr};
                if (heap.size() < k) {
                    heap.push(std::move(h));
                } else if (k > 0 && better(h, heap.top())) {
                    heap.pop();
                    heap.push(std::move(h));
                }
            };
            dfs(*n, path, f);
        }
        std::vector<Hit> out;
        while (!heap.empty()) {
            out.push_back(heap.top());
            heap.pop();
        }
        std::reverse(out.begin(), out.end()); // best first
        return out;
    }
};

int main()
{
    AutoComplete ac;
    // Shell history of a BeagleBone session, with counts.
    const std::vector<AutoComplete::Hit> hist = {
        {"git", 50},          {"gcc", 40},      {"gdb", 35},     {"grep", 45},
        {"gzip", 5},          {"gst-launch-1.0", 30}, {"gst-inspect-1.0", 12},
        {"make", 60},         {"modprobe", 22}, {"mount", 10},   {"dmesg", 70}};
    for (const auto& [w, f] : hist)
        ac.add(w, f);

    auto t = ac.top_k("g", 3);
    std::cout << "top-3 for 'g':";
    for (const auto& [w, f] : t)
        std::cout << ' ' << w << '(' << f << ')';
    std::cout << '\n';
    assert(t.size() == 3 && t[0].first == "git" && t[1].first == "grep" && t[2].first == "gcc");

    auto g = ac.all("gst-");
    std::cout << "all for 'gst-':";
    for (const auto& [w, f] : g)
        std::cout << ' ' << w;
    std::cout << '\n';
    assert(g.size() == 2 && g[0].first == "gst-inspect-1.0"); // '-' and '.' just work
    assert(ac.all("zz").empty() && ac.top_k("m", 0).empty());

    // ---- randomised check vs std::map<string,int> ----
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> len(1, 6), ch(0, 3), fr(1, 9), kk(0, 6);
    AutoComplete big;
    std::map<std::string, int> ref;
    for (int step = 0; step < 4000; ++step) {
        std::string w;
        for (int i = len(rng); i > 0; --i)
            w.push_back(static_cast<char>("ab-Z"[ch(rng)])); // mixed alphabet
        int f = fr(rng);
        big.add(w, f);
        ref[w] += f;
        if (step % 40 == 0) {
            std::string p = w.substr(0, 1 + step % 2);
            std::vector<AutoComplete::Hit> want;
            for (auto it = ref.lower_bound(p); it != ref.end() && it->first.rfind(p, 0) == 0; ++it)
                want.emplace_back(it->first, it->second);
            assert(big.all(p) == want); // same words, same order, same counts
            std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
            });
            std::size_t k = static_cast<std::size_t>(kk(rng));
            want.resize(std::min(k, want.size()));
            assert(big.top_k(p, k) == want);
        }
    }
    std::cout << "randomised check: completions and top-k match std::map (" << ref.size()
              << " words)\n";
    return 0;
}
