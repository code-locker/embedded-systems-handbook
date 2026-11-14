/*
 * 04_heap_sort.c — heapsort: build a max-heap in place, then pop max to the end
 *
 * Concept : O(n log n) WORST case, O(1) extra memory, no recursion needed.
 *           Phase 1 (heapify, bottom-up): O(n) — not O(n log n)!
 *           Phase 2: swap root (max) with the last heap slot, shrink the heap,
 *           sift the new root down. Not stable, not adaptive, cache-unfriendly
 *           (jumps i -> 2i+1), so in practice ~2x slower than quicksort —
 *           but it never degrades. That is why introsort falls back to it, and
 *           why the Linux kernel's lib/sort.c IS a heapsort (bounded time,
 *           no extra memory, no recursion on a small kernel stack).
 * Build   : make run P=dsa/14-sorting-and-searching/c/04_heap_sort.c
 * Read    : ../README.md  (section "3.4 Heapsort")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 99u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void print_arr(const char *lbl, const int *a, size_t n, size_t heap_end)
{
    printf("  %-18s [", lbl);
    for (size_t i = 0; i < n; i++)
        printf("%s%s%d", i ? " " : "", (i == heap_end && i) ? "| " : "", a[i]);
    printf("]\n");
}

/* ---------------------------------------------------------------------------
 * The array IS the tree (0-based):  parent(i) = (i-1)/2,
 *                                   left(i)   = 2i+1,  right(i) = 2i+2
 *
 *   index: 0  1  2  3  4  5          a[0]=9
 *   value: 9  7  8  3  5  1         /      \
 *                                 a[1]=7  a[2]=8
 *                                 /  \     /
 *                                3    5   1
 * Max-heap property: every parent >= its children, so the max is at a[0].
 *
 * sift_down: the item at i may be smaller than a child; swap it with the
 * LARGER child and repeat until it is >= both children. O(log n).
 * We keep the moving item in a register and shift (like insertion sort)
 * instead of swapping at every level: half the writes.
 * --------------------------------------------------------------------------- */
static unsigned long g_levels;   /* how many levels items moved: the real work */

static void sift_down(int *a, size_t i, size_t n)
{
    int item = a[i];
    for (;;) {
        size_t child = 2 * i + 1;
        if (child >= n) break;                        /* leaf */
        if (child + 1 < n && a[child + 1] > a[child]) /* pick the larger child */
            child++;
        if (a[child] <= item) break;                  /* heap property holds */
        a[i] = a[child];                              /* move child up */
        g_levels++;
        i = child;
    }
    a[i] = item;
}

/* ---------------------------------------------------------------------------
 * Build heap bottom-up: leaves (the second half) are already 1-element heaps,
 * so sift down every internal node from the last one (n/2-1) back to the root.
 *
 * Why O(n)? Half the nodes are leaves (cost 0), a quarter sift 1 level, an
 * eighth 2 levels...  sum n/2^(h+1) * h  =  n * sum h/2^(h+1)  <=  n.
 * (Top-down build by repeated insert IS O(n log n): most nodes are deep.)
 * --------------------------------------------------------------------------- */
static void build_max_heap(int *a, size_t n)
{
    for (size_t i = n / 2; i-- > 0; )   /* i = n/2-1 .. 0, safe with size_t */
        sift_down(a, i, n);
}

static void heap_sort(int *a, size_t n, int trace)
{
    if (n < 2) return;
    build_max_heap(a, n);
    if (trace) print_arr("max-heap built", a, n, n);
    /*  [ heap (unsorted) | sorted tail ]
     *  swap a[0] (max) with a[end], the max is now final; shrink heap; fix root */
    for (size_t end = n - 1; end > 0; end--) {
        int t = a[0]; a[0] = a[end]; a[end] = t;
        sift_down(a, 0, end);
        if (trace) {
            char lbl[32];
            snprintf(lbl, sizeof lbl, "pop -> a[%zu]", end);
            print_arr(lbl, a, n, end);
        }
    }
}

static int is_max_heap(const int *a, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (a[(i - 1) / 2] < a[i]) return 0;
    return 1;
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void)
{
    printf("=== Heapsort ===\n\n");
    int demo[] = {4, 10, 3, 5, 1, 8, 7};
    size_t n = sizeof demo / sizeof demo[0];
    print_arr("start", demo, n, n);
    printf("  ('|' separates the shrinking heap from the sorted tail)\n");
    heap_sort(demo, n, 1);
    const int want[] = {1, 3, 4, 5, 7, 8, 10};
    assert(memcmp(demo, want, sizeof want) == 0);

    /* ---- random tests ----------------------------------------------------- */
    enum { MAXN = 3000 };
    static int a[MAXN], ref[MAXN];
    for (int trial = 0; trial < 300; trial++) {
        size_t m = xorshift32() % MAXN;
        uint32_t range = (trial & 1) ? 4u : 1000000u;
        for (size_t i = 0; i < m; i++) a[i] = ref[i] = (int)(xorshift32() % range) - 50;
        build_max_heap(a, m);
        assert(is_max_heap(a, m));
        heap_sort(a, m, 0);
        qsort(ref, m, sizeof *ref, cmp_int);
        assert(memcmp(a, ref, m * sizeof *a) == 0);
    }
    printf("\nbuild_max_heap gives a valid heap and heap_sort == qsort on 300 arrays.\n");

    /* ---- worst case is still n log n --------------------------------------
     * Count the sift-down levels on sorted, reversed and random input: they
     * stay within ~15% of each other, because heapsort ignores
     * existing order (an insertion sort would do 0 work on sorted input). */
    printf("\nheapsort is NOT adaptive — same work on sorted and random input:\n");
    enum { N = 1 << 14 };
    static int b[N];
    for (int kind = 0; kind < 3; kind++) {
        for (int i = 0; i < N; i++)
            b[i] = kind == 0 ? i : kind == 1 ? N - i : (int)(xorshift32() % 100000u);
        g_levels = 0;
        heap_sort(b, N, 0);
        for (int i = 1; i < N; i++) assert(b[i - 1] <= b[i]);
        printf("  %-8s n=%d  sift-down levels=%lu  (n*log2 n = %d)\n",
               kind == 0 ? "sorted" : kind == 1 ? "reversed" : "random", N, g_levels, N * 14);
        assert(g_levels > (unsigned long)N * 10 && g_levels < (unsigned long)N * 14);
    }

    printf("\nAll heapsort checks passed.\n");
    return 0;
}
