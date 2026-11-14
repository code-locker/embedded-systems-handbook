/*
 * 05_counting_radix_bucket.c — non-comparison sorts: counting, LSD radix, bucket
 *
 * Concept : Any sort that only COMPARES needs >= log2(n!) ~ n log2 n
 *           comparisons (decision-tree bound). These sorts beat it by never
 *           comparing: they use the key's VALUE as an array index.
 *             counting sort: O(n + k)  for keys in [0, k)
 *             radix sort   : O(d * (n + b)) for d digits of base b
 *             bucket sort  : O(n) expected for UNIFORMLY spread keys
 *           The prefix-sum trick makes counting sort STABLE, and stability is
 *           exactly what makes LSD radix sort correct.
 * Build   : make run P=dsa/14-sorting-and-searching/c/05_counting_radix_bucket.c
 * Read    : ../README.md  (section "3.5 Non-comparison sorts")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rng_state = 31337u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

static void print_u(const char *lbl, const unsigned *a, size_t n)
{
    printf("  %-16s [", lbl);
    for (size_t i = 0; i < n; i++) printf("%s%u", i ? " " : "", a[i]);
    printf("]\n");
}

/* ---------------------------------------------------------------------------
 * COUNTING SORT (stable), keys in [0, k).
 *
 *  input  = [2 5 3 0 2 3 0 3]        k = 6
 *  1. count   : value 0 1 2 3 4 5
 *               count 2 0 2 3 0 1
 *  2. prefix  : start 0 2 2 4 7 7    (start[v] = first output slot for v)
 *  3. scatter : walk input LEFT->RIGHT, out[start[v]++] = v
 *               equal keys land in input order -> STABLE
 *  output = [0 0 2 2 3 3 3 5]
 *
 * Caller owns 'out' (n slots). We malloc the k counters and free them.
 * --------------------------------------------------------------------------- */
static int counting_sort(const unsigned *in, unsigned *out, size_t n, unsigned k, int trace)
{
    size_t *start = calloc(k, sizeof *start);
    if (!start) return -1;
    for (size_t i = 0; i < n; i++) start[in[i]]++;          /* 1. histogram */
    if (trace) { printf("  counts          "); for (unsigned v = 0; v < k; v++) printf(" %zu", start[v]); printf("\n"); }
    size_t sum = 0;
    for (unsigned v = 0; v < k; v++) {                      /* 2. exclusive prefix sum */
        size_t c = start[v];
        start[v] = sum;
        sum += c;
    }
    if (trace) { printf("  start positions "); for (unsigned v = 0; v < k; v++) printf(" %zu", start[v]); printf("\n"); }
    for (size_t i = 0; i < n; i++) out[start[in[i]]++] = in[i];  /* 3. stable scatter */
    free(start);
    return 0;
}

/* ---------------------------------------------------------------------------
 * LSD RADIX SORT on 32-bit keys, base 256 (one byte per pass, 4 passes).
 *
 * Sort by the LEAST significant digit first, then the next, each pass a
 * STABLE counting sort. After pass d the array is sorted by the low d digits.
 * Decimal example (base 10, 3 digits):
 *     [170 45 75 90 802 24 2 66]
 *  by 1s  : [170 90 802 2 24 45 75 66]
 *  by 10s : [802 2 24 45 66 170 75 90]     (802 before 2: kept from last pass)
 *  by 100s: [2 24 45 66 75 90 170 802]
 * If a pass were unstable, the order from the previous digits would be lost.
 *
 * Used for: GPU sorting, database integer columns, suffix arrays, sorting
 * 8/16-bit pixel or sample values. O(4 * (n + 256)) = O(n) for 32-bit keys.
 * --------------------------------------------------------------------------- */
static int radix_sort_u32(uint32_t *a, size_t n)
{
    uint32_t *buf = malloc(n * sizeof *buf + 1);   /* +1: malloc(0) may be NULL */
    if (!buf) return -1;
    uint32_t *src = a, *dst = buf;
    for (unsigned shift = 0; shift < 32; shift += 8) {
        size_t count[257] = {0};
        for (size_t i = 0; i < n; i++) count[((src[i] >> shift) & 0xFFu) + 1]++;
        for (int b = 0; b < 256; b++) count[b + 1] += count[b];   /* prefix sums */
        for (size_t i = 0; i < n; i++) dst[count[(src[i] >> shift) & 0xFFu]++] = src[i];
        uint32_t *t = src; src = dst; dst = t;                    /* ping-pong buffers */
    }
    /* 4 passes = even number of swaps -> result is back in 'a'. */
    assert(src == a);
    free(buf);
    return 0;
}

/* Decimal LSD radix sort, only for the printed trace above. */
static void radix_decimal_trace(unsigned *a, size_t n)
{
    unsigned tmp[16];
    assert(n <= 16);
    print_u("start", a, n);
    for (unsigned exp = 1; exp <= 100; exp *= 10) {
        size_t count[11] = {0};
        for (size_t i = 0; i < n; i++) count[(a[i] / exp) % 10 + 1]++;
        for (int d = 0; d < 10; d++) count[d + 1] += count[d];
        for (size_t i = 0; i < n; i++) tmp[count[(a[i] / exp) % 10]++] = a[i];
        memcpy(a, tmp, n * sizeof *a);
        char lbl[24];
        snprintf(lbl, sizeof lbl, "by %us digit", exp);
        print_u(lbl, a, n);
    }
}

/* ---------------------------------------------------------------------------
 * BUCKET SORT for doubles in [0, 1).
 *
 *  n buckets; value x goes to bucket floor(x * n). With uniform input each
 *  bucket gets ~1 item, so sorting each bucket (insertion sort) is O(1)
 *  expected, total O(n). Skewed input -> everything in one bucket -> O(n^2).
 *
 *  [.78 .17 .39 .26 .72 .94 .21 .12 .23 .68]   n = 10 buckets
 *   b1: .17 .12    b2: .26 .21 .23   b3: .39   b6: .68   b7: .78 .72   b9: .94
 *  sort each, concatenate.
 *
 *  Implementation: counting-sort style (count per bucket, prefix sum,
 *  scatter) so no per-bucket malloc / linked list is needed.
 * --------------------------------------------------------------------------- */
static int bucket_sort(double *a, size_t n)
{
    if (n < 2) return 0;
    size_t *start = calloc(n + 1, sizeof *start);
    double *out = malloc(n * sizeof *out);
    if (!start || !out) { free(start); free(out); return -1; }
    for (size_t i = 0; i < n; i++) start[(size_t)(a[i] * (double)n) + 1]++;
    for (size_t b = 0; b < n; b++) start[b + 1] += start[b];
    size_t *pos = malloc(n * sizeof *pos);
    if (!pos) { free(start); free(out); return -1; }
    memcpy(pos, start, n * sizeof *pos);
    for (size_t i = 0; i < n; i++) out[pos[(size_t)(a[i] * (double)n)]++] = a[i];
    /* insertion sort inside each bucket [start[b], start[b+1]) */
    for (size_t b = 0; b < n; b++) {
        for (size_t i = start[b] + 1; i < start[b + 1]; i++) {
            double key = out[i];
            size_t j = i;
            while (j > start[b] && out[j - 1] > key) { out[j] = out[j - 1]; j--; }
            out[j] = key;
        }
    }
    memcpy(a, out, n * sizeof *a);
    free(pos); free(start); free(out);
    return 0;
}

/* ---- references ----------------------------------------------------------- */
static int cmp_u32(const void *x, const void *y)
{
    uint32_t a = *(const uint32_t *)x, b = *(const uint32_t *)y;
    return (a > b) - (a < b);
}
static int cmp_unsigned(const void *x, const void *y)
{
    unsigned a = *(const unsigned *)x, b = *(const unsigned *)y;
    return (a > b) - (a < b);
}
static int cmp_double(const void *x, const void *y)
{
    double a = *(const double *)x, b = *(const double *)y;
    return (a > b) - (a < b);
}

/* Stability demo for counting sort: sort records by an 'age' key. */
typedef struct { unsigned age; char name; } person_t;

static void counting_sort_people(const person_t *in, person_t *out, size_t n, unsigned k)
{
    size_t start[16] = {0};
    assert(k <= 16);
    for (size_t i = 0; i < n; i++) start[in[i].age]++;
    size_t sum = 0;
    for (unsigned v = 0; v < k; v++) { size_t c = start[v]; start[v] = sum; sum += c; }
    for (size_t i = 0; i < n; i++) out[start[in[i].age]++] = in[i];
}

int main(void)
{
    printf("=== Non-comparison sorts ===\n\n");

    printf("Counting sort, keys in [0,6):\n");
    unsigned in[] = {2, 5, 3, 0, 2, 3, 0, 3}, out[8];
    print_u("input", in, 8);
    assert(counting_sort(in, out, 8, 6, 1) == 0);
    print_u("output", out, 8);
    const unsigned want[] = {0, 0, 2, 2, 3, 3, 3, 5};
    assert(memcmp(out, want, sizeof want) == 0);

    printf("\nCounting sort is STABLE (records sorted by age, names show order):\n");
    const person_t ppl[] = {{3, 'A'}, {1, 'B'}, {3, 'C'}, {2, 'D'}, {1, 'E'}};
    person_t sorted[5];
    counting_sort_people(ppl, sorted, 5, 4);
    printf("  ");
    for (int i = 0; i < 5; i++) printf("(%u,%c) ", sorted[i].age, sorted[i].name);
    printf("\n");
    assert(sorted[0].name == 'B' && sorted[1].name == 'E');   /* B before E */
    assert(sorted[3].name == 'A' && sorted[4].name == 'C');   /* A before C */

    printf("\nLSD radix sort, base 10 (trace):\n");
    unsigned r[] = {170, 45, 75, 90, 802, 24, 2, 66};
    radix_decimal_trace(r, 8);
    const unsigned rwant[] = {2, 24, 45, 66, 75, 90, 170, 802};
    assert(memcmp(r, rwant, sizeof rwant) == 0);

    /* ---- random tests ----------------------------------------------------- */
    enum { MAXN = 5000 };
    static unsigned cu[MAXN], co[MAXN], cref[MAXN];
    static uint32_t ra[MAXN], rref[MAXN];
    static double da[MAXN], dref[MAXN];
    for (int trial = 0; trial < 200; trial++) {
        size_t n = xorshift32() % MAXN;
        unsigned k = 1 + xorshift32() % 300;
        for (size_t i = 0; i < n; i++) cu[i] = cref[i] = xorshift32() % k;
        assert(counting_sort(cu, co, n, k, 0) == 0);
        qsort(cref, n, sizeof *cref, cmp_unsigned);
        assert(memcmp(co, cref, n * sizeof *co) == 0);

        for (size_t i = 0; i < n; i++) ra[i] = rref[i] = xorshift32();   /* full 32-bit */
        assert(radix_sort_u32(ra, n) == 0);
        qsort(rref, n, sizeof *rref, cmp_u32);
        assert(memcmp(ra, rref, n * sizeof *ra) == 0);

        for (size_t i = 0; i < n; i++) da[i] = dref[i] = (double)(xorshift32() >> 8) / 16777216.0;
        assert(bucket_sort(da, n) == 0);
        qsort(dref, n, sizeof *dref, cmp_double);
        assert(memcmp(da, dref, n * sizeof *da) == 0);
    }
    printf("\nCounting, radix (full 32-bit keys) and bucket sort == qsort on 200 arrays each.\n");

    printf("\nAll non-comparison sort checks passed.\n");
    return 0;
}
