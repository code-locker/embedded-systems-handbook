/*
 * 04_recursion_master_theorem.c — complexity of recursive algorithms, by recurrence and by counting
 *
 * Concept : A recursive algorithm's cost is a RECURRENCE:
 *             binary search  T(n) = T(n/2) + O(1)      -> O(log n)
 *             merge sort     T(n) = 2T(n/2) + O(n)     -> O(n log n)
 *             naive fib      T(n) = T(n-1) + T(n-2)+1  -> O(phi^n) ~ O(1.618^n)
 *             memoised fib   each value computed once  -> O(n)
 *           The Master theorem solves T(n) = a T(n/b) + O(n^d) instantly.
 *           We count calls/comparisons and compare with the predictions.
 *           Space: recursion depth = stack frames = O(log n) or O(n).
 * Build   : make run P=dsa/04-complexity-analysis/c/04_recursion_master_theorem.c
 * Read    : ../README.md  (section "3.6 Recursion and the Master theorem")
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t calls, compares;
static int depth, max_depth;

/* ---- T(n) = T(n/2) + 1 ---------------------------------------------------- */
static int bsearch_rec(const int *a, int lo, int hi, int key)
{
    calls++;
    if (++depth > max_depth) max_depth = depth;
    int r = -1;
    if (lo <= hi) {
        int mid = lo + (hi - lo) / 2;        /* not (lo+hi)/2: that can overflow */
        compares++;
        if (a[mid] == key) r = mid;
        else if (a[mid] < key) r = bsearch_rec(a, mid + 1, hi, key);
        else r = bsearch_rec(a, lo, mid - 1, key);
    }
    depth--;
    return r;
}

/* ---- T(n) = 2T(n/2) + n ---------------------------------------------------- */
static void merge_sort(int *a, int *tmp, int n)
{
    calls++;
    if (++depth > max_depth) max_depth = depth;
    if (n > 1) {
        int h = n / 2;
        merge_sort(a, tmp, h);
        merge_sort(a + h, tmp, n - h);
        int i = 0, j = h, k = 0;
        while (i < h && j < n) { compares++; tmp[k++] = a[i] <= a[j] ? a[i++] : a[j++]; }
        while (i < h) tmp[k++] = a[i++];
        while (j < n) tmp[k++] = a[j++];
        memcpy(a, tmp, (size_t)n * sizeof *a);
    }
    depth--;
}

/*
 *  Recursion tree of merge sort, n = 8:          work per level
 *                    [8]                           8
 *              [4]          [4]                    8
 *           [2]   [2]    [2]   [2]                 8
 *          [1][1][1][1] [1][1][1][1]               (leaves)
 *  log2(8) = 3 levels x n work = n log n. Depth (stack) = log n + 1.
 */

/* ---- T(n) = T(n-1) + T(n-2) + 1 ------------------------------------------- */
static uint64_t fib_naive(int n) { calls++; return n < 2 ? (uint64_t)n : fib_naive(n - 1) + fib_naive(n - 2); }

static uint64_t memo[100];
static uint64_t fib_memo(int n)
{
    calls++;
    if (n < 2) return (uint64_t)n;
    if (memo[n]) return memo[n];
    return memo[n] = fib_memo(n - 1) + fib_memo(n - 2);
}

/* ---- T(n) = 2T(n/2) + 1 : e.g. sum of a tree / array by halves -> O(n) ---- */
static long sum_halves(const int *a, int n)
{
    calls++;
    if (n == 1) return a[0];
    return sum_halves(a, n / 2) + sum_halves(a + n / 2, n - n / 2);
}

int main(void)
{
    enum { N = 1 << 16 };
    int *a = malloc(N * sizeof *a), *tmp = malloc(N * sizeof *tmp);
    assert(a && tmp);
    for (int i = 0; i < N; i++) a[i] = 2 * i;

    calls = compares = 0; depth = max_depth = 0;
    assert(bsearch_rec(a, 0, N - 1, 2 * 12345) == 12345);
    printf("binary search n=%d: %llu calls, max depth %d   (log2 n = %d)  -> O(log n) time AND stack\n",
           N, (unsigned long long)calls, max_depth, 16);
    assert(calls <= 17 && max_depth <= 17);

    srand(42);
    for (int i = 0; i < N; i++) a[i] = rand();
    calls = compares = 0; depth = max_depth = 0;
    merge_sort(a, tmp, N);
    for (int i = 1; i < N; i++) assert(a[i - 1] <= a[i]);
    printf("merge sort    n=%d: %llu comparisons (n log2 n = %d), %llu calls (2n-1), depth %d\n",
           N, (unsigned long long)compares, N * 16, (unsigned long long)calls, max_depth);
    assert(compares <= (uint64_t)N * 16 && compares > (uint64_t)N * 14);
    assert(calls == 2 * N - 1);

    printf("\nnaive fib: T(n) = T(n-1) + T(n-2) + 1\n");
    for (int n = 20; n <= 28; n += 2) {
        calls = 0;
        uint64_t f = fib_naive(n);
        printf("  fib(%d) = %llu  calls = %llu\n", n, (unsigned long long)f, (unsigned long long)calls);
        uint64_t c = calls;
        memset(memo, 0, sizeof memo);
        assert(c == 2 * fib_memo(n + 1) - 1);   /* exact: calls(n) = 2*F(n+1) - 1 */
    }
    /* ratio of calls for n+1 vs n approaches phi = 1.618... */
    calls = 0; fib_naive(27); uint64_t c27 = calls;
    calls = 0; fib_naive(28); uint64_t c28 = calls;
    printf("  calls(28)/calls(27) = %.4f  (golden ratio 1.6180) -> O(1.618^n)\n", (double)c28 / c27);
    assert(fabs((double)c28 / c27 - 1.618) < 0.01);

    calls = 0;
    memset(memo, 0, sizeof memo);
    uint64_t f90 = fib_memo(90);
    printf("memoised fib(90) = %llu with %llu calls -> O(n)   (naive would need ~10^19 calls)\n",
           (unsigned long long)f90, (unsigned long long)calls);
    assert(calls <= 2 * 90);

    for (int i = 0; i < N; i++) a[i] = 1;
    calls = 0;
    assert(sum_halves(a, N) == N);
    printf("\nsum by halves T(n)=2T(n/2)+1: %llu calls for n=%d -> O(n) (Master case 1)\n",
           (unsigned long long)calls, N);
    assert(calls == 2 * N - 1);

    free(a);
    free(tmp);
    printf("All checks passed.\n");
    return 0;
}
