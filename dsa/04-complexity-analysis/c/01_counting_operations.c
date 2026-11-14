/*
 * 01_counting_operations.c — Big-O by COUNTING, not by guessing
 *
 * Concept : Complexity is "how does the number of basic steps grow with n".
 *           Instead of timing (noisy), count the steps of each loop shape
 *           exactly and watch what happens when n doubles:
 *             O(1)       stays the same       O(n)       doubles
 *             O(log n)   +1                   O(n log n) a bit more than doubles
 *             O(n^2)     x4                   O(2^n)     squares (n -> n+1 doubles!)
 * Build   : make run P=dsa/04-complexity-analysis/c/01_counting_operations.c
 * Read    : ../README.md  (section "3.2 Counting operations")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

typedef uint64_t u64;

static u64 constant(u64 n)        { (void)n; return 3; }               /* 3 steps, any n */

static u64 logarithmic(u64 n)                                          /* halve until 1 */
{
    u64 ops = 0;
    for (u64 i = n; i > 1; i /= 2) ops++;
    return ops;
}

static u64 linear(u64 n)
{
    u64 ops = 0;
    for (u64 i = 0; i < n; i++) ops++;
    return ops;
}

static u64 n_log_n(u64 n)                                              /* n times a halving loop */
{
    u64 ops = 0;
    for (u64 i = 0; i < n; i++)
        for (u64 j = n; j > 1; j /= 2) ops++;
    return ops;
}

static u64 quadratic(u64 n)                                            /* all pairs i < j */
{
    u64 ops = 0;
    for (u64 i = 0; i < n; i++)
        for (u64 j = i + 1; j < n; j++) ops++;
    return ops;                                                        /* n(n-1)/2 */
}

static u64 cubic(u64 n)
{
    u64 ops = 0;
    for (u64 i = 0; i < n; i++)
        for (u64 j = 0; j < n; j++)
            for (u64 k = 0; k < n; k++) ops++;
    return ops;
}

static u64 exponential(u64 n)                                          /* all subsets */
{
    return n == 0 ? 1 : 2 * exponential(n - 1);                        /* 2^n leaves */
}

/* A trap: this LOOKS like O(n^2) (two nested loops) but j never resets,
 * so the inner body runs n times IN TOTAL: O(n). (Two-pointer technique.) */
static u64 looks_quadratic_is_linear(u64 n)
{
    u64 ops = 0, j = 0;
    for (u64 i = 0; i < n; i++)
        while (j < i) { j++; ops++; }
    return ops + n;
}

/* Another trap: the inner loop runs i times, and i doubles: 1+2+4+...+n < 2n -> O(n). */
static u64 geometric_inner(u64 n)
{
    u64 ops = 0;
    for (u64 i = 1; i <= n; i *= 2)
        for (u64 j = 0; j < i; j++) ops++;
    return ops;
}

int main(void)
{
    printf("%6s %8s %8s %10s %12s %12s %14s\n", "n", "O(1)", "O(logn)", "O(n)", "O(nlogn)", "O(n^2)", "O(n^3)");
    for (u64 n = 16; n <= 512; n *= 2)
        printf("%6llu %8llu %8llu %10llu %12llu %12llu %14llu\n", (unsigned long long)n,
               (unsigned long long)constant(n), (unsigned long long)logarithmic(n),
               (unsigned long long)linear(n), (unsigned long long)n_log_n(n),
               (unsigned long long)quadratic(n), (unsigned long long)cubic(n));

    /* Exact checks of the formulas. */
    assert(logarithmic(1024) == 10);
    assert(linear(1000) == 1000);
    assert(n_log_n(1024) == 1024 * 10);
    assert(quadratic(1000) == 1000ULL * 999 / 2);
    assert(cubic(50) == 50ULL * 50 * 50);
    assert(exponential(20) == 1ULL << 20);

    /* Doubling ratios: the fingerprint of each class. */
    printf("\nratio f(2n)/f(n) at n = 256:  O(n): %.2f  O(nlogn): %.2f  O(n^2): %.2f  O(n^3): %.2f\n",
           (double)linear(512) / linear(256), (double)n_log_n(512) / n_log_n(256),
           (double)quadratic(512) / quadratic(256), (double)cubic(512) / cubic(256));
    printf("O(2^n): n=20 -> %llu, n=21 -> %llu (n+1 doubles the work)\n",
           (unsigned long long)exponential(20), (unsigned long long)exponential(21));

    printf("\ntraps: looks_quadratic_is_linear(10000) = %llu steps (~2n), geometric_inner(1<<20) = %llu (< 2n)\n",
           (unsigned long long)looks_quadratic_is_linear(10000), (unsigned long long)geometric_inner(1 << 20));
    assert(looks_quadratic_is_linear(10000) < 2 * 10000);
    assert(geometric_inner(1 << 20) < 2 * (1 << 20));

    printf("All checks passed.\n");
    return 0;
}
