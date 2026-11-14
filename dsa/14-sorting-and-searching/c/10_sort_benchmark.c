/*
 * 10_sort_benchmark.c — time every sort in this lesson on several input shapes
 *
 * Concept : Big-O hides constants and cache effects. Measure! Each algorithm
 *           sorts the same inputs (random, sorted, reversed, few-unique);
 *           every result is checked, and times are printed in ms.
 *           Meaningful numbers need an optimised build WITHOUT sanitizers:
 *
 *   rm -f build/dsa/14-sorting-and-searching/c/10_sort_benchmark
 *   make run P=dsa/14-sorting-and-searching/c/10_sort_benchmark.c OPT=1 SAN=0
 *
 *           (the rm is needed because make does not rebuild when only the
 *           flags change). In the default debug+ASan build, the program
 *           detects it and uses small sizes so `make test` stays fast.
 * Build   : make run P=dsa/14-sorting-and-searching/c/10_sort_benchmark.c
 * Read    : ../README.md  (section "3.11 Benchmark")
 */
#define _POSIX_C_SOURCE 199309L   /* clock_gettime under -std=c11 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__SANITIZE_ADDRESS__) || !defined(__OPTIMIZE__)
#  define FAST_BUILD 0
#else
#  define FAST_BUILD 1
#endif

static uint32_t rng_state = 0xC0FFEEu;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }

/* ---- the contestants (compact versions of programs 01-09) ---------------- */
static void insertion_sort(int *a, size_t n)
{
    for (size_t i = 1; i < n; i++) {
        int key = a[i]; size_t j = i;
        while (j > 0 && a[j - 1] > key) { a[j] = a[j - 1]; j--; }
        a[j] = key;
    }
}

static void msort_rec(int *a, int *tmp, size_t lo, size_t hi)
{
    if (hi - lo <= 16) { insertion_sort(a + lo, hi - lo); return; }
    size_t mid = lo + (hi - lo) / 2;
    msort_rec(a, tmp, lo, mid);
    msort_rec(a, tmp, mid, hi);
    if (a[mid - 1] <= a[mid]) return;
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) tmp[k++] = (a[i] <= a[j]) ? a[i++] : a[j++];
    while (i < mid) tmp[k++] = a[i++];
    while (j < hi)  tmp[k++] = a[j++];
    memcpy(a + lo, tmp + lo, (hi - lo) * sizeof *a);
}
static void merge_sort(int *a, size_t n)
{
    int *tmp = malloc(n * sizeof *tmp + 1);
    assert(tmp);
    msort_rec(a, tmp, 0, n);
    free(tmp);
}

/* random-pivot Hoare quicksort, small pieces by insertion sort */
static void quick_rec(int *a, size_t lo, size_t hi)   /* [lo, hi) */
{
    while (hi - lo > 16) {
        int p = a[lo + xorshift32() % (hi - lo)];
        size_t i = lo - 1, j = hi;
        for (;;) {
            do i++; while (a[i] < p);
            do j--; while (a[j] > p);
            if (i >= j) break;
            swap(&a[i], &a[j]);
        }
        if (j + 1 - lo < hi - j - 1) { quick_rec(a, lo, j + 1); lo = j + 1; }
        else                         { quick_rec(a, j + 1, hi); hi = j + 1; }
    }
    insertion_sort(a + lo, hi - lo);
}
static void quick_sort(int *a, size_t n) { if (n > 1) quick_rec(a, 0, n); }

static void sift_down(int *a, size_t i, size_t n)
{
    int item = a[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && a[c + 1] > a[c]) c++;
        if (a[c] <= item) break;
        a[i] = a[c]; i = c;
    }
    a[i] = item;
}
static void heap_sort(int *a, size_t n)
{
    if (n < 2) return;
    for (size_t i = n / 2; i-- > 0; ) sift_down(a, i, n);
    for (size_t e = n - 1; e > 0; e--) { swap(&a[0], &a[e]); sift_down(a, 0, e); }
}

/* LSD radix on signed ints: flip the sign bit so negatives order first. */
static void radix_sort(int *a, size_t n)
{
    uint32_t *src = malloc(n * sizeof *src + 1), *dst = malloc(n * sizeof *dst + 1);
    assert(src && dst);
    for (size_t i = 0; i < n; i++) src[i] = (uint32_t)a[i] ^ 0x80000000u;
    for (unsigned sh = 0; sh < 32; sh += 8) {
        size_t cnt[257] = {0};
        for (size_t i = 0; i < n; i++) cnt[((src[i] >> sh) & 0xFF) + 1]++;
        for (int b = 0; b < 256; b++) cnt[b + 1] += cnt[b];
        for (size_t i = 0; i < n; i++) dst[cnt[(src[i] >> sh) & 0xFF]++] = src[i];
        uint32_t *t = src; src = dst; dst = t;
    }
    for (size_t i = 0; i < n; i++) a[i] = (int)(src[i] ^ 0x80000000u);
    free(src); free(dst);
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}
static void libc_qsort(int *a, size_t n) { qsort(a, n, sizeof *a, cmp_int); }

typedef struct { const char *name; void (*fn)(int *, size_t); int quadratic; } algo_t;

static const char *const kinds[] = {"random", "sorted", "reversed", "few-unique"};

static void fill(int *a, size_t n, int kind)
{
    for (size_t i = 0; i < n; i++) {
        switch (kind) {
        case 0:  a[i] = (int)xorshift32(); break;
        case 1:  a[i] = (int)i; break;
        case 2:  a[i] = (int)(n - i); break;
        default: a[i] = (int)(xorshift32() % 8); break;
        }
    }
}

int main(void)
{
    const algo_t algos[] = {
        {"insertion", insertion_sort, 1},
        {"merge",     merge_sort,     0},
        {"quick",     quick_sort,     0},
        {"heap",      heap_sort,      0},
        {"radix",     radix_sort,     0},
        {"qsort",     libc_qsort,     0},
    };
    const size_t nalg = sizeof algos / sizeof algos[0];
    const size_t n       = FAST_BUILD ? 2000000 : 50000;
    const size_t n_quad  = FAST_BUILD ? 50000   : 5000;    /* insertion sort size */

    printf("=== Sort benchmark: %s ===\n",
           FAST_BUILD ? "optimised build" : "debug+sanitizer build, timings NOT meaningful");
    if (!FAST_BUILD)
        printf("For real numbers:  rm -f build/dsa/14-sorting-and-searching/c/10_sort_benchmark &&\n"
               "                   make run P=dsa/14-sorting-and-searching/c/10_sort_benchmark.c OPT=1 SAN=0\n");
    printf("n = %zu  (insertion sort: n = %zu, it is O(n^2))\n\n", n, n_quad);

    int *orig = malloc(n * sizeof *orig), *work = malloc(n * sizeof *work), *ref = malloc(n * sizeof *ref);
    assert(orig && work && ref);

    printf("%-12s", "ms");
    for (int k = 0; k < 4; k++) printf("%12s", kinds[k]);
    printf("\n");

    for (size_t a = 0; a < nalg; a++) {
        printf("%-12s", algos[a].name);
        for (int k = 0; k < 4; k++) {
            size_t m = algos[a].quadratic ? n_quad : n;
            rng_state = 0xC0FFEEu + (uint32_t)k;    /* same input for every algorithm */
            fill(orig, m, k);
            memcpy(work, orig, m * sizeof *work);
            memcpy(ref, orig, m * sizeof *ref);
            qsort(ref, m, sizeof *ref, cmp_int);
            double t0 = now_ms();
            algos[a].fn(work, m);
            double t1 = now_ms();
            assert(memcmp(work, ref, m * sizeof *work) == 0);   /* correctness first */
            printf("%12.2f", t1 - t0);
        }
        printf("\n");
    }

    printf("\nWhat to look for (optimised build):\n"
           "  - quick < merge < heap on random data; heap suffers from cache misses\n"
           "  - radix is O(n) and often fastest for plain 32-bit ints\n"
           "  - insertion sort is ~instant on sorted input (adaptive) and hopeless on random\n"
           "  - qsort pays an indirect comparator call per comparison\n");
    free(orig); free(work); free(ref);
    return 0;
}
