/*
 * 05_best_avg_worst.c — the same algorithm, three very different costs
 *
 * Concept : Complexity depends on the INPUT, not only on n.
 *             linear search : best 1 (first), average n/2, worst n (absent)
 *             insertion sort: best n-1 (already sorted), average ~n^2/4, worst ~n^2/2 (reversed)
 *             quicksort (first element as pivot): average ~1.39 n log2 n,
 *                           worst n^2/2 on ALREADY SORTED input — a famous trap
 *           Big-O usually quotes the WORST case; libraries document which.
 * Build   : make run P=dsa/04-complexity-analysis/c/05_best_avg_worst.c
 * Read    : ../README.md  (section "3.7 Best, average, worst case")
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t cmp;

static int linear_search(const int *a, int n, int key)
{
    for (int i = 0; i < n; i++) { cmp++; if (a[i] == key) return i; }
    return -1;
}

static void insertion_sort(int *a, int n)
{
    for (int i = 1; i < n; i++) {
        int x = a[i], j = i - 1;
        while (j >= 0) {
            cmp++;
            if (a[j] <= x) break;
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = x;
    }
}

/* Quicksort with the FIRST element as pivot (Lomuto-style partition). */
static void quicksort_first(int *a, int lo, int hi)
{
    while (lo < hi) {                       /* loop on one side: bounded stack for the demo */
        int p = a[lo], i = lo;
        for (int j = lo + 1; j <= hi; j++) {
            cmp++;
            if (a[j] < p) { i++; int t = a[i]; a[i] = a[j]; a[j] = t; }
        }
        int t = a[lo]; a[lo] = a[i]; a[i] = t;
        if (i - lo < hi - i) { quicksort_first(a, lo, i - 1); lo = i + 1; }
        else                 { quicksort_first(a, i + 1, hi); hi = i - 1; }
    }
}

enum shape { SORTED, RANDOM, REVERSED };
static void make(int *a, int n, enum shape s)
{
    for (int i = 0; i < n; i++) a[i] = s == SORTED ? i : s == REVERSED ? n - i : rand() % (10 * n);
}

int main(void)
{
    enum { N = 4000 };
    static int a[N];
    srand(7);
    const char *names[] = {"sorted", "random", "reversed"};

    make(a, N, SORTED);
    cmp = 0; linear_search(a, N, 0);       uint64_t best = cmp;
    cmp = 0; linear_search(a, N, -1);      uint64_t worst = cmp;
    uint64_t total = 0;
    for (int k = 0; k < N; k++) { cmp = 0; linear_search(a, N, k); total += cmp; }
    printf("linear search n=%d: best %llu, average %.0f, worst %llu comparisons\n", N,
           (unsigned long long)best, (double)total / N, (unsigned long long)worst);
    assert(best == 1 && worst == N && llabs((long long)(total / N) - N / 2) <= 1);

    printf("\n%-16s %10s %10s %10s   (n=%d)\n", "algorithm", names[0], names[1], names[2], N);
    uint64_t ins[3], qs[3];
    for (int s = 0; s < 3; s++) {
        make(a, N, (enum shape)s); cmp = 0; insertion_sort(a, N); ins[s] = cmp;
        for (int i = 1; i < N; i++) assert(a[i - 1] <= a[i]);
        make(a, N, (enum shape)s); cmp = 0; quicksort_first(a, 0, N - 1); qs[s] = cmp;
        for (int i = 1; i < N; i++) assert(a[i - 1] <= a[i]);
    }
    printf("%-16s %10llu %10llu %10llu\n", "insertion sort", (unsigned long long)ins[0], (unsigned long long)ins[1], (unsigned long long)ins[2]);
    printf("%-16s %10llu %10llu %10llu\n", "quicksort(1st)", (unsigned long long)qs[0], (unsigned long long)qs[1], (unsigned long long)qs[2]);
    printf("reference: n-1 = %d, n^2/4 = %d, n^2/2 = %d, 1.39 n log2 n = %.0f\n",
           N - 1, N * N / 4, N * N / 2, 1.39 * N * log2(N));

    assert(ins[SORTED] == N - 1);                          /* best: O(n) */
    assert(ins[REVERSED] == (uint64_t)N * (N - 1) / 2);    /* worst: O(n^2) */
    assert(ins[RANDOM] > (uint64_t)N * N / 5 && ins[RANDOM] < (uint64_t)N * N / 3);
    assert(qs[SORTED] == (uint64_t)N * (N - 1) / 2);       /* sorted input = worst for first-pivot */
    assert(qs[RANDOM] < 2 * 1.39 * N * log2(N));           /* average: O(n log n) */
    printf("=> insertion sort is GREAT on nearly-sorted data; first-pivot quicksort is TERRIBLE on it.\n"
           "   Real quicksorts use median-of-3 / random pivots and fall back to heapsort (introsort).\n");
    printf("All checks passed.\n");
    return 0;
}
