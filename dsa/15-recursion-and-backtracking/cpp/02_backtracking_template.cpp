/*
 * 02_backtracking_template.cpp — one generic backtracking skeleton, reused for
 *                                subsets, permutations, combinations and
 *                                permutations with duplicates
 *
 * Concept : Every backtracking problem answers four questions:
 *              is_solution(state)?   -> record it
 *              candidates(state)     -> what can I choose next?
 *              apply / undo          -> choose / un-choose
 *           A tiny class per problem plugs into one template function.
 *           Results are checked against the STL: std::next_permutation and
 *           bitmask enumeration.
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/02_backtracking_template.cpp
 * Read    : ../README.md  (section "Backtracking template")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <set>
#include <vector>

using Vec = std::vector<int>;

// ---------------------------------------------------------------------------
// The skeleton. Problem must provide:
//   bool done() const;              void record();
//   Vec  choices() const;           void choose(int);  void unchoose(int);
// ---------------------------------------------------------------------------
template <typename Problem>
void backtrack(Problem& p) {
    if (p.done()) { p.record(); return; }      // leaf (or every node, for subsets)
    for (int c : p.choices()) {
        p.choose(c);                           // CHOOSE
        backtrack(p);                          // EXPLORE
        p.unchoose(c);                         // UN-CHOOSE
    }
}

// --- subsets: choices are "indices after the last one taken" (so {a,b} is
//     produced but never {b,a}). Unlike the others, EVERY node of the tree is
//     a subset, not just the leaves, so done() is always false and we record
//     the current path when the node lists its choices.
//
//                 {}
//          a/     b|     \c
//        {a}     {b}     {c}
//       b/ \c     |c
//    {a,b} {a,c} {b,c}
//      |c
//   {a,b,c}                    8 nodes = 8 subsets
struct Subsets {
    Vec items, path;
    std::vector<Vec> out;
    int start = 0;
    std::vector<int> starts;                   // stack of `start` to undo
    bool done() const { return false; }        // never a leaf by itself
    void record() {}
    Vec choices() {
        out.push_back(path);                   // every node IS a subset
        Vec c;
        for (int i = start; i < int(items.size()); ++i) c.push_back(i);
        return c;
    }
    void choose(int i)   { path.push_back(items[i]); starts.push_back(start); start = i + 1; }
    void unchoose(int)   { path.pop_back(); start = starts.back(); starts.pop_back(); }
};

// --- permutations with a used[] mask
struct Permutations {
    Vec items, path;
    std::vector<bool> used;
    std::vector<Vec> out;
    bool done() const { return path.size() == items.size(); }
    void record() { out.push_back(path); }
    Vec choices() const {
        Vec c;
        for (int i = 0; i < int(items.size()); ++i) if (!used[i]) c.push_back(i);
        return c;
    }
    void choose(int i)   { used[i] = true;  path.push_back(items[i]); }
    void unchoose(int i) { used[i] = false; path.pop_back(); }
};

// --- permutations of a multiset (duplicates!), e.g. {1,1,2} -> 3 not 6.
//     Rule: items sorted; skip i if items[i]==items[i-1] and i-1 is NOT used.
//     That forces equal values to be placed in index order, so each distinct
//     arrangement is produced exactly once.
struct UniquePermutations : Permutations {
    Vec choices() const {
        Vec c;
        for (int i = 0; i < int(items.size()); ++i) {
            if (used[i]) continue;
            if (i > 0 && items[i] == items[i - 1] && !used[i - 1]) continue;  // dedupe
            c.push_back(i);
        }
        return c;
    }
};

// --- combinations C(n,k) of 1..n, with the "enough numbers left" prune
struct Combinations {
    int n = 0, k = 0;
    Vec path;
    std::vector<Vec> out;
    bool done() const { return int(path.size()) == k; }
    void record() { out.push_back(path); }
    Vec choices() const {
        Vec c;
        const int from = path.empty() ? 1 : path.back() + 1;
        const int need = k - int(path.size());
        for (int i = from; i <= n - need + 1; ++i) c.push_back(i);   // PRUNE
        return c;
    }
    void choose(int i)   { path.push_back(i); }
    void unchoose(int)   { path.pop_back(); }
};

static long binom(int n, int k) { long r = 1; for (int i = 1; i <= k; ++i) r = r * (n - k + i) / i; return r; }

int main() {
    for (int n = 0; n <= 7; ++n) {
        Vec items(n);
        for (int i = 0; i < n; ++i) items[i] = 10 + i;

        // subsets vs bitmask enumeration
        Subsets s;
        s.items = items;
        backtrack(s);
        assert(s.out.size() == (1u << n));
        std::set<Vec> a(s.out.begin(), s.out.end()), b;
        for (unsigned m = 0; m < (1u << n); ++m) {
            Vec v;
            for (int i = 0; i < n; ++i) if (m >> i & 1) v.push_back(items[i]);
            b.insert(v);
        }
        assert(a == b);

        // permutations vs std::next_permutation (same lexicographic order!)
        Permutations p;
        p.items = items;
        p.used.assign(n, false);
        backtrack(p);
        Vec ref = items;
        std::size_t idx = 0;
        do { assert(p.out.at(idx++) == ref); } while (std::next_permutation(ref.begin(), ref.end()));
        assert(idx == p.out.size());

        // combinations
        for (int k = 0; k <= n; ++k) {
            Combinations c;
            c.n = n; c.k = k;
            backtrack(c);
            assert(long(c.out.size()) == binom(n, k));
            assert(std::is_sorted(c.out.begin(), c.out.end()));      // lexicographic
        }
    }
    std::cout << "subsets == bitmasks, permutations == std::next_permutation, "
                 "|combinations| == C(n,k)  for n = 0..7\n";

    // multiset permutations
    const std::vector<Vec> multisets = { {1, 1, 2}, {1, 1, 1}, {1, 2, 2, 3, 3}, {4, 4, 5, 5, 5, 6} };
    for (const Vec& ms : multisets) {
        UniquePermutations u;
        u.items = ms;
        std::sort(u.items.begin(), u.items.end());
        u.used.assign(ms.size(), false);
        backtrack(u);
        std::set<Vec> distinct(u.out.begin(), u.out.end());
        assert(distinct.size() == u.out.size());                    // no duplicates
        Vec ref = u.items;
        std::size_t cnt = 0;
        do ++cnt; while (std::next_permutation(ref.begin(), ref.end()));  // handles dups
        assert(cnt == u.out.size());
        std::cout << "unique permutations of {";
        for (std::size_t i = 0; i < ms.size(); ++i) std::cout << (i ? "," : "") << ms[i];
        std::cout << "} = " << u.out.size() << '\n';
    }

    Combinations c;
    c.n = 4; c.k = 2;
    backtrack(c);
    std::cout << "C(4,2):";
    for (const auto& v : c.out) std::cout << " {" << v[0] << ',' << v[1] << '}';
    std::cout << "\n\nall backtracking-template checks passed\n";
    return 0;
}
