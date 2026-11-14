/*
 * 06_partition_and_combination_sum.cpp — three more backtracking patterns:
 *     palindrome partitioning, combination sum II (input has duplicates,
 *     each number used once), and generate parentheses with std::string
 *
 * Concept : Same choose/explore/un-choose loop; what changes is
 *             - WHAT a choice is (a cut position, a number, a bracket),
 *             - HOW duplicates are avoided (sort + "skip equal sibling"),
 *             - WHERE we prune (non-palindrome prefix, sum too big,
 *               close > open).
 *           Each result is checked against brute force over all bitmasks.
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/06_partition_and_combination_sum.cpp
 * Read    : ../README.md  (sections "Generate parentheses", "Pruning")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Palindrome partitioning: split s so every piece is a palindrome.
//    Choice at position `start`: where does the next piece end?
//
//    "aab":  start=0: "a" | "aa" | "aab"x(not palindrome -> PRUNE)
//              "a"  -> start=1: "a" | "ab"x
//                        "a" -> start=2: "b" -> start=3 = end -> [a,a,b]
//              "aa" -> start=2: "b" -> [aa,b]
// ---------------------------------------------------------------------------
static bool is_pal(const std::string& s, std::size_t i, std::size_t j) {   // [i, j]
    while (i < j) if (s[i++] != s[j--]) return false;
    return true;
}

static void partition(const std::string& s, std::size_t start,
                      std::vector<std::string>& cur, std::vector<std::vector<std::string>>& out) {
    if (start == s.size()) { out.push_back(cur); return; }
    for (std::size_t end = start; end < s.size(); ++end) {
        if (!is_pal(s, start, end)) continue;                  // prune
        cur.push_back(s.substr(start, end - start + 1));        // choose
        partition(s, end + 1, cur, out);                        // explore
        cur.pop_back();                                         // un-choose
    }
}

// Brute force: every one of the 2^(n-1) ways to place cuts, keep the valid ones.
static std::set<std::vector<std::string>> partition_brute(const std::string& s) {
    std::set<std::vector<std::string>> res;
    const std::size_t n = s.size();
    if (n == 0) {                       // one way to split "": no pieces
        res.insert(std::vector<std::string>{});   // NB: insert({}) would pick the
        return res;                               // initializer_list overload = no-op!
    }
    for (unsigned m = 0; m < (1u << (n - 1)); ++m) {           // bit i = cut after i
        std::vector<std::string> parts;
        std::size_t from = 0;
        bool ok = true;
        for (std::size_t i = 0; i < n; ++i)
            if (i == n - 1 || (m >> i & 1)) {
                ok = ok && is_pal(s, from, i);
                parts.push_back(s.substr(from, i - from + 1));
                from = i + 1;
            }
        if (ok) res.insert(parts);
    }
    return res;
}

// ---------------------------------------------------------------------------
// 2. Combination sum II: choose a sub-multiset of `cand` summing to target.
//    Duplicates in the input: [1,1,2], target 3 -> {1,2} only ONCE.
//    Rule: sorted input; at one tree level, skip a value equal to the
//    previous SIBLING (i > start && a[i] == a[i-1]). Equal values can still
//    appear together along one path (going deeper), just not twice as
//    alternative choices at the same level.
// ---------------------------------------------------------------------------
static void comb2(const std::vector<int>& a, std::size_t start, int remaining,
                  std::vector<int>& cur, std::vector<std::vector<int>>& out, long& nodes) {
    ++nodes;
    if (remaining == 0) { out.push_back(cur); return; }
    for (std::size_t i = start; i < a.size(); ++i) {
        if (i > start && a[i] == a[i - 1]) continue;           // dedupe siblings
        if (a[i] > remaining) break;                           // sorted -> prune rest
        cur.push_back(a[i]);
        comb2(a, i + 1, remaining - a[i], cur, out, nodes);    // i+1: use once
        cur.pop_back();
    }
}

static std::set<std::vector<int>> comb2_brute(const std::vector<int>& a, int target) {
    std::set<std::vector<int>> res;                            // set removes duplicates
    for (unsigned m = 0; m < (1u << a.size()); ++m) {
        std::vector<int> v;
        int sum = 0;
        for (std::size_t i = 0; i < a.size(); ++i) if (m >> i & 1) { v.push_back(a[i]); sum += a[i]; }
        if (sum == target) res.insert(v);                      // a sorted -> v sorted
    }
    return res;
}

// ---------------------------------------------------------------------------
// 3. Generate parentheses with std::string push/pop.
// ---------------------------------------------------------------------------
static void parens(int n, int open, int close, std::string& cur, std::vector<std::string>& out) {
    if (int(cur.size()) == 2 * n) { out.push_back(cur); return; }
    if (open < n)     { cur.push_back('('); parens(n, open + 1, close, cur, out); cur.pop_back(); }
    if (close < open) { cur.push_back(')'); parens(n, open, close + 1, cur, out); cur.pop_back(); }
}

int main() {
    std::cout << "== palindrome partitioning ==\n";
    for (const std::string s : { "aab", "racecar", "aaaa", "abc", "", "abbab", "noonabbad" }) {
        std::vector<std::string> cur;
        std::vector<std::vector<std::string>> out;
        partition(s, 0, cur, out);
        const std::set<std::vector<std::string>> got(out.begin(), out.end());
        assert(got.size() == out.size());                      // no duplicates
        assert(got == partition_brute(s));
        std::cout << "  \"" << s << "\": " << out.size() << " partitions";
        if (s == "aab") {
            for (const auto& p : out) {
                std::cout << "  [";
                for (std::size_t i = 0; i < p.size(); ++i) std::cout << (i ? "," : "") << p[i];
                std::cout << ']';
            }
        }
        std::cout << '\n';
    }

    std::cout << "\n== combination sum II ==\n";
    {
        std::vector<int> a = { 10, 1, 2, 7, 6, 1, 5 };
        std::sort(a.begin(), a.end());
        std::vector<int> cur;
        std::vector<std::vector<int>> out;
        long nodes = 0;
        comb2(a, 0, 8, cur, out, nodes);
        std::cout << "  [10,1,2,7,6,1,5] target 8:";
        for (const auto& v : out) {
            std::cout << " {";
            for (std::size_t i = 0; i < v.size(); ++i) std::cout << (i ? "," : "") << v[i];
            std::cout << '}';
        }
        std::cout << "  (" << nodes << " nodes)\n";
        assert(std::set<std::vector<int>>(out.begin(), out.end()) == comb2_brute(a, 8));
        assert(out.size() == 4);                                // {1,1,6} {1,2,5} {1,7} {2,6}
    }
    std::mt19937 rng(6);
    for (int t = 0; t < 300; ++t) {
        std::vector<int> a(1 + rng() % 12);
        for (int& x : a) x = 1 + int(rng() % 6);               // many duplicates
        std::sort(a.begin(), a.end());
        const int target = 1 + int(rng() % 20);
        std::vector<int> cur;
        std::vector<std::vector<int>> out;
        long nodes = 0;
        comb2(a, 0, target, cur, out, nodes);
        const std::set<std::vector<int>> got(out.begin(), out.end());
        assert(got.size() == out.size());                      // dedupe rule worked
        assert(got == comb2_brute(a, target));
    }
    std::cout << "  300 random multisets: backtracking == brute force, no duplicates\n";

    std::cout << "\n== generate parentheses ==\n";
    const long catalan[] = { 1, 1, 2, 5, 14, 42, 132, 429, 1430 };
    for (int n = 0; n <= 8; ++n) {
        std::string cur;
        std::vector<std::string> out;
        parens(n, 0, 0, cur, out);
        assert(long(out.size()) == catalan[n]);
        assert(std::is_sorted(out.begin(), out.end()));        // '(' < ')' in ASCII
        for (const auto& s : out) {
            int d = 0;
            for (char ch : s) { d += ch == '(' ? 1 : -1; assert(d >= 0); }
            assert(d == 0);
        }
    }
    std::cout << "  counts follow Catalan numbers 1 1 2 5 14 42 132 429 1430\n";

    std::cout << "\nall partition / combination-sum / parentheses checks passed\n";
    return 0;
}
