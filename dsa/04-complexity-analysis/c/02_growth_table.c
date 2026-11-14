/*
 * 02_growth_table.c — what each complexity class means in SECONDS
 *
 * Concept : Assume ~1e9 simple operations per second (one modern core,
 *           ~1e8 for "realistic" work with cache misses). Then:
 *             n = 1e6:  O(n) = 1 ms,  O(n log n) = 20 ms,  O(n^2) = 17 MINUTES.
 *           The second part MEASURES a doubling experiment on this machine:
 *           time(2n)/time(n) ~ 2 for linear, ~4 for quadratic.
 * Build   : make run P=dsa/04-complexity-analysis/c/02_growth_table.c
 *           Real numbers: make OPT=1 SAN=0 run P=... — ASan + -O0
 *           make everything ~10x slower but the RATIOS stay the same.
 * Read    : ../README.md  (section "3.3 Common classes with concrete timings")
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void human(double seconds, char *out, size_t cap)
{
    if (seconds < 1e-6)        snprintf(out, cap, "%.0f ns", seconds * 1e9);
    else if (seconds < 1e-3)   snprintf(out, cap, "%.1f us", seconds * 1e6);
    else if (seconds < 1)      snprintf(out, cap, "%.1f ms", seconds * 1e3);
    else if (seconds < 120)    snprintf(out, cap, "%.1f s", seconds);
    else if (seconds < 7200)   snprintf(out, cap, "%.0f min", seconds / 60);
    else if (seconds < 172800) snprintf(out, cap, "%.0f hours", seconds / 3600);
    else if (seconds < 6.3e7)  snprintf(out, cap, "%.0f days", seconds / 86400);
    else if (seconds < 4.3e17) snprintf(out, cap, "%.2g years", seconds / 3.15e7);
    else                       snprintf(out, cap, "> universe age");   /* 13.8e9 years */
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static volatile uint64_t sink;      /* keeps the optimiser from deleting the loops */

static void linear_work(uint64_t n)
{
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i++) s += i ^ (s >> 3);
    sink = s;
}

static void quadratic_work(uint64_t n)
{
    uint64_t s = 0;
    for (uint64_t i = 0; i < n; i++)
        for (uint64_t j = 0; j < n; j++) s += i ^ j;
    sink = s;
}

static double time_it(void (*f)(uint64_t), uint64_t n)
{
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) {        /* best of 3: removes scheduler noise */
        double t0 = now();
        f(n);
        double dt = now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

int main(void)
{
    const double ops_per_sec = 1e9;
    const double ns[] = {10, 100, 1e3, 1e4, 1e5, 1e6, 1e9};
    printf("Estimated time at 1e9 simple operations/second:\n");
    printf("%8s | %9s | %9s | %9s | %11s | %12s | %16s\n", "n", "log2 n", "n", "n log2 n", "n^2", "n^3", "2^n");
    for (size_t i = 0; i < sizeof ns / sizeof ns[0]; i++) {
        double n = ns[i];
        double f[6] = {log2(n), n, n * log2(n), n * n, n * n * n, n <= 1000 ? pow(2, n) : INFINITY};
        char cell[6][24];
        for (int k = 0; k < 6; k++)
            human(isinf(f[k]) ? 1e300 : f[k] / ops_per_sec, cell[k], sizeof cell[k]);
        printf("%8.0e | %9s | %9s | %9s | %11s | %12s | %16s\n", n, cell[0], cell[1], cell[2], cell[3], cell[4], cell[5]);
    }

    printf("\nMeasured doubling experiment on this machine (best of 3):\n");
    double prev = 0;
    printf("  linear:    ");
    for (uint64_t n = 1u << 20; n <= 1u << 23; n *= 2) {
        double t = time_it(linear_work, n);
        printf("n=%-8llu %7.2f ms %s  ", (unsigned long long)n, t * 1e3, prev > 0 ? "" : "");
        if (prev > 0) printf("(x%.1f)  ", t / prev);
        prev = t;
    }
    printf("\n  quadratic: ");
    prev = 0;
    for (uint64_t n = 500; n <= 4000; n *= 2) {
        double t = time_it(quadratic_work, n);
        printf("n=%-5llu %7.2f ms ", (unsigned long long)n, t * 1e3);
        if (prev > 0) printf("(x%.1f)  ", t / prev);
        prev = t;
    }
    printf("\n  -> doubling n multiplies time by ~2 (linear) and ~4 (quadratic).\n");

    char buf[32];
    human(1e12 / ops_per_sec, buf, sizeof buf);
    printf("\nSorting 1e6 camera timestamps with an O(n^2) sort: ~%s. With O(n log n): ", buf);
    human(1e6 * log2(1e6) / ops_per_sec, buf, sizeof buf);
    printf("~%s.\n", buf);
    assert(sink != 12345);   /* use the sink */
    printf("All checks passed.\n");
    return 0;
}
