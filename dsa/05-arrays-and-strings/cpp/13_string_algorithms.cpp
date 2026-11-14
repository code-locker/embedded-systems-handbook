/*
 * 13_string_algorithms.cpp — pattern search (KMP, Rabin-Karp, Boyer-Moore-
 *                            Horspool via std::search) and classic problems
 *
 * Concept : Three ways to find a pattern in O(n + m)-ish time:
 *             KMP         - never moves backwards in the text (failure table)
 *             Rabin-Karp  - compare HASHES of windows; rolling hash updates
 *                           in O(1) per shift; verify on hash hit
 *             BMH         - compare from the pattern's END and skip ahead by
 *                           up to m chars on a mismatch (sublinear on average;
 *                           what grep-like tools use). C++17 ships it.
 *           Plus: all anagram positions (sliding window of counts), longest
 *           palindromic substring (expand around centre), rotation check.
 *           Everything takes std::string_view: no copies of the input.
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/13_string_algorithms.cpp
 * Read    : ../notes/strings.md  (section "String algorithms")
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

using Positions = std::vector<std::size_t>;

// ---------------------------------------------------------------- KMP ----
// fail[i] = length of the longest proper prefix of p[0..i] that is also its
// suffix. See ../c/18_string_algorithms.c for the step-by-step diagram.
// O(m) build + O(n) scan, O(m) extra space.
static std::vector<std::size_t> kmp_table(std::string_view p)
{
    std::vector<std::size_t> fail(p.size(), 0);
    for (std::size_t i = 1, k = 0; i < p.size(); ++i) {
        while (k > 0 && p[i] != p[k])
            k = fail[k - 1];
        if (p[i] == p[k])
            ++k;
        fail[i] = k;
    }
    return fail;
}

static Positions kmp_find_all(std::string_view text, std::string_view p)
{
    Positions out;
    if (p.empty() || p.size() > text.size())
        return out;
    const auto fail = kmp_table(p);
    for (std::size_t i = 0, j = 0; i < text.size(); ++i) {
        while (j > 0 && text[i] != p[j])
            j = fail[j - 1];
        if (text[i] == p[j])
            ++j;
        if (j == p.size()) {
            out.push_back(i + 1 - j);
            j = fail[j - 1];
        }
    }
    return out;
}

// --------------------------------------------------------- Rabin-Karp ----
// Treat a window of m bytes as a number in base B, modulo a prime M:
//   h("abc") = (a*B^2 + b*B + c) mod M
// Sliding one step right: remove the leading char, shift, add the new one:
//   h' = ((h - a*B^(m-1)) * B + d) mod M          O(1) per step
//
//   text: [a b c] d e      h = H(abc)
//          a [b c d] e     h = (h - a*B^2)*B + d
//
// Equal hashes may be a collision, so verify with a real compare.
// Expected O(n + m); worst case O(n*m) if every window collides.
// Shines when searching for MANY patterns of the same length at once
// (put their hashes in a set) — plagiarism detection, rsync, dedup.
static Positions rabin_karp(std::string_view text, std::string_view p)
{
    Positions out;
    const std::size_t n = text.size(), m = p.size();
    if (m == 0 || m > n)
        return out;
    constexpr std::uint64_t B = 256, M = 1'000'000'007; // M prime; products fit in 64 bits
    std::uint64_t high = 1; // B^(m-1) mod M
    for (std::size_t i = 1; i < m; ++i)
        high = high * B % M;
    std::uint64_t hp = 0, hw = 0;
    for (std::size_t i = 0; i < m; ++i) {
        hp = (hp * B + static_cast<unsigned char>(p[i])) % M;
        hw = (hw * B + static_cast<unsigned char>(text[i])) % M;
    }
    for (std::size_t i = 0;; ++i) {
        if (hp == hw && text.substr(i, m) == p) // verify: hashes can collide
            out.push_back(i);
        if (i + m >= n)
            break;
        // + M*... keeps the subtraction from going negative in unsigned math
        std::uint64_t lead = static_cast<unsigned char>(text[i]) * high % M;
        hw = (hw + M - lead) % M;
        hw = (hw * B + static_cast<unsigned char>(text[i + m])) % M;
    }
    return out;
}

// ------------------------------------- Boyer-Moore-Horspool via the STL ----
static Positions bmh_find_all(std::string_view text, std::string_view p)
{
    Positions out;
    if (p.empty())
        return out;
    // The searcher preprocesses p once (a skip table of 256 entries).
    std::boyer_moore_horspool_searcher searcher(p.begin(), p.end());
    for (auto it = text.begin();;) {
        it = std::search(it, text.end(), searcher);
        if (it == text.end())
            break;
        out.push_back(static_cast<std::size_t>(it - text.begin()));
        ++it; // allow overlapping matches
    }
    return out;
}

static Positions naive_find_all(std::string_view text, std::string_view p)
{
    Positions out;
    if (p.empty())
        return out;
    for (auto pos = text.find(p); pos != std::string_view::npos;
         pos = text.find(p, pos + 1))
        out.push_back(pos);
    return out;
}

// -------------------------------------------------- anagram positions ----
// Every start index where some permutation of p occurs in s.
// Fixed-size sliding window + two count arrays; the window moves one step
// by adding the entering char and removing the leaving char: O(1) per step
// with a "matches" counter of how many of the 256 counts are equal.
// O(n + 256) total.
static Positions anagram_positions(std::string_view s, std::string_view p)
{
    Positions out;
    if (p.empty() || p.size() > s.size())
        return out;
    std::array<int, 256> need{}, have{};
    for (unsigned char c : p)
        ++need[c];
    int equal = 0; // number of byte values with have[c] == need[c]
    for (int c = 0; c < 256; ++c)
        equal += (need[c] == have[c]);
    auto add = [&](unsigned char c, int d) {
        equal -= (have[c] == need[c]);
        have[c] += d;
        equal += (have[c] == need[c]);
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        add(static_cast<unsigned char>(s[i]), +1); // char enters the window
        if (i >= p.size())
            add(static_cast<unsigned char>(s[i - p.size()]), -1); // char leaves
        if (equal == 256)
            out.push_back(i + 1 - p.size());
    }
    return out;
}

// --------------------------------------- longest palindromic substring ----
// Every palindrome has a centre: a char (odd length) or a gap (even).
// Expand from each of the 2n-1 centres while both ends match. O(n^2) time,
// O(1) space. (Manacher's algorithm does it in O(n) — an exercise.)
static std::string_view longest_palindrome(std::string_view s)
{
    std::size_t best_lo = 0, best_len = s.empty() ? 0 : 1;
    auto expand = [&](std::size_t lo, std::size_t hi) { // [lo, hi] inclusive
        while (true) {
            if (s[lo] != s[hi])
                return;
            if (hi - lo + 1 > best_len) {
                best_lo = lo;
                best_len = hi - lo + 1;
            }
            if (lo == 0 || hi + 1 == s.size())
                return;
            --lo;
            ++hi;
        }
    };
    for (std::size_t c = 0; c + 1 < s.size(); ++c) {
        expand(c, c);     // odd:  centre on s[c]
        expand(c, c + 1); // even: centre between s[c] and s[c+1]
    }
    return s.substr(best_lo, best_len);
}

// t is a rotation of s  <=>  same length and t occurs in s+s.
//   s = "waterbottle", s+s = "waterbottlewaterbottle", t = "erbottlewat" -> yes
static bool is_rotation(std::string_view s, std::string_view t)
{
    if (s.size() != t.size())
        return false;
    if (s.empty())
        return true;
    std::string ss;
    ss.reserve(2 * s.size());
    ss.append(s).append(s);
    return !kmp_find_all(ss, t).empty();
}

int main()
{
    std::puts("== 1. four searchers, one answer ==");
    const std::string_view text = "abracadabra abracadabra";
    for (std::string_view p : {"abra", "cad", "a", "zzz", "abracadabra"}) {
        auto k = kmp_find_all(text, p);
        std::printf("  \"%.*s\": %zu match(es)", static_cast<int>(p.size()),
                    p.data(), k.size());
        for (auto i : k)
            std::printf(" %zu", i);
        std::putchar('\n');
        CHECK(k == rabin_karp(text, p));
        CHECK(k == bmh_find_all(text, p));
        CHECK(k == naive_find_all(text, p));
    }

    std::puts("\n== 2. random cross-check (small alphabet = many near-misses) ==");
    std::mt19937 rng(42); // fixed seed: reproducible
    for (int t = 0; t < 3000; ++t) {
        std::string s(rng() % 80, 'a'), p(1 + rng() % 5, 'a');
        for (auto &c : s)
            c = static_cast<char>('a' + rng() % 3);
        for (auto &c : p)
            c = static_cast<char>('a' + rng() % 3);
        const auto ref = naive_find_all(s, p);
        CHECK(kmp_find_all(s, p) == ref);
        CHECK(rabin_karp(s, p) == ref);
        CHECK(bmh_find_all(s, p) == ref);
    }
    std::puts("  KMP == Rabin-Karp == BMH == naive on 3000 random cases");

    std::puts("\n== 3. anagram positions (sliding window) ==");
    auto pos = anagram_positions("cbaebabacd", "abc");
    std::printf("  anagrams of \"abc\" in \"cbaebabacd\" start at:");
    for (auto i : pos)
        std::printf(" %zu", i);
    std::putchar('\n');
    CHECK((pos == Positions{0, 6}));
    CHECK((anagram_positions("abab", "ab") == Positions{0, 1, 2}));

    std::puts("\n== 4. longest palindromic substring ==");
    for (auto [in, want] : {std::pair<std::string_view, std::string_view>{"babad", "bab"},
                            {"cbbd", "bb"},
                            {"forgeeksskeegfor", "geeksskeeg"},
                            {"x", "x"},
                            {"", ""}}) {
        auto got = longest_palindrome(in);
        std::printf("  \"%.*s\" -> \"%.*s\"\n", static_cast<int>(in.size()), in.data(),
                    static_cast<int>(got.size()), got.data());
        CHECK(got == want);
    }

    std::puts("\n== 5. rotation check via s+s ==");
    CHECK(is_rotation("waterbottle", "erbottlewat"));
    CHECK(!is_rotation("waterbottle", "bottlewatre"));
    CHECK(is_rotation("", ""));
    std::puts("  \"erbottlewat\" is a rotation of \"waterbottle\"");

    std::puts("\nall checks passed");
    return 0;
}
