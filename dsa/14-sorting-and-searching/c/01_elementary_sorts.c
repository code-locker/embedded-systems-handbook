/*
 * 01_elementary_sorts.c — bubble, selection and insertion sort, traced pass by pass
 *
 * Concept : The three O(n^2) sorts. Each keeps a growing SORTED region and a
 *           shrinking UNSORTED region; they differ only in HOW one element
 *           crosses the border. Insertion sort is the one that matters in
 *           practice: adaptive (O(n) on sorted input), stable, in-place, and
 *           the fastest sort for tiny arrays (used inside introsort/timsort).
 * Build   : make run P=dsa/14-sorting-and-searching/c/01_elementary_sorts.c
 * Read    : ../README.md  (section "3.1 The elementary sorts")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- tiny deterministic RNG so every run is reproducible ----------------- */
static uint32_t rng_state = 12345u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void print_array(const char *label, const int *a, size_t n)
{
    printf("  %-14s [", label);
    for (size_t i = 0; i < n; i++)
        printf("%s%d", i ? " " : "", a[i]);
    printf("]\n");
}

/* Counters so we can SEE the difference between the algorithms. */
typedef struct { unsigned long cmps, swaps; } stats_t;

/* ---------------------------------------------------------------------------
 * BUBBLE SORT — "the biggest bubble rises to the top each pass".
 *
 *   pass 1: compare neighbours left->right, swap if out of order.
 *           After the pass the maximum is at a[n-1] (it was carried along).
 *   pass 2: same, but stop one earlier (a[n-1] is final) ...
 *
 *   [5 1 4 2 8]  ->  [1 4 2 5 | 8]  ->  [1 2 4 | 5 8]  -> no swaps -> done
 *
 * Early exit: if a whole pass did no swap, the array is sorted -> O(n) best.
 * Stable: we only swap on a[j] > a[j+1] (strict), equal keys never cross.
 * --------------------------------------------------------------------------- */
static void bubble_sort(int *a, size_t n, stats_t *st, int trace)
{
    if (n < 2) return;
    for (size_t pass = 0; pass < n - 1; pass++) {
        int swapped = 0;
        /* a[n-1-pass .. n-1] already holds the largest 'pass' items. */
        for (size_t j = 0; j + 1 < n - pass; j++) {
            st->cmps++;
            if (a[j] > a[j + 1]) {             /* '>' not '>=' -> stable */
                int t = a[j]; a[j] = a[j + 1]; a[j + 1] = t;
                st->swaps++;
                swapped = 1;
            }
        }
        if (trace) {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "after pass %zu", pass + 1);
            print_array(lbl, a, n);
        }
        if (!swapped) break;                    /* adaptive: sorted already */
    }
}

/* ---------------------------------------------------------------------------
 * SELECTION SORT — "find the smallest, put it at the front".
 *
 *   [5 1 4 2 8]  min of all = 1  -> swap with a[0] -> [1 | 5 4 2 8]
 *   [1 | 5 4 2 8] min of rest = 2 -> swap with a[1] -> [1 2 | 4 5 8] ...
 *
 * Always n(n-1)/2 comparisons (not adaptive) but at most n-1 swaps: the
 * sort of choice when WRITES are very expensive (e.g. EEPROM/flash cells).
 * NOT stable: the long-distance swap can jump an element over its equal twin:
 *   [2a 2b 1] -> min=1 swaps with 2a -> [1 2b 2a]   (2a now after 2b!)
 * --------------------------------------------------------------------------- */
static void selection_sort(int *a, size_t n, stats_t *st, int trace)
{
    for (size_t i = 0; i + 1 < n; i++) {
        size_t min = i;
        for (size_t j = i + 1; j < n; j++) {
            st->cmps++;
            if (a[j] < a[min]) min = j;
        }
        if (min != i) {
            int t = a[i]; a[i] = a[min]; a[min] = t;
            st->swaps++;
        }
        if (trace) {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "after i=%zu", i);
            print_array(lbl, a, n);
        }
    }
}

/* ---------------------------------------------------------------------------
 * INSERTION SORT — "sorting a hand of playing cards".
 *
 * Take the next card (key), slide bigger cards in the sorted part one slot
 * right, drop the key into the hole.  We SHIFT, not swap: 1 write per move.
 *
 *   sorted | unsorted
 *   [1 4 5 | 2 8]   key = 2
 *   [1 4 5 5 8]     5 > 2 -> shift right
 *   [1 4 4 5 8]     4 > 2 -> shift right
 *   [1 2 4 5 8]     1 <= 2 -> stop, drop key into the hole
 *
 * Inner loop runs once per INVERSION (pair i<j with a[i]>a[j]), so the cost
 * is O(n + inversions): O(n) on nearly-sorted input. That is why real-world
 * sorts finish small/nearly-sorted pieces with insertion sort.
 * --------------------------------------------------------------------------- */
static void insertion_sort(int *a, size_t n, stats_t *st, int trace)
{
    for (size_t i = 1; i < n; i++) {
        int key = a[i];
        size_t j = i;
        /* j is size_t: test j > 0 BEFORE reading a[j-1] (no underflow). */
        while (j > 0) {
            st->cmps++;
            if (a[j - 1] <= key) break;         /* '<=' stops at equals -> stable */
            a[j] = a[j - 1];                    /* shift right, one write */
            st->swaps++;
            j--;
        }
        a[j] = key;
        if (trace) {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "insert a[%zu]", i);
            print_array(lbl, a, n);
        }
    }
}

/* ---- reference + checking ------------------------------------------------- */
static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);   /* NOT a - b: that can overflow for big values */
}

typedef void (*sort_fn)(int *, size_t, stats_t *, int);

static void check_against_qsort(const char *name, sort_fn fn)
{
    enum { MAXN = 200 };
    int a[MAXN], ref[MAXN];
    for (int trial = 0; trial < 300; trial++) {
        size_t n = xorshift32() % MAXN;          /* includes n = 0 and 1 */
        int range = (trial % 3 == 0) ? 5 : 1000; /* small range -> many dups */
        for (size_t i = 0; i < n; i++)
            a[i] = (int)(xorshift32() % (uint32_t)range) - range / 2;
        memcpy(ref, a, n * sizeof *a);
        qsort(ref, n, sizeof *ref, cmp_int);
        stats_t st = {0, 0};
        fn(a, n, &st, 0);
        assert(memcmp(a, ref, n * sizeof *a) == 0);
    }
    printf("  %-15s OK on 300 random arrays (n = 0..199, with duplicates)\n", name);
}

/* Stability can only be seen when records carry more than the key, so we
 * sort (key, original index) pairs by key only. Stable = equal keys keep
 * their original index order. Pair versions of insertion and selection: */
typedef struct { int key; int idx; } pair_t;

static void insertion_pairs(pair_t *p, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        pair_t k = p[i];
        size_t j = i;
        while (j > 0 && p[j - 1].key > k.key) { p[j] = p[j - 1]; j--; }
        p[j] = k;
    }
}

static void selection_pairs(pair_t *p, size_t n)
{
    for (size_t i = 0; i + 1 < n; i++) {
        size_t m = i;
        for (size_t j = i + 1; j < n; j++)
            if (p[j].key < p[m].key) m = j;
        pair_t t = p[i]; p[i] = p[m]; p[m] = t;
    }
}

static int is_stable(const pair_t *p, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (p[i - 1].key == p[i].key && p[i - 1].idx > p[i].idx) return 0;
    return 1;
}

int main(void)
{
    printf("=== Elementary O(n^2) sorts ===\n\n");

    const int demo[] = {5, 1, 4, 2, 8};
    const size_t n = sizeof demo / sizeof demo[0];
    int a[5];

    printf("Bubble sort (largest bubbles to the right each pass):\n");
    memcpy(a, demo, sizeof a);
    print_array("start", a, n);
    stats_t sb = {0, 0}; bubble_sort(a, n, &sb, 1);
    printf("  cmps=%lu swaps=%lu\n\n", sb.cmps, sb.swaps);

    printf("Selection sort (pick min, swap to front):\n");
    memcpy(a, demo, sizeof a);
    print_array("start", a, n);
    stats_t ss = {0, 0}; selection_sort(a, n, &ss, 1);
    printf("  cmps=%lu swaps=%lu\n\n", ss.cmps, ss.swaps);

    printf("Insertion sort (card hand: shift bigger ones right):\n");
    memcpy(a, demo, sizeof a);
    print_array("start", a, n);
    stats_t si = {0, 0}; insertion_sort(a, n, &si, 1);
    printf("  cmps=%lu shifts=%lu\n\n", si.cmps, si.swaps);

    /* ---- adaptivity: already-sorted input --------------------------------- */
    printf("Adaptivity on an ALREADY SORTED array of 1000:\n");
    enum { N = 1000 };
    static int big[N];
    for (int i = 0; i < N; i++) big[i] = i;
    stats_t b1 = {0, 0}, b2 = {0, 0}, b3 = {0, 0};
    bubble_sort(big, N, &b1, 0);
    selection_sort(big, N, &b2, 0);
    insertion_sort(big, N, &b3, 0);
    printf("  bubble    cmps=%6lu  (early exit after 1 pass -> n-1)\n", b1.cmps);
    printf("  selection cmps=%6lu  (always n(n-1)/2 = %d)\n", b2.cmps, N * (N - 1) / 2);
    printf("  insertion cmps=%6lu  (n-1: each key stops immediately)\n\n", b3.cmps);
    assert(b1.cmps == N - 1);
    assert(b2.cmps == (unsigned long)N * (N - 1) / 2);
    assert(b3.cmps == N - 1);

    /* Reverse-sorted = worst case: every pair is an inversion. */
    for (int i = 0; i < N; i++) big[i] = N - i;
    stats_t w = {0, 0};
    insertion_sort(big, N, &w, 0);
    printf("Insertion sort on REVERSED input: shifts=%lu = n(n-1)/2 inversions\n\n", w.swaps);
    assert(w.swaps == (unsigned long)N * (N - 1) / 2);

    /* ---- stability -------------------------------------------------------- */
    printf("Stability ([2a 2b 1] style test):\n");
    pair_t p1[] = {{2, 0}, {2, 1}, {1, 2}};
    pair_t p2[] = {{2, 0}, {2, 1}, {1, 2}};
    insertion_pairs(p1, 3);
    selection_pairs(p2, 3);
    printf("  insertion -> (%d,%d) (%d,%d) (%d,%d)  stable=%d\n",
           p1[0].key, p1[0].idx, p1[1].key, p1[1].idx, p1[2].key, p1[2].idx, is_stable(p1, 3));
    printf("  selection -> (%d,%d) (%d,%d) (%d,%d)  stable=%d  <- the 2s swapped order!\n\n",
           p2[0].key, p2[0].idx, p2[1].key, p2[1].idx, p2[2].key, p2[2].idx, is_stable(p2, 3));
    assert(is_stable(p1, 3));
    assert(!is_stable(p2, 3));

    /* ---- random tests against qsort --------------------------------------- */
    printf("Random tests against qsort():\n");
    check_against_qsort("bubble_sort", bubble_sort);
    check_against_qsort("selection_sort", selection_sort);
    check_against_qsort("insertion_sort", insertion_sort);

    printf("\nAll elementary sort checks passed.\n");
    return 0;
}
