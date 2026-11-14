/*
 * 02_benchmark_harness.cpp — measure complexity empirically with a doubling experiment
 *
 * Concept : If T(n) ~ c * n^k, then T(2n)/T(n) ~ 2^k, so k ~ log2(T(2n)/T(n)).
 *           A tiny harness (std::chrono::steady_clock, warm-up, best-of-N,
 *           a "do not optimise away" sink) estimates the exponent k of real
 *           code. log factors show up as k slightly above 1.
 *           Rules of honest benchmarking are in the README.
 * Build   : make run P=dsa/04-complexity-analysis/cpp/02_benchmark_harness.cpp
 *           Real numbers: make OPT=1 SAN=0 run P=dsa/04-complexity-analysis/cpp/02_benchmark_harness.cpp
 * Read    : ../README.md  (section "3.3 Common classes with concrete timings" -> "Measuring")
 */
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <numeric>
#include <random>
#include <vector>

static volatile std::uint64_t g_sink;      // results go here so the work is not deleted

// Best-of-reps wall time of f(n) in seconds; one warm-up run first (caches, page faults).
static double bench(const std::function<void(int)> &f, int n, int reps = 3)
{
    f(n);                                   // warm-up
    double best = 1e30;
    for (int r = 0; r < reps; r++) {
        auto t0 = std::chrono::steady_clock::now();   // monotonic, never jumps
        f(n);
        std::chrono::duration<double> dt = std::chrono::steady_clock::now() - t0;
        best = std::min(best, dt.count());
    }
    return best;
}

static double estimate_exponent(const char *name, const std::function<void(int)> &f, int n0, int steps)
{
    std::printf("%-26s", name);
    double prev = 0, k_sum = 0;
    int k_cnt = 0;
    for (int i = 0, n = n0; i < steps; i++, n *= 2) {
        double t = bench(f, n);
        std::printf("  n=%-7d %8.3f ms", n, t * 1e3);
        if (prev > 0 && t > 1e-5) { k_sum += std::log2(t / prev); k_cnt++; }
        prev = t;
    }
    double k = k_cnt ? k_sum / k_cnt : 0;
    std::printf("   => k ~ %.2f\n", k);
    return k;
}

int main()
{
    std::mt19937 rng(1);

    auto linear = [](int n) {
        std::vector<std::uint32_t> v(n, 1);
        g_sink = std::accumulate(v.begin(), v.end(), std::uint64_t{0});
    };
    auto sorting = [&rng](int n) {
        std::vector<std::uint32_t> v(n);
        for (auto &x : v) x = rng();
        std::sort(v.begin(), v.end());
        g_sink = v[n / 2];
    };
    auto quadratic = [](int n) {
        std::uint64_t s = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) s += static_cast<std::uint64_t>(i ^ j);
        g_sink = s;
    };
    // Lesson in "dominant term": building the vector is O(n), the 1000 binary
    // searches are O(1000 log n). The total behaves like O(n): k ~ 1.
    auto build_and_search = [](int n) {
        std::vector<int> v(n);
        std::iota(v.begin(), v.end(), 0);
        std::uint64_t s = 0;
        for (int q = 0; q < 1000; q++)
            s += static_cast<std::uint64_t>(std::lower_bound(v.begin(), v.end(), q * 7 % n) - v.begin());
        g_sink = s;
    };

    std::printf("Doubling experiment: k = average log2(T(2n)/T(n))\n");
    double k_lin = estimate_exponent("O(n)       accumulate", linear, 1 << 19, 4);
    double k_srt = estimate_exponent("O(n log n) std::sort", sorting, 1 << 16, 4);
    double k_quad = estimate_exponent("O(n^2)     double loop", quadratic, 400, 4);
    (void)estimate_exponent("O(n) build + O(log n) x1000", build_and_search, 1 << 16, 4);

    // Timing is noisy (other processes, CPU frequency scaling), so only a loose
    // sanity check: the quadratic exponent must clearly exceed the linear one.
    std::printf("\nlinear k=%.2f, n log n k=%.2f, quadratic k=%.2f\n", k_lin, k_srt, k_quad);
    assert(k_quad > k_lin + 0.5);
    std::printf("All checks passed.\n");
    return 0;
}
