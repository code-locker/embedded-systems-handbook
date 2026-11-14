/*
 * 08_quickselect.c — k-th smallest in O(n) average: quickselect (Hoare's FIND)
 *                    and median-of-medians for an O(n) WORST case
 *
 * Concept : Quicksort recurses into BOTH sides of the pivot. If you only want
 *           the k-th smallest, you know which side k is on — recurse into ONE.
 *             n + n/2 + n/4 + ... = 2n  ->  O(n) average, O(n^2) worst.
 *           Median-of-medians picks a pivot guaranteed to be in the middle
 *           30%-70%, giving O(n) worst case (big constant; used as a fallback,
 *           like std::nth_element's introselect).
 *           Uses: median filter (image denoise), percentiles (p99 latency),
 *           top-k, kd-tree construction.
 * Build   : make run P=dsa/14-sorting-and-searching/c/08_quickselect.c
 * Read    : ../README.md  (section "3.9 Selection: quickselect and nth_element")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 5150u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
static unsigned long g_cmps;

/* Lomuto partition around a[hi]; returns pivot's final index. */
static size_t partition(int *a, size_t lo, size_t hi)
{
    int pivot = a[hi];
    size_t i = lo;
    for (size_t j = lo; j < hi; j++) {
        g_cmps++;
        if (a[j] < pivot) swap(&a[i++], &a[j]);
    }
    swap(&a[i], &a[hi]);
    return i;
}

/* ---------------------------------------------------------------------------
 * QUICKSELECT: returns the k-th smallest (k is 0-based) and, as a side
 * effect, leaves a[k] = that value, a[0..k) <= a[k] <= a(k..n)  — exactly the
 * guarantee of std::nth_element.
 *
 *   find k=3 in [9 1 8 2 7 3 6]        pivot (random) = 6
 *   partition -> [1 2 3 | 6 | 7 9 8]  pivot index 3 == k  -> answer 6
 *   (order INSIDE each side is arbitrary — only the split is guaranteed)
 *   if pivot index were 5 > k: continue in the LEFT part only, etc.
 *
 * Iterative (no recursion needed: only one side is kept).
 * --------------------------------------------------------------------------- */
static int quickselect(int *a, size_t n, size_t k)
{
    size_t lo = 0, hi = n - 1;
    while (lo < hi) {
        size_t r = lo + xorshift32() % (hi - lo + 1);   /* random pivot */
        swap(&a[r], &a[hi]);
        size_t p = partition(a, lo, hi);
        if (p == k) break;
        if (k < p) hi = p - 1;      /* answer is left of the pivot  */
        else       lo = p + 1;      /* answer is right of the pivot */
    }
    return a[k];
}

/* ---------------------------------------------------------------------------
 * MEDIAN OF MEDIANS (Blum-Floyd-Pratt-Rivest-Tarjan, 1973).
 *   1. split into groups of 5, find each group's median (insertion sort)
 *   2. recursively find the median M of those n/5 medians
 *   3. partition around M: at least 3/10 of the items are <= M and 3/10 >= M
 *        groups:  o o o o ...       half the medians are <= M, and in each of
 *                 o o o o ...       those groups 3 items are <= their median
 *        median-> * * M * ...       -> (n/5)/2 * 3 = 3n/10 items <= M
 *                 o o o o ...
 *   T(n) <= T(n/5) + T(7n/10) + O(n)  ->  O(n) because 1/5 + 7/10 < 1.
 * Caveat: the 3/10 bound assumes DISTINCT keys. With a 2-way partition, many
 * equal keys all land on one side; a production version uses a 3-way
 * (<, ==, >) partition so the guarantee survives duplicates.
 * --------------------------------------------------------------------------- */
static void insertion(int *a, size_t lo, size_t hi)   /* sorts a[lo..hi] */
{
    for (size_t i = lo + 1; i <= hi; i++) {
        int key = a[i];
        size_t j = i;
        while (j > lo && a[j - 1] > key) { g_cmps++; a[j] = a[j - 1]; j--; }
        a[j] = key;
    }
}

static size_t mom_select_idx(int *a, size_t lo, size_t hi, size_t k);

/* Returns the INDEX (in a[lo..hi]) of a good pivot. */
static size_t median_of_medians(int *a, size_t lo, size_t hi)
{
    size_t n = hi - lo + 1;
    if (n <= 5) { insertion(a, lo, hi); return lo + n / 2; }
    size_t m = 0;                                     /* medians packed to the front */
    for (size_t g = lo; g <= hi; g += 5) {
        size_t ghi = (g + 4 <= hi) ? g + 4 : hi;
        insertion(a, g, ghi);
        swap(&a[lo + m], &a[g + (ghi - g) / 2]);
        m++;
    }
    return mom_select_idx(a, lo, lo + m - 1, lo + m / 2);
}

/* k is an absolute index in [lo, hi]; returns k after placing the right
 * value there. Deterministic: no random numbers at all. */
static size_t mom_select_idx(int *a, size_t lo, size_t hi, size_t k)
{
    for (;;) {
        if (lo == hi) return lo;
        size_t piv = median_of_medians(a, lo, hi);
        swap(&a[piv], &a[hi]);
        size_t p = partition(a, lo, hi);
        if (p == k) return k;
        if (k < p) hi = p - 1;
        else       lo = p + 1;
    }
}

static int mom_select(int *a, size_t n, size_t k)
{
    return a[mom_select_idx(a, 0, n - 1, k)];
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

/* Partition guarantee check: everything left <= a[k] <= everything right. */
static void check_partitioned(const int *a, size_t n, size_t k)
{
    for (size_t i = 0; i < k; i++) assert(a[i] <= a[k]);
    for (size_t i = k + 1; i < n; i++) assert(a[i] >= a[k]);
}

int main(void)
{
    printf("=== Selection: k-th smallest without sorting ===\n\n");
    int demo[] = {9, 1, 8, 2, 7, 3, 6};
    size_t n = sizeof demo / sizeof demo[0];
    int v = quickselect(demo, n, 3);
    printf("quickselect k=3 (median) of [9 1 8 2 7 3 6] -> %d\n  array after: [", v);
    for (size_t i = 0; i < n; i++) printf("%s%d", i ? " " : "", demo[i]);
    printf("]  (left part <= %d <= right part)\n", v);
    assert(v == 6);
    check_partitioned(demo, n, 3);

    /* ---- random tests ----------------------------------------------------- */
    enum { MAXN = 2000 };
    static int a[MAXN], b[MAXN], ref[MAXN];
    for (int t = 0; t < 500; t++) {
        size_t m = 1 + xorshift32() % (MAXN - 1);
        uint32_t range = (t & 1) ? 5u : 1000000u;
        for (size_t i = 0; i < m; i++) a[i] = b[i] = ref[i] = (int)(xorshift32() % range);
        qsort(ref, m, sizeof *ref, cmp_int);
        size_t k = xorshift32() % m;
        assert(quickselect(a, m, k) == ref[k]);
        check_partitioned(a, m, k);
        assert(mom_select(b, m, k) == ref[k]);
        check_partitioned(b, m, k);
    }
    printf("\nquickselect and median-of-medians == sorted[k] on 500 random arrays,\n"
           "and both leave the array partitioned around k (nth_element contract).\n");

    /* ---- linear behaviour ------------------------------------------------- */
    printf("\nComparisons to find the median (vs ~n log2 n to sort):\n");
    for (size_t m = 1000; m <= 64000; m *= 4) {
        static int c[64000];
        for (size_t i = 0; i < m; i++) c[i] = (int)xorshift32();
        g_cmps = 0;
        quickselect(c, m, m / 2);
        unsigned long qs = g_cmps;
        for (size_t i = 0; i < m; i++) c[i] = (int)i;      /* sorted: bad for naive pivots */
        g_cmps = 0;
        mom_select(c, m, m / 2);
        printf("  n=%6zu  quickselect=%8lu (%.1f n)   median-of-medians(sorted input)=%8lu (%.1f n)\n",
               m, qs, (double)qs / (double)m, g_cmps, (double)g_cmps / (double)m);
        assert(qs < 6 * m);                 /* expected ~3.4n for the median */
        assert(g_cmps < 30 * m);            /* linear, with a bigger constant */
    }

    /* ---- application: p99 latency ----------------------------------------- */
    enum { S = 10000 };
    static int lat[S], lref[S];
    for (int i = 0; i < S; i++) lat[i] = lref[i] = 100 + (int)(xorshift32() % 900) + ((i % 97 == 0) ? 5000 : 0);
    size_t k99 = (size_t)(0.99 * S);
    int p99 = quickselect(lat, S, k99);
    qsort(lref, S, sizeof *lref, cmp_int);
    printf("\np99 of %d latency samples = %d us (no full sort needed)\n", S, p99);
    assert(p99 == lref[k99]);

    printf("\nAll selection checks passed.\n");
    return 0;
}
