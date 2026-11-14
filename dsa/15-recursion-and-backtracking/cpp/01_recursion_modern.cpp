/*
 * 01_recursion_modern.cpp — recursion in modern C++: recursive lambdas,
 *                           constexpr (compile-time) recursion, variadic
 *                           template recursion, memoisation with a map
 *
 * Concept : The idea is identical to C; C++ adds ways to express it:
 *             - constexpr functions: the compiler runs the recursion and
 *               static_assert checks the answer at BUILD time;
 *             - lambdas cannot name themselves, so pass "self" explicitly
 *               (cheap) or wrap in std::function (heap + indirect call);
 *             - variadic templates recurse over a parameter pack at compile
 *               time (and C++17 fold expressions replace that recursion);
 *             - memoisation with std::unordered_map preview (chapter 17).
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/01_recursion_modern.cpp
 * Read    : ../README.md  (sections "C vs C++", "Memoisation preview")
 */
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// 1. constexpr recursion: evaluated by the compiler.
// ---------------------------------------------------------------------------
constexpr std::uint64_t fact(unsigned n) { return n <= 1 ? 1 : n * fact(n - 1); }
constexpr unsigned gcd(unsigned a, unsigned b) { return b == 0 ? a : gcd(b, a % b); }
constexpr std::uint64_t fib(unsigned n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static_assert(fact(10) == 3628800, "checked while compiling, zero run-time cost");
static_assert(gcd(48, 18) == 6);
static_assert(fib(20) == 6765);     // compilers memoise constexpr calls internally,
                                    // but have a step limit: fib(60) would fail.

// ---------------------------------------------------------------------------
// 2. Variadic template recursion: peel one argument per "call".
//    sum_rec(1, 2, 3) -> 1 + sum_rec(2, 3) -> 1 + (2 + sum_rec(3)) -> base
// ---------------------------------------------------------------------------
template <typename T>
constexpr T sum_rec(T last) { return last; }                       // base case

template <typename T, typename... Rest>
constexpr T sum_rec(T first, Rest... rest) { return first + sum_rec(rest...); }

template <typename... Ts>
constexpr auto sum_fold(Ts... xs) { return (xs + ...); }           // C++17 fold

static_assert(sum_rec(1, 2, 3, 4) == 10);
static_assert(sum_fold(1, 2, 3, 4) == 10);

// ---------------------------------------------------------------------------
// 3. Memoisation with a hash map — "count paths in a grid moving right/down"
//    paths(r, c) = paths(r-1, c) + paths(r, c-1), paths(0, *) = paths(*, 0) = 1
// ---------------------------------------------------------------------------
class GridPaths {
public:
    std::uint64_t count(int r, int c) {
        ++calls_;
        if (r == 0 || c == 0) return 1;
        const std::uint64_t key = (std::uint64_t(r) << 32) | unsigned(c);
        if (auto it = memo_.find(key); it != memo_.end()) return it->second;  // hit
        const std::uint64_t v = count(r - 1, c) + count(r, c - 1);
        memo_.emplace(key, v);         // store BEFORE returning: future calls hit
        return v;
    }
    long calls() const { return calls_; }

private:
    std::unordered_map<std::uint64_t, std::uint64_t> memo_;   // owns the cache
    long calls_ = 0;
};

static std::uint64_t binom(unsigned n, unsigned k) {          // reference: C(r+c, r)
    std::uint64_t r = 1;
    for (unsigned i = 1; i <= k; ++i) r = r * (n - k + i) / i;
    return r;
}

int main() {
    std::cout << "== constexpr recursion ==\n";
    constexpr auto f20 = fact(20);       // computed by the compiler
    std::cout << "fact(20) = " << f20 << " (computed at compile time)\n";
    assert(fact(5) == 120);

    std::cout << "\n== recursive lambdas ==\n";
    // (a) "self" parameter: the lambda receives itself. No heap, inlinable.
    auto fib_self = [](auto&& self, unsigned n) -> std::uint64_t {
        return n < 2 ? n : self(self, n - 1) + self(self, n - 2);
    };
    // (b) std::function: the lambda captures the std::function by reference.
    //     Type-erased: possible heap allocation + indirect call per level.
    std::function<std::uint64_t(unsigned)> fib_fn = [&fib_fn](unsigned n) -> std::uint64_t {
        return n < 2 ? n : fib_fn(n - 1) + fib_fn(n - 2);
    };
    for (unsigned n = 0; n <= 25; ++n) assert(fib_self(fib_self, n) == fib_fn(n));
    std::cout << "fib(25) = " << fib_fn(25) << " (both lambda styles agree for n<=25)\n";

    // A recursive lambda doing real work: all strings of length n over {a,b}
    std::vector<std::string> words;
    std::string cur;
    auto gen = [&](auto&& self, int n) -> void {
        if (int(cur.size()) == n) { words.push_back(cur); return; }
        for (char ch : {'a', 'b'}) {
            cur.push_back(ch);           // choose
            self(self, n);               // explore
            cur.pop_back();              // un-choose
        }
    };
    gen(gen, 3);
    assert(words.size() == 8 && words.front() == "aaa" && words.back() == "bbb");
    std::cout << "strings over {a,b} of length 3:";
    for (const auto& w : words) std::cout << ' ' << w;
    std::cout << '\n';

    std::cout << "\n== variadic recursion ==\n";
    std::cout << "sum_rec(1,2,3,4) = " << sum_rec(1, 2, 3, 4)
              << ", sum_fold(1.5, 2.5) = " << sum_fold(1.5, 2.5) << '\n';

    std::cout << "\n== memoised grid paths ==\n";
    for (int r = 0; r <= 16; r += 4)
        for (int c = 0; c <= 16; c += 4) {
            GridPaths g;
            const auto v = g.count(r, c);
            assert(v == binom(unsigned(r + c), unsigned(r)));
            assert(g.calls() <= 2L * (r + 1) * (c + 1) + 1);   // ~2 calls per cell
        }
    GridPaths g;
    std::cout << "paths(16,16) = " << g.count(16, 16) << " with " << g.calls()
              << " calls (naive recursion would make ~1.2 billion)\n";

    std::cout << "\nall modern-recursion checks passed\n";
    return 0;
}
