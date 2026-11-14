/*
 * 03_quick_sort.c — quicksort: Lomuto and Hoare partitions, random pivot,
 *                   3-way partition, and a live demo of the O(n^2) worst case
 *
 * Concept : Pick a pivot, PARTITION so smaller items go left and larger go
 *           right, recurse on both sides. All work is in partition; there is
 *           no merge step. Balanced splits -> log n levels -> O(n log n).
 *           Always-terrible splits (sorted input + "first/last element" pivot)
 *           -> n levels -> O(n^2) comparisons and O(n) recursion depth.
 *           Random pivot makes the bad case astronomically unlikely; recursing
 *           on the SMALLER side first caps stack depth at O(log n).
 * Build   : make run P=dsa/14-sorting-and-searching/c/03_quick_sort.c
 * Read    : ../README.md  (section "3.3 Quicksort")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 777u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static unsigned long g_cmps;     /* comparison counter for the experiments */
static int g_depth, g_max_depth; /* recursion depth tracking */

static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

static void print_arr(const char *lbl, const int *a, int lo, int hi)
{
    printf("  %-26s [", lbl);
    for (int i = lo; i <= hi; i++) printf("%s%d", i > lo ? " " : "", a[i]);
    printf("]\n");
}

/* ---------------------------------------------------------------------------
 * LOMUTO partition (pivot = a[hi]).  Easy to remember, used in textbooks.
 *
 * Invariant while j scans left -> right:
 *     a[lo..i-1]  < pivot  |  a[i..j-1] >= pivot  |  a[j..hi-1] unknown | pivot
 *
 *   [7 2 1 6 8 5 3 | 4]   pivot = 4
 *    j=7: >=4 skip;  j=2: <4 swap a[i=0]<->a[j] -> [2 7 1 6 8 5 3 4], i=1
 *    j=1: swap a[1]<->a[2] -> [2 1 7 6 8 5 3 4], i=2   ... j=3: -> [2 1 3 6 8 5 7 4] i=3
 *   finally swap pivot into a[i]:  [2 1 3 | 4 | 8 5 7 6]
 *
 * Returns the pivot's FINAL index p: a[p] is in its sorted position forever.
 * Weakness: many swaps, and ALL-EQUAL input gives (n-1, 0) splits -> O(n^2).
 * --------------------------------------------------------------------------- */
static int partition_lomuto(int *a, int lo, int hi)
{
    int pivot = a[hi];
    int i = lo;
    for (int j = lo; j < hi; j++) {
        g_cmps++;
        if (a[j] < pivot) swap(&a[i++], &a[j]);
    }
    swap(&a[i], &a[hi]);
    return i;
}

/* ---------------------------------------------------------------------------
 * HOARE partition (pivot = middle value). The original, ~3x fewer swaps.
 * Two fingers walk inward; each stops at an element on the WRONG side;
 * swap them; continue until they cross.
 *
 *   [7 2 1 6 8 5 3 4]  pivot = a[3] = 6
 *    i-> stops at 7 (>=6)          j<- stops at 4 (<=6)  swap -> [4 2 1 6 8 5 3 7]
 *    i-> stops at 6                j<- stops at 3        swap -> [4 2 1 3 8 5 6 7]
 *    i-> stops at 8                j<- stops at 5        swap -> [4 2 1 3 5 8 6 7]
 *    i-> stops at 8 (idx5)         j<- stops at 5 (idx4) crossed -> return j=4
 *   result: a[lo..4] <= 6 <= a[5..hi]   (pivot NOT necessarily at its final spot!)
 *
 * Recurse on [lo..j] and [j+1..hi] — NOT [lo..j-1]: a classic bug source.
 * Stopping on EQUAL keys (>= / <=) is deliberate: on all-equal input the
 * fingers meet in the middle -> balanced splits instead of O(n^2).
 * --------------------------------------------------------------------------- */
static int partition_hoare(int *a, int lo, int hi)
{
    int pivot = a[lo + (hi - lo) / 2];
    int i = lo - 1, j = hi + 1;
    for (;;) {
        do { i++; g_cmps++; } while (a[i] < pivot);
        do { j--; g_cmps++; } while (a[j] > pivot);
        if (i >= j) return j;
        swap(&a[i], &a[j]);
    }
}

/* ---- the sorts ----------------------------------------------------------- */

/* Naive Lomuto quicksort: pivot = last element. Recursion depth = number of
 * nested partitions, which is n on sorted input. */
static void quick_lomuto(int *a, int lo, int hi)
{
    if (lo >= hi) return;
    if (++g_depth > g_max_depth) g_max_depth = g_depth;
    int p = partition_lomuto(a, lo, hi);
    quick_lomuto(a, lo, p - 1);      /* pivot excluded: it is final */
    quick_lomuto(a, p + 1, hi);
    g_depth--;
}

static void quick_hoare(int *a, int lo, int hi)
{
    if (lo >= hi) return;
    if (++g_depth > g_max_depth) g_max_depth = g_depth;
    int p = partition_hoare(a, lo, hi);
    quick_hoare(a, lo, p);           /* p INCLUDED on the left side */
    quick_hoare(a, p + 1, hi);
    g_depth--;
}

/* Randomised pivot + recurse on the smaller side, LOOP on the larger side.
 * This is "manual tail-call elimination": the larger half never gets a new
 * stack frame, so depth <= log2(n) even in the worst case. */
static void quick_random(int *a, int lo, int hi)
{
    while (lo < hi) {
        if (++g_depth > g_max_depth) g_max_depth = g_depth;
        int r = lo + (int)(xorshift32() % (uint32_t)(hi - lo + 1));
        swap(&a[r], &a[hi]);                 /* move random pivot to the end */
        int p = partition_lomuto(a, lo, hi);
        if (p - lo < hi - p) {
            quick_random(a, lo, p - 1);      /* smaller side: recurse */
            lo = p + 1;                      /* larger side: iterate */
        } else {
            quick_random(a, p + 1, hi);
            hi = p - 1;
        }
        g_depth--;
    }
}

/* ---------------------------------------------------------------------------
 * 3-WAY partition (Dijkstra's Dutch national flag): < pivot | == pivot | > pivot
 *
 *   lt        i            gt
 *   [ <p  | =p  | unknown | >p ]
 *
 * The '== pivot' block is final and NEVER recursed into, so arrays with
 * many duplicates (e.g. 8-bit pixel values!) sort in ~O(n * distinct).
 * --------------------------------------------------------------------------- */
static void quick_3way(int *a, int lo, int hi)
{
    if (lo >= hi) return;
    int r = lo + (int)(xorshift32() % (uint32_t)(hi - lo + 1));
    int pivot = a[r];
    int lt = lo, i = lo, gt = hi;
    while (i <= gt) {
        g_cmps++;
        if (a[i] < pivot)      swap(&a[lt++], &a[i++]);
        else if (a[i] > pivot) swap(&a[i], &a[gt--]);   /* don't i++: new a[i] unseen */
        else                   i++;
    }
    quick_3way(a, lo, lt - 1);
    quick_3way(a, gt + 1, hi);
}

/* ---- checking ------------------------------------------------------------- */
static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

typedef void (*qs_fn)(int *, int, int);

static void random_test(const char *name, qs_fn fn)
{
    enum { MAXN = 3000 };
    static int a[MAXN], ref[MAXN];
    for (int trial = 0; trial < 300; trial++) {
        int n = (int)(xorshift32() % MAXN);
        uint32_t range = (trial % 3 == 0) ? 3u : 100000u;
        for (int i = 0; i < n; i++) a[i] = ref[i] = (int)(xorshift32() % range);
        if (trial % 7 == 0) qsort(a, (size_t)n, sizeof *a, cmp_int);  /* sorted input too */
        memcpy(ref, a, (size_t)n * sizeof *a);
        qsort(ref, (size_t)n, sizeof *ref, cmp_int);
        fn(a, 0, n - 1);
        assert(memcmp(a, ref, (size_t)n * sizeof *a) == 0);
    }
    printf("  %-13s OK on 300 random/sorted/duplicate-heavy arrays\n", name);
}

static void measure(const char *name, qs_fn fn, int *a, int n)
{
    g_cmps = 0; g_depth = 0; g_max_depth = 0;
    fn(a, 0, n - 1);
    printf("  %-13s cmps=%9lu  max recursion depth=%5d\n", name, g_cmps, g_max_depth);
    for (int i = 1; i < n; i++) assert(a[i - 1] <= a[i]);
}

int main(void)
{
    printf("=== Quicksort ===\n\n");

    int d1[] = {7, 2, 1, 6, 8, 5, 3, 4};
    printf("Lomuto partition, pivot = last (4):\n");
    print_arr("before", d1, 0, 7);
    int p = partition_lomuto(d1, 0, 7);
    print_arr("after (pivot at index 3)", d1, 0, 7);
    assert(p == 3 && d1[3] == 4);
    for (int i = 0; i < p; i++) assert(d1[i] < 4);
    for (int i = p + 1; i < 8; i++) assert(d1[i] >= 4);

    int d2[] = {7, 2, 1, 6, 8, 5, 3, 4};
    printf("\nHoare partition, pivot = middle value (6):\n");
    print_arr("before", d2, 0, 7);
    int q = partition_hoare(d2, 0, 7);
    print_arr("after (split after idx 4)", d2, 0, 7);
    printf("  returned j=%d: a[0..%d] <= 6 <= a[%d..7]\n", q, q, q + 1);
    for (int i = 0; i <= q; i++) assert(d2[i] <= 6);
    for (int i = q + 1; i < 8; i++) assert(d2[i] >= 6);

    printf("\nRandom tests against qsort():\n");
    random_test("lomuto", quick_lomuto);
    random_test("hoare", quick_hoare);
    random_test("random-pivot", quick_random);
    random_test("3-way", quick_3way);

    /* ---- why the worst case is O(n^2) -------------------------------------
     * Sorted input, pivot = last element = the MAXIMUM. Partition puts
     * everything on the left: sizes n-1, n-2, ... 1.
     *   comparisons = (n-1) + (n-2) + ... + 1 = n(n-1)/2
     *   depth       = n-1 frames -> on a 8 KB embedded stack this CRASHES.
     */
    enum { N = 2000 };
    static int a[N];
    printf("\nWorst case demo, n=%d, input ALREADY SORTED:\n", N);
    for (int i = 0; i < N; i++) a[i] = i;
    measure("lomuto(last)", quick_lomuto, a, N);
    printf("  -> n(n-1)/2 = %d, exactly the quadratic sum\n", N * (N - 1) / 2);
    assert(g_cmps == (unsigned long)N * (N - 1) / 2);
    assert(g_max_depth == N - 1);
    for (int i = 0; i < N; i++) a[i] = i;
    measure("hoare(middle)", quick_hoare, a, N);
    for (int i = 0; i < N; i++) a[i] = i;
    measure("random-pivot", quick_random, a, N);
    assert(g_max_depth <= 11);   /* log2(2000) ~ 11: smaller-side-first bound */

    printf("\nAll-equal input (n=%d), the OTHER classic trap:\n", N);
    for (int i = 0; i < N; i++) a[i] = 42;
    measure("lomuto(last)", quick_lomuto, a, N);         /* '<' never true -> (n-1,0) splits */
    assert(g_cmps == (unsigned long)N * (N - 1) / 2);
    for (int i = 0; i < N; i++) a[i] = 42;
    measure("hoare(middle)", quick_hoare, a, N);          /* stops on equals -> balanced */
    assert(g_cmps < 40000);
    for (int i = 0; i < N; i++) a[i] = 42;
    g_cmps = 0; quick_3way(a, 0, N - 1);
    printf("  %-13s cmps=%9lu  (one pass: everything == pivot)\n", "3-way", g_cmps);
    assert(g_cmps == N);

    printf("\nAll quicksort checks passed.\n");
    return 0;
}
