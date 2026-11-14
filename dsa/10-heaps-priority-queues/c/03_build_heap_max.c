/*
 * 03_build_heap_max.c — max-heap and build-heap in O(n) (Floyd's method)
 *
 * Concept : Turning n arbitrary values into a heap by n pushes costs O(n log n).
 *           Sifting DOWN every internal node from the last one (n/2-1) back to
 *           the root costs only O(n), because most nodes are near the bottom
 *           and sink only a little. This program counts swaps to show it.
 * Build   : make run P=dsa/10-heaps-priority-queues/c/03_build_heap_max.c
 * Read    : ../README.md  (section "Build-heap in O(n) and why")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static unsigned long g_swaps;   /* instrumentation only */

static void swap(int *x, int *y) { int t = *x; *x = *y; *y = t; g_swaps++; }

/* MAX-heap: same code as the min-heap with the comparisons flipped. */
static void sift_up_max(int *a, size_t i)            /* O(log n) */
{
    while (i > 0 && a[(i - 1) / 2] < a[i]) {
        swap(&a[(i - 1) / 2], &a[i]);
        i = (i - 1) / 2;
    }
}

static void sift_down_max(int *a, size_t n, size_t i) /* O(height of i) */
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && a[l] > a[m]) m = l;     /* pick the LARGER child */
        if (r < n && a[r] > a[m]) m = r;
        if (m == i) return;
        swap(&a[i], &a[m]);
        i = m;
    }
}

/*
 * build_heap_max — O(n).
 * Leaves (indices n/2..n-1) are already 1-element heaps, so start at n/2-1
 * and go BACKWARDS: when we sift node i down, both its subtrees are heaps.
 *
 *   [3,9,2,1,4,5]        i=2: 2 vs child 5   i=1: 9 ok        i=0: 3 sinks
 *
 *        3                   3                   3                9
 *       / \                 / \                 / \              / \
 *      9   2      ->       9   5      ->       9   5     ->     4   5
 *     / \  /              / \  /              / \  /           / \  /
 *    1  4 5              1  4 2              1  4 2           1  3 2
 *
 * Why O(n): about n/4 nodes can sink 1 level, n/8 sink 2, n/16 sink 3...
 *   total <= n * (1/4 + 2/8 + 3/16 + ...) = n * 1 = O(n)
 * Whereas n pushes: half the nodes are leaves that may climb log n levels.
 */
static void build_heap_max(int *a, size_t n)
{
    for (size_t i = n / 2; i-- > 0;)   /* i = n/2-1 down to 0, unsigned-safe */
        sift_down_max(a, n, i);
}

static void build_by_pushes(int *a, size_t n)        /* O(n log n) */
{
    for (size_t i = 1; i < n; i++)
        sift_up_max(a, i);             /* "push" a[i] onto heap a[0..i-1] */
}

static bool is_max_heap(const int *a, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (a[(i - 1) / 2] < a[i]) return false;
    return true;
}

static int pop_max(int *a, size_t *n)                  /* O(log n) */
{
    int top = a[0];
    a[0] = a[--*n];
    sift_down_max(a, *n, 0);
    return top;
}

static int cmp_desc(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a < b) - (a > b);
}

int main(void)
{
    int demo[] = {3, 9, 2, 1, 4, 5};
    build_heap_max(demo, 6);
    printf("build_heap_max([3,9,2,1,4,5]) =");
    for (int i = 0; i < 6; i++) printf(" %d", demo[i]);
    printf("\n");
    assert(is_max_heap(demo, 6) && demo[0] == 9);

    /* Worst case for push-building: ascending input, every new item climbs
     * to the root. Compare swap counts. */
    size_t n = 1u << 16;
    int *a = malloc(n * sizeof *a), *b = malloc(n * sizeof *b);
    assert(a && b);
    for (size_t i = 0; i < n; i++) a[i] = b[i] = (int)i;

    g_swaps = 0; build_heap_max(a, n);
    unsigned long floyd = g_swaps;
    g_swaps = 0; build_by_pushes(b, n);
    unsigned long pushes = g_swaps;
    printf("n=%zu ascending input: Floyd swaps=%lu (<= n), push swaps=%lu (~n log n)\n",
           n, floyd, pushes);
    assert(is_max_heap(a, n) && is_max_heap(b, n));
    assert(floyd <= n);                 /* the O(n) bound, concretely */
    assert(pushes > 10 * floyd);

    /* Randomised: build, then pop everything -> must be descending sorted. */
    srand(7);
    for (int round = 0; round < 100; round++) {
        size_t m = (size_t)(rand() % 500) + 1;
        for (size_t i = 0; i < m; i++) a[i] = b[i] = rand() % 200;
        build_heap_max(a, m);
        assert(is_max_heap(a, m));
        qsort(b, m, sizeof *b, cmp_desc);
        size_t len = m;
        for (size_t i = 0; i < m; i++) assert(pop_max(a, &len) == b[i]);
    }
    printf("100 random rounds: build + pop-all == descending qsort\n");

    free(a);
    free(b);
    printf("all checks passed\n");
    return 0;
}
