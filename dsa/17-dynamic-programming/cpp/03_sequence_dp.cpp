/*
 * 03_sequence_dp.cpp — sequence DPs with the STL: LIS via std::lower_bound,
 *                      LCS, edit distance, and a tiny line-based diff
 *
 * Concept : LIS in O(n log n) is "patience sorting": tails[] stays sorted, so
 *           std::lower_bound (strict LIS) / std::upper_bound (non-decreasing)
 *           finds where each element goes. LCS on LINES instead of characters
 *           is exactly what `diff` prints: lines outside the LCS are - or +.
 * Build   : make run P=dsa/17-dynamic-programming/cpp/03_sequence_dp.cpp
 * Read    : ../README.md  (sections "Longest increasing subsequence", "LCS",
 *           "Edit distance")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#define CHECK(cond) assert(cond)

// LIS length. strict = true  -> a[i] <  a[j] (lower_bound: replace first >= x)
//             strict = false -> a[i] <= a[j] (upper_bound: replace first >  x)
// Why the bound matters: with duplicates {2,2,2}, strict LIS is 1, but
// non-decreasing is 3. lower_bound lands ON the equal 2 and replaces it
// (length stays 1); upper_bound skips past it and appends.
template <class T>
std::size_t lis_length(const std::vector<T> &a, bool strict = true)
{
    std::vector<T> tails;
    for (const T &x : a) {
        auto it = strict ? std::lower_bound(tails.begin(), tails.end(), x)
                         : std::upper_bound(tails.begin(), tails.end(), x);
        if (it == tails.end()) tails.push_back(x);   // extends the longest
        else *it = x;                                // better (smaller) tail
    }
    return tails.size();
}

template <class T>
std::size_t lis_brute(const std::vector<T> &a, bool strict)
{
    std::size_t best = 0;
    for (unsigned mask = 0; mask < (1u << a.size()); ++mask) {
        std::vector<T> s;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (mask >> i & 1u) s.push_back(a[i]);
        bool ok = true;
        for (std::size_t i = 1; i < s.size() && ok; ++i) ok = strict ? s[i - 1] < s[i] : s[i - 1] <= s[i];
        if (ok) best = std::max(best, s.size());
    }
    return best;
}

// Generic LCS over any sequence type (std::string, std::vector<std::string>).
// Returns the full table so callers can walk it back.
template <class Seq>
std::vector<std::vector<int>> lcs_table(const Seq &a, const Seq &b)
{
    std::vector<std::vector<int>> dp(a.size() + 1, std::vector<int>(b.size() + 1, 0));
    for (std::size_t i = 1; i <= a.size(); ++i)
        for (std::size_t j = 1; j <= b.size(); ++j)
            dp[i][j] = a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1
                                            : std::max(dp[i - 1][j], dp[i][j - 1]);
    return dp;
}

// diff: walk back from the bottom-right corner of the LCS table, emitting one
// line per step, then reverse (the walk produces the script backwards).
//   ' ' line in both (part of LCS), '-' only in a (deleted), '+' only in b
std::vector<std::string> diff(const std::vector<std::string> &a, const std::vector<std::string> &b)
{
    const auto dp = lcs_table(a, b);
    std::vector<std::string> out;
    std::size_t i = a.size(), j = b.size();
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && a[i - 1] == b[j - 1]) { out.push_back("  " + a[--i]); --j; }
        else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) out.push_back("+ " + b[--j]);
        else out.push_back("- " + a[--i]);
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Edit distance with O(min(n,m)) memory: one row plus the saved diagonal.
//   diag = dp[i-1][j-1], row[j] (before overwrite) = dp[i-1][j], row[j-1] = dp[i][j-1]
int edit_distance(const std::string &a, const std::string &b)
{
    if (a.size() < b.size()) return edit_distance(b, a);   // keep the row short
    std::vector<int> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) row[j] = static_cast<int>(j);
    for (std::size_t i = 1; i <= a.size(); ++i) {
        int diag = row[0];
        row[0] = static_cast<int>(i);
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const int up = row[j];
            row[j] = a[i - 1] == b[j - 1] ? diag : 1 + std::min({diag, up, row[j - 1]});
            diag = up;
        }
    }
    return row[b.size()];
}

int edit_brute(const std::string &a, const std::string &b)
{
    if (a.empty()) return static_cast<int>(b.size());
    if (b.empty()) return static_cast<int>(a.size());
    const std::string ra = a.substr(1), rb = b.substr(1);
    if (a[0] == b[0]) return edit_brute(ra, rb);
    return 1 + std::min({edit_brute(ra, rb), edit_brute(ra, b), edit_brute(a, rb)});
}

int main()
{
    std::cout << "== LIS with std::lower_bound / std::upper_bound ==\n";
    const std::vector<int> a{3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5};
    std::cout << "strict LIS          = " << lis_length(a) << "  (1 4 5 9 or 1 2 3 5)\n";
    std::cout << "non-decreasing LIS  = " << lis_length(a, false) << "  (1 1 2 3 5)\n";
    CHECK(lis_length(a) == 4 && lis_length(a, false) == 5);
    const std::vector<std::string> words{"pear", "apple", "fig", "kiwi", "banana", "mango"};
    std::cout << "LIS of words (alphabetical) = " << lis_length(words) << "  (apple fig kiwi mango)\n";
    CHECK(lis_length(words) == 4);

    std::mt19937 gen(5);
    std::uniform_int_distribution<int> v(0, 6), n(0, 14);
    for (int t = 0; t < 300; ++t) {
        std::vector<int> b(static_cast<std::size_t>(n(gen)));
        for (int &x : b) x = v(gen);
        CHECK(lis_length(b) == lis_brute(b, true));
        CHECK(lis_length(b, false) == lis_brute(b, false));
    }
    std::cout << "300 random arrays: both variants == brute force\n";

    std::cout << "\n== diff = LCS over lines ==\n";
    const std::vector<std::string> old_file{"#include <stdio.h>", "int main(void)", "{",
                                            "    printf(\"hi\\n\");", "    return 0;", "}"};
    const std::vector<std::string> new_file{"#include <stdio.h>", "#include <stdlib.h>", "int main(void)",
                                            "{", "    puts(\"hi\");", "    return 0;", "}"};
    const auto d = diff(old_file, new_file);
    for (const auto &line : d) std::cout << "  " << line << '\n';
    CHECK(lcs_table(old_file, new_file).back().back() == 5);
    CHECK(std::count_if(d.begin(), d.end(), [](const std::string &s) { return s[0] == '+'; }) == 2);
    CHECK(std::count_if(d.begin(), d.end(), [](const std::string &s) { return s[0] == '-'; }) == 1);

    std::cout << "\n== Edit distance (two-row version) ==\n";
    for (auto [x, y] : std::vector<std::pair<std::string, std::string>>{
             {"kitten", "sitting"}, {"intention", "execution"}, {"", "abc"}, {"gstreamer", "streamer"}})
        std::cout << "  " << x << " -> " << y << " : " << edit_distance(x, y) << '\n';
    CHECK(edit_distance("kitten", "sitting") == 3);
    CHECK(edit_distance("intention", "execution") == 5);
    std::uniform_int_distribution<int> ch(0, 2), sl(0, 7);
    for (int t = 0; t < 300; ++t) {
        std::string x(static_cast<std::size_t>(sl(gen)), 'a'), y(static_cast<std::size_t>(sl(gen)), 'a');
        for (char &c : x) c = static_cast<char>('a' + ch(gen));
        for (char &c : y) c = static_cast<char>('a' + ch(gen));
        CHECK(edit_distance(x, y) == edit_brute(x, y));
    }
    std::cout << "300 random pairs: two-row edit distance == brute force\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
