/*
 * 02_merge_sort.c — merge sort: top-down (recursive) and bottom-up (iterative)
 *
 * Concept : Divide and conquer. Splitting is trivial; all the work is in
 *           MERGE: two sorted runs -> one sorted run in O(n) with two
 *           fingers. log2(n) levels x O(n) per level = O(n log n) ALWAYS.
 *           Stable (take from the LEFT run on ties). Costs O(n) extra memory.
 *           Bottom-up merges runs of width 1,2,4,... with no recursion — the
 *           shape used for linked lists (Linux list_sort) and external sorting.
 * Build   : make run P=dsa/14-sorting-and-searching/c/02_merge_sort.c
 * Read    : ../README.md  (section "3.2 Merge sort")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 2024u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void print_range(const int *a, size_t lo, size_t hi)
{
    printf("[");
    for (size_t i = lo; i < hi; i++) printf("%s%d", i > lo ? " " : "", a[i]);
    printf("]");
}

/* ---------------------------------------------------------------------------
 * MERGE a[lo..mid) and a[mid..hi) (both sorted) using tmp as scratch.
 *
 *   left  = [1 4 7]      right = [2 3 9]
 *            ^ i                  ^ j
 *   take min(left[i], right[j]) each step:
 *   out: 1 | 1 2 | 1 2 3 | 1 2 3 4 | 1 2 3 4 7 | then copy rest: 9
 *
 * '<=' takes from the LEFT on ties -> equal keys keep their order -> STABLE.
 * Using '<' here would make merge sort unstable. O(hi-lo) time.
 * --------------------------------------------------------------------------- */
static void merge(int *a, int *tmp, size_t lo, size_t mid, size_t hi)
{
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi)
        tmp[k++] = (a[i] <= a[j]) ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];   /* only one of these two loops runs */
    while (j < hi)  tmp[k++] = a[j++];
    memcpy(a + lo, tmp + lo, (hi - lo) * sizeof *a);
}

/* ---------------------------------------------------------------------------
 * TOP-DOWN: split in half, sort each half recursively, merge.
 *
 *                 [38 27 43 3 9 82 10]
 *                /                    \
 *         [38 27 43]              [3 9 82 10]
 *          /      \                /        \
 *       [38]   [27 43]         [3 9]     [82 10]
 *              /    \          /   \      /    \
 *            [27]  [43]      [3]   [9]  [82]  [10]
 *   merge up:  [27 43]          [3 9]      [10 82]
 *         [27 38 43]              [3 9 10 82]
 *                 [3 9 10 27 38 43 82]
 *
 * Recursion depth = ceil(log2 n) -> stack use is tiny and bounded.
 * --------------------------------------------------------------------------- */
static void msort_rec(int *a, int *tmp, size_t lo, size_t hi, int depth, int trace)
{
    if (hi - lo < 2) return;                 /* 0 or 1 element: already sorted */
    size_t mid = lo + (hi - lo) / 2;         /* no (lo+hi)/2 overflow */
    msort_rec(a, tmp, lo, mid, depth + 1, trace);
    msort_rec(a, tmp, mid, hi, depth + 1, trace);
    if (a[mid - 1] <= a[mid]) {              /* optimisation: runs already in order */
        if (trace) { printf("%*sskip merge (already ordered) ", depth * 2, ""); print_range(a, lo, hi); printf("\n"); }
        return;                              /* -> O(n) on sorted input */
    }
    merge(a, tmp, lo, mid, hi);
    if (trace) { printf("%*smerged ", depth * 2, ""); print_range(a, lo, hi); printf("\n"); }
}

/* Owner: this function allocates tmp once (not per call!) and frees it. */
static int merge_sort_topdown(int *a, size_t n, int trace)
{
    if (n < 2) return 0;
    int *tmp = malloc(n * sizeof *tmp);
    if (!tmp) return -1;
    msort_rec(a, tmp, 0, n, 0, trace);
    free(tmp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * BOTTOM-UP: no recursion. Treat every element as a run of width 1, then
 * merge neighbouring runs of width 1 -> 2 -> 4 -> 8 ... until width >= n.
 *
 *   width 1: [38|27|43|3|9|82|10]  -> [27 38|3 43|9 82|10]
 *   width 2: [27 38 3 43|9 82 10]  -> [3 27 38 43|9 10 82]
 *   width 4: [3 27 38 43|9 10 82]  -> [3 9 10 27 38 43 82]
 * --------------------------------------------------------------------------- */
static int merge_sort_bottomup(int *a, size_t n, int trace)
{
    if (n < 2) return 0;
    int *tmp = malloc(n * sizeof *tmp);
    if (!tmp) return -1;
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo + width < n; lo += 2 * width) {
            size_t mid = lo + width;
            size_t hi  = (mid + width < n) ? mid + width : n;  /* last run may be short */
            merge(a, tmp, lo, mid, hi);
        }
        if (trace) { printf("  after width %-3zu ", width); print_range(a, 0, n); printf("\n"); }
    }
    free(tmp);
    return 0;
}

/* ---- checking ------------------------------------------------------------- */
static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

/* Stability check needs records: sort by key, remember original position.
 * Same merge logic, on structs. */
typedef struct { int key, idx; } rec_t;

static void merge_rec(rec_t *a, rec_t *tmp, size_t lo, size_t mid, size_t hi)
{
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi)
        tmp[k++] = (a[i].key <= a[j].key) ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < hi)  tmp[k++] = a[j++];
    memcpy(a + lo, tmp + lo, (hi - lo) * sizeof *a);
}

static void msort_records(rec_t *a, rec_t *tmp, size_t lo, size_t hi)
{
    if (hi - lo < 2) return;
    size_t mid = lo + (hi - lo) / 2;
    msort_records(a, tmp, lo, mid);
    msort_records(a, tmp, mid, hi);
    merge_rec(a, tmp, lo, mid, hi);
}

int main(void)
{
    printf("=== Merge sort ===\n\n");
    int demo[] = {38, 27, 43, 3, 9, 82, 10};
    size_t n = sizeof demo / sizeof demo[0];

    int a[7];
    memcpy(a, demo, sizeof a);
    printf("Top-down trace (indent = recursion depth):\n");
    assert(merge_sort_topdown(a, n, 1) == 0);

    memcpy(a, demo, sizeof a);
    printf("\nBottom-up trace:\n  start           "); print_range(a, 0, n); printf("\n");
    assert(merge_sort_bottomup(a, n, 1) == 0);
    const int want[] = {3, 9, 10, 27, 38, 43, 82};
    assert(memcmp(a, want, sizeof want) == 0);

    /* ---- random tests ----------------------------------------------------- */
    enum { MAXN = 2000 };
    static int x[MAXN], y[MAXN], ref[MAXN];
    for (int trial = 0; trial < 300; trial++) {
        size_t m = xorshift32() % MAXN;
        uint32_t range = (trial & 1) ? 10u : 1000000u;
        for (size_t i = 0; i < m; i++) x[i] = y[i] = ref[i] = (int)(xorshift32() % range);
        qsort(ref, m, sizeof *ref, cmp_int);
        assert(merge_sort_topdown(x, m, 0) == 0);
        assert(merge_sort_bottomup(y, m, 0) == 0);
        assert(memcmp(x, ref, m * sizeof *x) == 0);
        assert(memcmp(y, ref, m * sizeof *y) == 0);
    }
    printf("\nTop-down and bottom-up agree with qsort on 300 random arrays.\n");

    /* ---- stability -------------------------------------------------------- */
    enum { RN = 1000 };
    static rec_t r[RN], rt[RN];
    for (int i = 0; i < RN; i++) { r[i].key = (int)(xorshift32() % 10); r[i].idx = i; }
    msort_records(r, rt, 0, RN);
    for (int i = 1; i < RN; i++) {
        assert(r[i - 1].key <= r[i].key);
        if (r[i - 1].key == r[i].key) assert(r[i - 1].idx < r[i].idx);   /* stable */
    }
    printf("Stability: 1000 records with 10 distinct keys keep original order. OK\n");

    printf("\nAll merge sort checks passed.\n");
    return 0;
}
