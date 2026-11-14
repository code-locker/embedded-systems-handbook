/*
 * 05_word_search_trie.cpp — Word Search I (one word) and Word Search II (many
 *                           words at once, pruned by a trie)
 *
 * Concept : Searching each of W words separately costs W full grid DFSs.
 *           Instead put all words in a trie and run ONE DFS per start cell:
 *           the trie node tells us immediately whether the current path is a
 *           prefix of ANY word. If not, prune. That is "pruning by prefix" —
 *           exactly what Boggle solvers and autocomplete engines do.
 *           Self-check: trie result == running Word Search I for every word.
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/05_word_search_trie.cpp
 * Read    : ../README.md  (sections "Word search", "Pruning")
 */
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

using Board = std::vector<std::string>;

// ---------------------------------------------------------------------------
// Word Search I — one word, mark-in-place backtracking.
// ---------------------------------------------------------------------------
static bool dfs1(Board& b, int r, int c, const std::string& w, std::size_t k) {
    if (k == w.size()) return true;
    if (r < 0 || c < 0 || r >= int(b.size()) || c >= int(b[0].size()) || b[r][c] != w[k])
        return false;
    const char saved = b[r][c];
    b[r][c] = '#';                                                  // choose
    const bool ok = dfs1(b, r + 1, c, w, k + 1) || dfs1(b, r - 1, c, w, k + 1) ||
                    dfs1(b, r, c + 1, w, k + 1) || dfs1(b, r, c - 1, w, k + 1);
    b[r][c] = saved;                                                // un-choose
    return ok;
}

static bool exist(Board b, const std::string& w) {     // by value: we mutate a copy
    for (int r = 0; r < int(b.size()); ++r)
        for (int c = 0; c < int(b[0].size()); ++c)
            if (dfs1(b, r, c, w, 0)) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Word Search II — trie-guided DFS.
//
//   words: OATH, OAT, EAT        trie:   root
//                                       /    \    (O and E children)
//                                      O      E
//                                      |      |
//                                      A      A
//                                      |      |
//                                      T*     T*        * = end of a word
//                                      |
//                                      H*
//
//   A DFS path "O-X..." dies at once: root->O has no child 'X'.
// ---------------------------------------------------------------------------
struct TrieNode {
    std::array<std::unique_ptr<TrieNode>, 26> next{};   // owns children (RAII)
    const std::string* word = nullptr;                   // set at word end
    int live = 0;                                        // words still unfound below
};

class WordFinder {
public:
    explicit WordFinder(const std::vector<std::string>& words) {
        for (const auto& w : words) {
            TrieNode* n = &root_;
            ++n->live;
            for (char ch : w) {
                auto& slot = n->next[ch - 'A'];
                if (!slot) slot = std::make_unique<TrieNode>();
                n = slot.get();
                ++n->live;
            }
            if (n->word) {                 // duplicate word: undo the counts
                n = &root_; --n->live;
                for (char ch : w) { n = n->next[ch - 'A'].get(); --n->live; }
            } else {
                n->word = &w;              // points into caller's vector (outlives us)
            }
        }
    }

    std::set<std::string> find(Board b) {
        found_.clear();
        for (int r = 0; r < int(b.size()); ++r)
            for (int c = 0; c < int(b[0].size()); ++c) dfs(b, r, c, &root_);
        return found_;
    }
    long nodes() const { return nodes_; }

private:
    // Returns how many words were found in this subtree (to update `live`).
    int dfs(Board& b, int r, int c, TrieNode* parent) {
        if (r < 0 || c < 0 || r >= int(b.size()) || c >= int(b[0].size())) return 0;
        const char ch = b[r][c];
        if (ch == '#') return 0;
        TrieNode* n = parent->next[ch - 'A'].get();
        if (!n || n->live == 0) return 0;       // PRUNE: no remaining word has this prefix
        ++nodes_;
        int got = 0;
        if (n->word) { found_.insert(*n->word); n->word = nullptr; ++got; }  // report once
        b[r][c] = '#';                                                    // choose
        got += dfs(b, r + 1, c, n) + dfs(b, r - 1, c, n) +
               dfs(b, r, c + 1, n) + dfs(b, r, c - 1, n);         // explore
        b[r][c] = ch;                                                     // un-choose
        n->live -= got;              // found words no longer need this branch:
        return got;                  // later DFSs prune it entirely
    }

    TrieNode root_;
    std::set<std::string> found_;
    long nodes_ = 0;
};

int main() {
    const Board board = { "OAAN", "ETAE", "IHKR", "IFLV" };
    const std::vector<std::string> words = { "OATH", "PEA", "EAT", "RAIN", "OAT", "HIKE", "KEA", "FLIK", "TAKE" };

    std::cout << "board:\n";
    for (const auto& row : board) std::cout << "  " << row << '\n';

    std::set<std::string> expect;
    for (const auto& w : words) if (exist(board, w)) expect.insert(w);
    WordFinder wf(words);
    const auto got = wf.find(board);
    assert(got == expect);
    std::cout << "found:";
    for (const auto& w : got) std::cout << ' ' << w;
    std::cout << "\n(" << wf.nodes() << " trie-guided DFS nodes)\n";
    assert(got == (std::set<std::string>{ "EAT", "OAT", "OATH" }));

    // Randomised cross-check on a larger board with many words.
    std::mt19937 rng(15);
    for (int round = 0; round < 30; ++round) {
        Board b(5, std::string(5, 'A'));
        for (auto& row : b) for (char& ch : row) ch = char('A' + rng() % 5);   // small alphabet
        std::vector<std::string> ws;
        for (int i = 0; i < 60; ++i) {
            std::string w;
            const int len = 1 + int(rng() % 7);
            for (int j = 0; j < len; ++j) w += char('A' + rng() % 5);
            ws.push_back(w);
        }
        std::set<std::string> ref;
        for (const auto& w : ws) if (exist(b, w)) ref.insert(w);
        WordFinder f(ws);
        assert(f.find(b) == ref);
    }
    std::cout << "30 random boards x 60 words: trie search == per-word search\n";

    std::cout << "\nall word-search checks passed\n";
    return 0;
}
