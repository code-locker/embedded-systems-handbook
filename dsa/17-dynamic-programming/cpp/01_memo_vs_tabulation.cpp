/*
 * 01_memo_vs_tabulation.cpp — top-down memoisation vs bottom-up tabulation
 *                             in modern C++, on 1-D DPs
 *
 * Concept : A reusable Memo<Key,Value> wrapper turns any pure recursive
 *           function into a memoised one. The same problems are then written
 *           bottom-up with std::vector and finally with O(1) rolling state.
 *           Problems: Fibonacci, house robber, decode ways (1-D DPs whose
 *           state is "position in the input").
 * Build   : make run P=dsa/17-dynamic-programming/cpp/01_memo_vs_tabulation.cpp
 * Read    : ../README.md  (sections "Top-down vs bottom-up vs space-optimised"
 *           and "The 5-step method")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#define CHECK(cond) assert(cond)

// ------------------------------------------------------------------ Memo --
// Wraps a recursive function f(self, key). `self` is how the function calls
// itself THROUGH the cache, so every recursive call is memoised too.
// std::map is used because the key only needs operator< (tuples, pairs and
// strings all work); std::unordered_map would need a hash.
template <class Key, class Value>
class Memo {
public:
    using Fn = std::function<Value(Memo &, const Key &)>;
    explicit Memo(Fn f) : f_(std::move(f)) {}

    Value operator()(const Key &k)
    {
        if (auto it = cache_.find(k); it != cache_.end()) return it->second;  // hit
        ++computed_;
        Value v = f_(*this, k);   // may recurse into operator() again
        cache_.emplace(k, v);     // emplace after the call: the map may have changed
        return v;
    }
    std::size_t computed() const { return computed_; }

private:
    Fn f_;
    std::map<Key, Value> cache_;
    std::size_t computed_ = 0;
};

// ------------------------------------------------------------- Fibonacci --
static std::uint64_t fib_tab(int n)
{
    if (n < 2) return static_cast<std::uint64_t>(n);
    std::vector<std::uint64_t> dp(static_cast<std::size_t>(n) + 1);
    dp[1] = 1;
    for (std::size_t i = 2; i < dp.size(); ++i) dp[i] = dp[i - 1] + dp[i - 2];
    return dp.back();
}

// ---------------------------------------------------------- house robber --
// Houses in a row with money; you may not rob two adjacent houses.
// Step 1 state : best(i) = max money from houses i..n-1
// Step 2 choice: rob i (then skip to i+2) or skip i (go to i+1)
// Step 3 recur : best(i) = max(a[i] + best(i+2), best(i+1))
// Step 4 base  : best(i) = 0 for i >= n
// Step 5 order : i from n-1 down to 0 (needs larger i first)
static int robber_brute(const std::vector<int> &a, std::size_t i)
{
    if (i >= a.size()) return 0;
    return std::max(a[i] + robber_brute(a, i + 2), robber_brute(a, i + 1));
}

static int robber_memo(const std::vector<int> &a)
{
    Memo<std::size_t, int> best([&a](auto &self, std::size_t i) -> int {
        if (i >= a.size()) return 0;
        return std::max(a[i] + self(i + 2), self(i + 1));
    });
    return best(0);
}

static int robber_tab(const std::vector<int> &a)
{
    std::vector<int> best(a.size() + 2, 0);       // +2: best[n], best[n+1] = 0
    for (std::size_t i = a.size(); i-- > 0;)
        best[i] = std::max(a[i] + best[i + 2], best[i + 1]);
    return best[0];
}

static int robber_rolling(const std::vector<int> &a)
{
    int next1 = 0, next2 = 0;                     // best(i+1), best(i+2)
    for (std::size_t i = a.size(); i-- > 0;) {
        int cur = std::max(a[i] + next2, next1);
        next2 = next1;
        next1 = cur;
    }
    return next1;
}

// ----------------------------------------------------------- decode ways --
// "A"=1 ... "Z"=26. How many ways to decode a digit string?
//   ways(i) = ways(i+1) if s[i] != '0'            (one-digit letter)
//           + ways(i+2) if s[i..i+1] in 10..26    (two-digit letter)
//   ways(n) = 1
// "226" -> "2 2 6", "22 6", "2 26" = 3.
static long long decode_brute(const std::string &s, std::size_t i)
{
    if (i == s.size()) return 1;
    if (s[i] == '0') return 0;                    // no letter starts with 0
    long long r = decode_brute(s, i + 1);
    if (i + 1 < s.size() && std::stoi(s.substr(i, 2)) <= 26) r += decode_brute(s, i + 2);
    return r;
}

static long long decode_tab(const std::string &s)
{
    const std::size_t n = s.size();
    std::vector<long long> ways(n + 1, 0);
    ways[n] = 1;
    for (std::size_t i = n; i-- > 0;) {
        if (s[i] == '0') continue;                // ways[i] stays 0
        ways[i] = ways[i + 1];
        if (i + 1 < n && (s[i] - '0') * 10 + (s[i + 1] - '0') <= 26) ways[i] += ways[i + 2];
    }
    return ways[0];
}

int main()
{
    std::cout << "== Fibonacci through the generic Memo wrapper ==\n";
    Memo<int, std::uint64_t> fib([](auto &self, int n) -> std::uint64_t {
        return n < 2 ? static_cast<std::uint64_t>(n) : self(n - 1) + self(n - 2);
    });
    std::cout << "fib(90) = " << fib(90) << ", distinct subproblems computed: " << fib.computed()
              << '\n';
    CHECK(fib(90) == 2880067194370816120ULL);
    CHECK(fib.computed() == 91);                  // n = 0..90, each exactly once
    for (int n = 0; n <= 90; ++n) CHECK(fib(n) == fib_tab(n));

    std::cout << "\n== House robber ==\n";
    const std::vector<int> houses{2, 7, 9, 3, 1};
    std::cout << "houses {2,7,9,3,1}: best = " << robber_tab(houses) << "  (2+9+1)\n";
    CHECK(robber_tab(houses) == 12 && robber_memo(houses) == 12 && robber_rolling(houses) == 12);

    std::mt19937 gen(17);
    std::uniform_int_distribution<int> money(0, 50), len(0, 18);
    for (int t = 0; t < 300; ++t) {
        std::vector<int> a(static_cast<std::size_t>(len(gen)));
        for (int &x : a) x = money(gen);
        const int bf = robber_brute(a, 0);
        CHECK(robber_memo(a) == bf && robber_tab(a) == bf && robber_rolling(a) == bf);
    }
    std::cout << "300 random streets: memo == table == rolling == brute force\n";

    std::cout << "\n== Decode ways ==\n";
    for (const char *s : {"12", "226", "06", "11106", "2611055971756562"})
        std::cout << "  \"" << s << "\" -> " << decode_tab(s) << " ways\n";
    CHECK(decode_tab("12") == 2 && decode_tab("226") == 3 && decode_tab("06") == 0);
    CHECK(decode_tab("11106") == 2);
    std::uniform_int_distribution<int> digit(0, 9), dlen(0, 14);
    for (int t = 0; t < 500; ++t) {
        std::string s(static_cast<std::size_t>(dlen(gen)), '0');
        for (char &c : s) c = static_cast<char>('0' + (digit(gen) % 3 == 0 ? digit(gen) : 1 + digit(gen) % 2));
        CHECK(decode_tab(s) == decode_brute(s, 0));
    }
    std::cout << "500 random digit strings: table == brute force\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
