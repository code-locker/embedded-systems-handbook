/*
 * 02_min_heap.c — growable min-heap of ints: push, pop, peek, sift-up, sift-down
 *
 * Concept : push = append at the end + sift-up; pop = move last element to the
 *           root + sift-down. Both walk one root-to-leaf path: O(log n).
 *           Randomised self-check: pushing N values and popping them all must
 *           give the same sequence as qsort().
 * Build   : make run P=dsa/10-heaps-priority-queues/c/02_min_heap.c
 * Read    : ../README.md  (sections "Sift-up" and "Sift-down")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int    *a;    /* owned: malloc'd/realloc'd by heap_push, freed by heap_free */
    size_t  len;
    size_t  cap;
} MinHeap;

static void heap_init(MinHeap *h) { h->a = NULL; h->len = h->cap = 0; }
static void heap_free(MinHeap *h) { free(h->a); heap_init(h); }

static void swap(int *x, int *y) { int t = *x; *x = *y; *y = t; }

/*
 * sift_up — O(log n): the new element climbs while it is smaller than its parent.
 *
 *   push 0 into [1,3,2,7,4]:
 *
 *        1              1               0
 *       / \            / \             / \
 *      3   2   ->     3   0    ->     3   1
 *     / \ /          / \ /           / \ /
 *    7  4 0         7  4 2          7  4 2
 *   (append)      (0<2 swap)      (0<1 swap, now root: stop)
 */
static void sift_up(int *a, size_t i)
{
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (a[p] <= a[i])
            break;              /* parent already smaller: property restored */
        swap(&a[p], &a[i]);
        i = p;
    }
}

/*
 * sift_down — O(log n): the element sinks, swapping with its SMALLER child.
 * Why the smaller child? It becomes the parent of the other child, so it must
 * be <= that child. Swapping with the larger one would break the property.
 *
 *   pop from [0,3,1,7,4,2]: move last (2) to root, then sink
 *
 *        2               1
 *       / \             / \
 *      3   1    ->     3   2
 *     / \             / \
 *    7   4           7   4
 *   (smaller child is 1: swap)  (2 has no children: stop)
 */
static void sift_down(int *a, size_t n, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && a[l] < a[m]) m = l;
        if (r < n && a[r] < a[m]) m = r;
        if (m == i)
            break;              /* both children >= me: done */
        swap(&a[i], &a[m]);
        i = m;
    }
}

/* O(log n) amortised: realloc doubles, so copying is O(1) per push on average. */
static bool heap_push(MinHeap *h, int v)
{
    if (h->len == h->cap) {
        size_t ncap = h->cap ? h->cap * 2 : 8;
        int *na = realloc(h->a, ncap * sizeof *na);
        if (!na)
            return false;       /* old buffer still valid and owned by h */
        h->a = na;
        h->cap = ncap;
    }
    h->a[h->len] = v;
    sift_up(h->a, h->len);
    h->len++;
    return true;
}

/* O(1): the minimum is always at index 0. */
static int heap_peek(const MinHeap *h) { assert(h->len > 0); return h->a[0]; }

/* O(log n) */
static int heap_pop(MinHeap *h)
{
    assert(h->len > 0);
    int top = h->a[0];
    h->a[0] = h->a[--h->len];   /* last leaf fills the hole: tree stays complete */
    sift_down(h->a, h->len, 0);
    return top;
}

static bool valid(const MinHeap *h)
{
    for (size_t i = 1; i < h->len; i++)
        if (h->a[(i - 1) / 2] > h->a[i])
            return false;
    return true;
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);   /* never return a-b: it can overflow */
}

int main(void)
{
    MinHeap h;
    heap_init(&h);

    int demo[] = {5, 3, 8, 1, 9, 2, 7};
    for (size_t i = 0; i < sizeof demo / sizeof demo[0]; i++) {
        assert(heap_push(&h, demo[i]));
        printf("push %d -> min=%d  array:", demo[i], heap_peek(&h));
        for (size_t k = 0; k < h.len; k++) printf(" %d", h.a[k]);
        printf("\n");
        assert(valid(&h));
    }
    printf("pop order:");
    while (h.len) printf(" %d", heap_pop(&h));
    printf("\n");

    /* Randomised check: interleave pushes and pops, compare with a sorted
     * reference. Also confirm the property after every operation. */
    srand(12345);
    for (int round = 0; round < 200; round++) {
        size_t n = (size_t)(rand() % 300);
        int *ref = malloc((n ? n : 1) * sizeof *ref);
        assert(ref);
        for (size_t i = 0; i < n; i++) {
            ref[i] = rand() % 1000 - 500;          /* duplicates on purpose */
            assert(heap_push(&h, ref[i]));
            assert(valid(&h));
        }
        qsort(ref, n, sizeof *ref, cmp_int);
        for (size_t i = 0; i < n; i++) {
            assert(heap_peek(&h) == ref[i]);
            assert(heap_pop(&h) == ref[i]);
            assert(valid(&h));
        }
        assert(h.len == 0);
        free(ref);
    }
    printf("200 random rounds: heap pop order == qsort order\n");

    heap_free(&h);
    printf("all checks passed\n");
    return 0;
}
