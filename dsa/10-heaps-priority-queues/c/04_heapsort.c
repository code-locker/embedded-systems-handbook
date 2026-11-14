/*
 * 04_heapsort.c — in-place heapsort: build a max-heap, then repeatedly swap the
 *                 max to the end and shrink the heap
 *
 * Concept : O(n log n) worst case, O(1) extra memory, not stable. That
 *           combination is why the Linux kernel's lib/sort.c is a heapsort and
 *           why introsort (std::sort) falls back to heapsort when quicksort
 *           recursion gets too deep.
 * Build   : make run P=dsa/10-heaps-priority-queues/c/04_heapsort.c
 * Read    : ../README.md  (section "Heapsort")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sift_down(int *a, size_t n, size_t i)    /* O(log n) */
{
    int v = a[i];                 /* "hole" technique: carry the value down and */
    for (;;) {                    /* write it once, instead of swapping each level */
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && a[c + 1] > a[c]) c++;   /* larger child */
        if (a[c] <= v) break;
        a[i] = a[c];              /* move child up into the hole */
        i = c;
    }
    a[i] = v;
}

/*
 * heapsort — O(n log n) time, O(1) space.
 *
 *   after build:   [9 5 8 1 3 2]   heap = whole array
 *   swap 9 to end: [2 5 8 1 3 | 9] sift 2 down -> [8 5 2 1 3 | 9]
 *   swap 8 to end: [3 5 2 1 | 8 9] sift 3 down -> [5 3 2 1 | 8 9]
 *   ...            the sorted tail grows from the right
 */
static void heapsort_int(int *a, size_t n)
{
    for (size_t i = n / 2; i-- > 0;)     /* O(n) build-heap */
        sift_down(a, n, i);
    for (size_t end = n; end > 1; end--) {   /* n-1 times O(log n) */
        int t = a[0]; a[0] = a[end - 1]; a[end - 1] = t;
        sift_down(a, end - 1, 0);
    }
}

/* Generic version with a comparator, same shape as qsort / kernel sort(). */
typedef int (*cmp_fn)(const void *, const void *);

static void swap_bytes(unsigned char *x, unsigned char *y, size_t sz)
{
    while (sz--) { unsigned char t = *x; *x++ = *y; *y++ = t; }
}

static void sift_down_g(unsigned char *base, size_t n, size_t i, size_t sz, cmp_fn cmp)
{
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) return;
        if (c + 1 < n && cmp(base + (c + 1) * sz, base + c * sz) > 0) c++;
        if (cmp(base + c * sz, base + i * sz) <= 0) return;
        swap_bytes(base + i * sz, base + c * sz, sz);
        i = c;
    }
}

static void heapsort_generic(void *base_, size_t n, size_t sz, cmp_fn cmp)
{
    unsigned char *base = base_;
    for (size_t i = n / 2; i-- > 0;) sift_down_g(base, n, i, sz, cmp);
    for (size_t end = n; end > 1; end--) {
        swap_bytes(base, base + (end - 1) * sz, sz);
        sift_down_g(base, end - 1, 0, sz, cmp);
    }
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

typedef struct { char name[8]; double score; } Rec;

static int cmp_rec_score(const void *x, const void *y)
{
    double a = ((const Rec *)x)->score, b = ((const Rec *)y)->score;
    return (a > b) - (a < b);
}

int main(void)
{
    int demo[] = {3, 9, 2, 1, 5, 8};
    heapsort_int(demo, 6);
    printf("heapsort([3,9,2,1,5,8]) =");
    for (int i = 0; i < 6; i++) printf(" %d", demo[i]);
    printf("\n");

    srand(42);
    int *a = malloc(5000 * sizeof *a), *b = malloc(5000 * sizeof *b), *c = malloc(5000 * sizeof *c);
    assert(a && b && c);
    for (int round = 0; round < 300; round++) {
        size_t n = (size_t)(rand() % 5000);
        for (size_t i = 0; i < n; i++) a[i] = b[i] = c[i] = rand() - RAND_MAX / 2;
        heapsort_int(a, n);
        heapsort_generic(c, n, sizeof *c, cmp_int);
        qsort(b, n, sizeof *b, cmp_int);
        assert(memcmp(a, b, n * sizeof *a) == 0);
        assert(memcmp(c, b, n * sizeof *c) == 0);
    }
    printf("300 random arrays: heapsort == qsort (int and generic versions)\n");

    /* Not stable: equal keys may come out in a different order. */
    Rec r[] = {{"a", 1}, {"b", 1}, {"c", 1}, {"d", 0}};
    heapsort_generic(r, 4, sizeof r[0], cmp_rec_score);
    printf("records sorted by score:");
    for (int i = 0; i < 4; i++) printf(" %s(%.0f)", r[i].name, r[i].score);
    printf("   <- a,b,c order not preserved: heapsort is NOT stable\n");
    for (int i = 1; i < 4; i++) assert(r[i - 1].score <= r[i].score);

    free(a); free(b); free(c);
    printf("all checks passed\n");
    return 0;
}
