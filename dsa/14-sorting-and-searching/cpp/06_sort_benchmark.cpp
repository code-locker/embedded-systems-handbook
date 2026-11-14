/*
 * 06_sort_benchmark.cpp — std::sort vs std::stable_sort vs qsort vs radix,
 *                         and the price of a function-pointer comparator
 *
 * Concept : Same algorithm family, very different speed:
 *             std::sort(v, lambda)       comparator inlined into the loop
 *             std::sort(v, fn pointer)   same introsort, indirect call per cmp
 *             qsort(v, cmp)              indirect call + byte-wise element moves
 *             std::stable_sort           merge sort, extra buffer
 *             radix                      no comparisons at all
 *           Run optimised, without sanitizers, for meaningful numbers:
 *
 *   rm -f build/dsa/14-sorting-and-searching/cpp/06_sort_benchmark
 *   make run P=dsa/14-sorting-and-searching/cpp/06_sort_benchmark.cpp OPT=1 SAN=0
 *
 *           (rm first: make does not rebuild when only the flags change.)
 * Build   : make run P=dsa/14-sorting-and-searching/cpp/06_sort_benchmark.cpp
 * Read    : ../README.md  (section "3.11 Benchmark")
 */
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#if defined(__SANITIZE_ADDRESS__) || !defined(__OPTIMIZE__)
constexpr bool kFast = false;
#else
constexpr bool kFast = true;
#endif

static int cmp_qsort(const void *a, const void *b)
{
    int x = *static_cast<const int *>(a), y = *static_cast<const int *>(b);
    return (x > y) - (x < y);
}

// A plain function used through a pointer. 'noinline' keeps the compiler
// from seeing through the pointer, as happens across translation units.
__attribute__((noinline)) static bool less_fn(int a, int b) { return a < b; }

static void radix_sort(std::vector<int> &v)
{
    std::vector<uint32_t> a(v.size()), b(v.size());
    for (size_t i = 0; i < v.size(); ++i) a[i] = static_cast<uint32_t>(v[i]) ^ 0x80000000u;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        std::array<size_t, 257> cnt{};
        for (uint32_t x : a) cnt[((x >> sh) & 0xFF) + 1]++;
        for (int i = 0; i < 256; ++i) cnt[i + 1] += cnt[i];
        for (uint32_t x : a) b[cnt[(x >> sh) & 0xFF]++] = x;
        a.swap(b);
    }
    for (size_t i = 0; i < v.size(); ++i) v[i] = static_cast<int>(a[i] ^ 0x80000000u);
}

struct Contestant {
    const char *name;
    std::function<void(std::vector<int> &)> run;
};

int main()
{
    const size_t n = kFast ? 2'000'000 : 50'000;
    std::printf("=== C++ sort benchmark: %s ===\n", kFast ? "optimised build"
                                                         : "debug+sanitizer build, timings NOT meaningful");
    if (!kFast)
        std::printf("For real numbers:  rm -f build/dsa/14-sorting-and-searching/cpp/06_sort_benchmark &&\n"
                    "                   make run P=dsa/14-sorting-and-searching/cpp/06_sort_benchmark.cpp OPT=1 SAN=0\n");
    std::printf("n = %zu ints\n\n", n);

    bool (*fp)(int, int) = less_fn;
    const std::vector<Contestant> cs = {
        {"std::sort (lambda)",     [](auto &v) { std::sort(v.begin(), v.end(), [](int a, int b) { return a < b; }); }},
        {"std::sort (fn pointer)", [fp](auto &v) { std::sort(v.begin(), v.end(), fp); }},
        {"std::stable_sort",       [](auto &v) { std::stable_sort(v.begin(), v.end()); }},
        {"qsort",                  [](auto &v) { std::qsort(v.data(), v.size(), sizeof(int), cmp_qsort); }},
        {"radix (LSD, 8-bit)",     [](auto &v) { radix_sort(v); }},
    };
    const char *kinds[] = {"random", "sorted", "reversed", "few-unique"};

    std::printf("%-24s", "ms");
    for (const char *k : kinds) std::printf("%12s", k);
    std::printf("\n");

    std::vector<std::vector<int>> inputs(4, std::vector<int>(n));
    std::mt19937 rng(77);
    for (size_t i = 0; i < n; ++i) {
        inputs[0][i] = static_cast<int>(rng());
        inputs[1][i] = static_cast<int>(i);
        inputs[2][i] = static_cast<int>(n - i);
        inputs[3][i] = static_cast<int>(rng() % 8);
    }
    std::vector<std::vector<int>> refs = inputs;
    for (auto &r : refs) std::sort(r.begin(), r.end());

    for (const auto &c : cs) {
        std::printf("%-24s", c.name);
        for (size_t k = 0; k < 4; ++k) {
            auto v = inputs[k];
            auto t0 = std::chrono::steady_clock::now();
            c.run(v);
            auto t1 = std::chrono::steady_clock::now();
            assert(v == refs[k]);                                  // correctness first
            std::printf("%12.2f", std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::printf("\n");
    }
    std::printf("\nExpect (optimised): lambda std::sort clearly beats the fn-pointer version\n"
                "and qsort; stable_sort is close to sort; radix wins big on random ints but\n"
                "gains nothing from presorted input (it never looks at order), while the\n"
                "comparison sorts get much faster on sorted/reversed data.\n");
    return 0;
}
