/*
 * 04_applications.cpp — hashing problems the C++ way: two-sum, group
 *                       anagrams, top-k frequent, subarray sum == k,
 *                       dedup, longest substring without repeats
 *
 * Concept : Same "have I seen it?" idea as c/07_applications.c, but with
 *           std::unordered_map/set the code shrinks to its essence. Each
 *           function states brute-force vs hashed complexity.
 * Build   : make run P=dsa/08-hashing/cpp/04_applications.cpp
 * Read    : ../README.md  (section "3.8 The seen-before pattern")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Two-sum: O(n^2) -> O(n) time, O(n) space.
std::optional<std::pair<std::size_t, std::size_t>> two_sum(const std::vector<int> &a, int target) {
    std::unordered_map<int, std::size_t> seen; // value -> index
    for (std::size_t j = 0; j < a.size(); ++j) {
        if (auto it = seen.find(target - a[j]); it != seen.end()) return {{it->second, j}};
        seen.emplace(a[j], j); // AFTER the check: don't pair j with itself
    }
    return std::nullopt;
}

// Group anagrams: key = the sorted word ("listen" -> "eilnst").
// O(n * L log L). Canonical-form keys are a general trick: design the key
// so that "equivalent" things hash equal.
std::vector<std::vector<std::string>> group_anagrams(const std::vector<std::string> &words) {
    std::unordered_map<std::string, std::vector<std::string>> groups;
    for (const auto &w : words) {
        std::string key = w;
        std::sort(key.begin(), key.end());
        groups[key].push_back(w); // operator[] creating empty vector: wanted here
    }
    std::vector<std::vector<std::string>> out;
    for (auto &kv : groups) out.push_back(std::move(kv.second));
    return out;
}

// Top-k frequent: count O(n), then bucket by frequency O(n) (no sort).
//   freq: {1:3, 2:2, 3:1}  ->  by_count[3]={1}, [2]={2}, [1]={3}
//   walk by_count from high to low, take k.
std::vector<int> top_k_frequent(const std::vector<int> &a, std::size_t k) {
    std::unordered_map<int, std::size_t> freq;
    for (int x : a) ++freq[x];
    std::vector<std::vector<int>> by_count(a.size() + 1);
    for (auto &[x, c] : freq) by_count[c].push_back(x);
    std::vector<int> out;
    for (std::size_t c = a.size(); c > 0 && out.size() < k; --c)
        for (int x : by_count[c])
            if (out.size() < k) out.push_back(x);
    return out;
}

// Number of subarrays summing to k: prefix sums + hash.
// sum(i..j) = P[j+1] - P[i] == k  <=>  P[i] == P[j+1] - k.
// Keep a count of prefix sums seen so far. O(n) instead of O(n^2).
// Works with negative numbers (sliding window does not).
long count_subarrays_sum_k(const std::vector<int> &a, long k) {
    std::unordered_map<long, long> prefix_count{{0, 1}}; // empty prefix
    long sum = 0, total = 0;
    for (int x : a) {
        sum += x;
        if (auto it = prefix_count.find(sum - k); it != prefix_count.end()) total += it->second;
        ++prefix_count[sum];
    }
    return total;
}

// Longest substring with no repeated char: sliding window + last-seen map.
int longest_unique_substring(const std::string &s) {
    std::unordered_map<char, std::size_t> last; // char -> last index
    std::size_t start = 0, best = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (auto it = last.find(s[i]); it != last.end() && it->second >= start)
            start = it->second + 1; // jump past the previous copy
        last[s[i]] = i;
        best = std::max(best, i - start + 1);
    }
    return static_cast<int>(best);
}

// Dedup keeping order. unordered_set::insert returns {it, bool inserted}.
std::vector<int> dedup(const std::vector<int> &a) {
    std::unordered_set<int> seen;
    std::vector<int> out;
    for (int x : a)
        if (seen.insert(x).second) out.push_back(x);
    return out;
}

int main() {
    auto r = two_sum({2, 7, 11, 15}, 9);
    assert(r && r->first == 0 && r->second == 1);
    assert(!two_sum({2, 7}, 4));

    auto g = group_anagrams({"eat", "tea", "tan", "ate", "nat", "bat"});
    assert(g.size() == 3);
    std::size_t sizes = 0;
    for (auto &grp : g) {
        sizes += grp.size();
        if (std::find(grp.begin(), grp.end(), "eat") != grp.end()) assert(grp.size() == 3);
    }
    assert(sizes == 6);

    auto top = top_k_frequent({1, 1, 1, 2, 2, 3}, 2);
    assert((top == std::vector<int>{1, 2}));

    assert(count_subarrays_sum_k({1, 1, 1}, 2) == 2);
    assert(count_subarrays_sum_k({3, 4, 7, 2, -3, 1, 4, 2}, 7) == 4);

    assert(longest_unique_substring("abcabcbb") == 3);
    assert(longest_unique_substring("pwwkew") == 3);
    assert(longest_unique_substring("") == 0);

    assert((dedup({5, 1, 5, 2, 1, 9}) == std::vector<int>{5, 1, 2, 9}));

    std::cout << "two-sum, group anagrams, top-k, subarray-sum, longest-unique, dedup: OK\n";
    std::cout << "all application checks passed\n";
    return 0;
}
