/*
 * 09_introsort.c — introsort: the hybrid behind std::sort
 *
 * Concept : Quicksort is fastest on average but has an O(n^2) worst case;
 *           heapsort is O(n log n) always but slower; insertion sort wins on
 *           tiny arrays. Introsort (Musser, 1997) uses all three:
 *             1. quicksort with median-of-3 pivot
 *             2. if recursion gets deeper than 2*log2(n) — the input is
 *                adversarial — switch that piece to HEAPSORT
 *             3. leave pieces of <= 16 elements unsorted, then finish with ONE
 *                insertion sort pass over the whole (now nearly-sorted) array
 *           Result: quicksort speed, heapsort guarantee. libstdc++ and libc++
 *           std::sort are introsorts (libc++ newer versions use a variant).
 * Build   : make run P=dsa/14-sorting-and-searching/c/09_introsort.c
 * Read    : ../README.md  (section "3.7 Introsort and what real libraries use")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 1997u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static unsigned long g_cmps;        /* comparisons, all phases */
static unsigned g_heap_fallbacks;   /* how often the depth limit fired */

#define LESS(x, y) (g_cmps++, (x) < (y))
#define SMALL 16

static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

/* ---- heapsort on a[0..n) (the fallback) ----------------------------------- */
static void sift_down(int *a, size_t i, size_t n)
{
    int item = a[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && LESS(a[c], a[c + 1])) c++;
        if (!LESS(item, a[c])) break;
        a[i] = a[c];
        i = c;
    }
    a[i] = item;
}
static void heap_sort(int *a, size_t n)
{
    for (size_t i = n / 2; i-- > 0; ) sift_down(a, i, n);
    for (size_t end = n - 1; end > 0; end--) { swap(&a[0], &a[end]); sift_down(a, 0, end); }
}

/* ---- insertion sort on a[0..n) -------------------------------------------- */
static void insertion_sort(int *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        int key = a[i];
        size_t j = i;
        while (j > 0 && LESS(key, a[j - 1])) { a[j] = a[j - 1]; j--; }
        a[j] = key;
    }
}

typedef enum { PIVOT_FIRST, PIVOT_MEDIAN3 } pivot_policy;

/* Put the chosen pivot at a[lo]. Median-of-3 looks at first, middle, last:
 * on sorted input it picks the true median -> perfect splits. */
static void choose_pivot(int *a, size_t lo, size_t hi, pivot_policy pol)
{
    if (pol == PIVOT_FIRST) return;              /* deliberately naive */
    size_t mid = lo + (hi - lo) / 2, last = hi - 1;
    /* order a[lo], a[mid], a[last] so the median ends in the middle */
    if (LESS(a[mid], a[lo]))   swap(&a[mid], &a[lo]);
    if (LESS(a[last], a[mid])) swap(&a[last], &a[mid]);
    if (LESS(a[mid], a[lo]))   swap(&a[mid], &a[lo]);
    swap(&a[lo], &a[mid]);                       /* median -> a[lo] */
}

/* Hoare partition with pivot = a[lo]. Returns j: a[lo..j] <= p <= a[j+1..hi).
 * With the pivot at a[lo], j is always < hi-1, so both sides are non-empty. */
static size_t partition(int *a, size_t lo, size_t hi)
{
    int p = a[lo];
    /* i starts one BEFORE lo: if lo == 0 this wraps to SIZE_MAX, and the first
     * i++ wraps back to 0 — unsigned wrap-around is well defined in C. */
    size_t i = lo - 1, j = hi;
    for (;;) {
        do i++; while (LESS(a[i], p));
        do j--; while (LESS(p, a[j]));
        if (i >= j) return j;
        swap(&a[i], &a[j]);
    }
}

/* ---------------------------------------------------------------------------
 *                     intro_loop(a, 0, n, depth = 2*log2 n)
 *                     /                                   \
 *         small side: recurse                  large side: loop (no new frame)
 *                ...                                      ...
 *         piece <= 16 ->  return (leave it for the final insertion pass)
 *         depth == 0  ->  heapsort this piece, return
 * --------------------------------------------------------------------------- */
static void intro_loop(int *a, size_t lo, size_t hi, unsigned depth, pivot_policy pol)
{
    while (hi - lo > SMALL) {
        if (depth == 0) {                        /* too deep: input is hostile */
            g_heap_fallbacks++;
            heap_sort(a + lo, hi - lo);
            return;
        }
        depth--;
        choose_pivot(a, lo, hi, pol);
        size_t j = partition(a, lo, hi);         /* [lo, j] and [j+1, hi) */
        if (j + 1 - lo < hi - (j + 1)) { intro_loop(a, lo, j + 1, depth, pol); lo = j + 1; }
        else                           { intro_loop(a, j + 1, hi, depth, pol); hi = j + 1; }
    }
}

static unsigned floor_log2(size_t n) { unsigned l = 0; while (n >>= 1) l++; return l; }

static void introsort(int *a, size_t n, pivot_policy pol)
{
    if (n < 2) return;
    intro_loop(a, 0, n, 2 * floor_log2(n), pol);
    insertion_sort(a, n);   /* every element is within its <=16 piece: O(16 n) */
}

/* Plain quicksort with the same partition but NO depth limit, for contrast.
 * Recurses on the smaller side so even the bad case does not blow the stack. */
static void plain_quick(int *a, size_t lo, size_t hi, pivot_policy pol)
{
    while (hi - lo > 1) {
        choose_pivot(a, lo, hi, pol);
        size_t j = partition(a, lo, hi);
        if (j + 1 - lo < hi - (j + 1)) { plain_quick(a, lo, j + 1, pol); lo = j + 1; }
        else                           { plain_quick(a, j + 1, hi, pol); hi = j + 1; }
    }
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void)
{
    printf("=== Introsort = quicksort + heapsort fallback + insertion finish ===\n\n");

    enum { MAXN = 4000 };
    static int a[MAXN], ref[MAXN];
    for (int t = 0; t < 300; t++) {
        size_t n = xorshift32() % MAXN;
        uint32_t range = (t % 3 == 0) ? 3u : 1000000u;
        for (size_t i = 0; i < n; i++) a[i] = (int)(xorshift32() % range);
        if (t % 5 == 0) qsort(a, n, sizeof *a, cmp_int);                  /* sorted */
        if (t % 5 == 1) for (size_t i = 0; i < n; i++) a[i] = (int)(n - i); /* reversed */
        memcpy(ref, a, n * sizeof *a);
        qsort(ref, n, sizeof *ref, cmp_int);
        introsort(a, n, (t & 1) ? PIVOT_FIRST : PIVOT_MEDIAN3);
        assert(memcmp(a, ref, n * sizeof *a) == 0);
    }
    printf("introsort == qsort on 300 random / sorted / reversed / duplicate arrays\n\n");

    /* ---- the point: bad pivots cannot hurt introsort ------------------------ */
    enum { N = 20000 };
    static int s[N];
    printf("Sorted input, n=%d, deliberately BAD pivot (first element):\n", N);

    for (int i = 0; i < N; i++) s[i] = i;
    g_cmps = 0;
    plain_quick(s, 0, N, PIVOT_FIRST);
    unsigned long plain = g_cmps;
    printf("  plain quicksort       cmps = %10lu  (~n^2/2 = %lu)\n", plain, (unsigned long)N * N / 2);

    for (int i = 0; i < N; i++) s[i] = i;
    g_cmps = 0; g_heap_fallbacks = 0;
    introsort(s, N, PIVOT_FIRST);
    unsigned long intro = g_cmps;
    printf("  introsort             cmps = %10lu  heapsort fallbacks = %u\n", intro, g_heap_fallbacks);
    for (int i = 0; i < N; i++) assert(s[i] == i);
    assert(g_heap_fallbacks >= 1);
    assert(intro * 20 < plain);       /* orders of magnitude better */

    for (int i = 0; i < N; i++) s[i] = i;
    g_cmps = 0; g_heap_fallbacks = 0;
    introsort(s, N, PIVOT_MEDIAN3);
    printf("  introsort (median-3)  cmps = %10lu  heapsort fallbacks = %u  (n log2 n = %lu)\n",
           g_cmps, g_heap_fallbacks, (unsigned long)N * 14);
    assert(g_heap_fallbacks == 0);    /* median-of-3 nails sorted input */

    printf("\nAll introsort checks passed.\n");
    return 0;
}
