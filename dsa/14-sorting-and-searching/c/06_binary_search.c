/*
 * 06_binary_search.c — binary search and its variants
 *
 * Concept : Binary search is not "find x in a sorted array". It is:
 *             "find the FIRST index where a monotone predicate turns true"
 *               F F F F T T T          (any yes/no question that flips once)
 *           Write it ONE way — half-open [lo, hi), loop while lo < hi,
 *           mid = lo + (hi-lo)/2, keep the answer inside [lo, hi] — and
 *           every variant (lower_bound, upper_bound, first/last occurrence,
 *           search-on-answer, rotated array) is that same loop with a
 *           different predicate.
 * Build   : make run P=dsa/14-sorting-and-searching/c/06_binary_search.c
 * Read    : ../README.md  (section "3.8 Binary search and its variants")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t rng_state = 4242u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

/* ---------------------------------------------------------------------------
 * Classic closed-interval version [lo, hi]: returns an index of x or -1.
 * Shown because you will meet it everywhere; note the three traps:
 *   1. mid = (lo + hi) / 2 overflows for huge arrays (the 2006 Java bug)
 *   2. hi = mid (not mid-1) here -> infinite loop
 *   3. using size_t for hi = n-1 when n == 0 -> wraps to SIZE_MAX
 * --------------------------------------------------------------------------- */
static long bsearch_classic(const int *a, long n, int x, int trace)
{
    long lo = 0, hi = n - 1;                 /* signed on purpose: hi may be -1 */
    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;       /* overflow-safe midpoint */
        if (trace) printf("    lo=%ld hi=%ld mid=%ld a[mid]=%d\n", lo, hi, mid, a[mid]);
        if (a[mid] == x) return mid;
        if (a[mid] < x) lo = mid + 1;        /* x is right of mid */
        else            hi = mid - 1;        /* x is left of mid  */
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * lower_bound: first index i with a[i] >= x   (n if none)
 * upper_bound: first index i with a[i] >  x   (n if none)
 *
 *   a     = [1 2 4 4 4 7 9]
 *   x = 4:   a[i] >= 4 ?  F F T T T T T  -> lower_bound = 2
 *            a[i] >  4 ?  F F F F F T T  -> upper_bound = 5
 *   count of 4s = upper - lower = 3;  insert position keeping order = lower
 *
 * Half-open template. Invariant: answer is in [lo, hi].
 *   pred(mid) true  -> answer <= mid -> hi = mid
 *   pred(mid) false -> answer >  mid -> lo = mid + 1
 * The range shrinks every step (mid < hi always) -> no infinite loop.
 * --------------------------------------------------------------------------- */
static size_t lower_bound(const int *a, size_t n, int x)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] >= x) hi = mid;
        else             lo = mid + 1;
    }
    return lo;
}

static size_t upper_bound(const int *a, size_t n, int x)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] > x) hi = mid;            /* the ONLY difference: > instead of >= */
        else            lo = mid + 1;
    }
    return lo;
}

/* first / last occurrence built from the bounds (-1 if absent) */
static long first_occurrence(const int *a, size_t n, int x)
{
    size_t i = lower_bound(a, n, x);
    return (i < n && a[i] == x) ? (long)i : -1;
}
static long last_occurrence(const int *a, size_t n, int x)
{
    size_t i = upper_bound(a, n, x);
    return (i > 0 && a[i - 1] == x) ? (long)(i - 1) : -1;
}

/* ---------------------------------------------------------------------------
 * SEARCH ON THE ANSWER: binary search over the space of possible answers.
 *
 * Problem: n packets of sizes w[] must be sent IN ORDER over D time slots;
 * each slot carries consecutive packets up to a capacity C. Find the minimum
 * C. (LeetCode 1011; also: minimum DMA burst size, minimum buffer size.)
 *
 *   feasible(C) is monotone:  C too small -> F ... F T T T <- C big enough
 *   range: [max(w), sum(w)]  (must fit the largest packet; one slot holds all)
 *   find the first C where feasible(C) is true  -> same template!
 * Cost: O(n log(sum)) instead of trying every C.
 * --------------------------------------------------------------------------- */
static int slots_needed(const int *w, size_t n, long cap)
{
    int slots = 1;
    long load = 0;
    for (size_t i = 0; i < n; i++) {
        if (load + w[i] > cap) { slots++; load = 0; }   /* greedy: fill each slot */
        load += w[i];
    }
    return slots;
}

static long min_capacity(const int *w, size_t n, int days)
{
    long lo = 0, hi = 0;
    for (size_t i = 0; i < n; i++) { if (w[i] > lo) lo = w[i]; hi += w[i]; }
    while (lo < hi) {                       /* answer in [lo, hi] */
        long mid = lo + (hi - lo) / 2;
        if (slots_needed(w, n, mid) <= days) hi = mid;   /* feasible: maybe smaller */
        else                                 lo = mid + 1;
    }
    return lo;
}

/* Integer square root: largest r with r*r <= x. "Last true" form: search for
 * the first r with r*r > x, answer is that minus one. */
static uint32_t isqrt(uint32_t x)
{
    uint64_t lo = 0, hi = (uint64_t)x + 1;    /* first r with r*r > x is in [0, x+1] */
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (mid * mid > x) hi = mid;
        else               lo = mid + 1;
    }
    return (uint32_t)(lo - 1);
}

/* ---------------------------------------------------------------------------
 * ROTATED SORTED ARRAY (distinct values): [4 5 6 7 0 1 2]
 *
 * 1. Find the rotation point = index of the minimum. Predicate:
 *      a[i] <= a[n-1] ?   4 5 6 7 | 0 1 2   ->  F F F F T T T   -> index 4
 *    (every element of the right part is <= the last element; the left part
 *     is > it). Same template again.
 * 2. Decide which sorted half x lives in, binary search that half.
 * --------------------------------------------------------------------------- */
static size_t rotation_point(const int *a, size_t n)
{
    size_t lo = 0, hi = n - 1;               /* n >= 1; index n-1 is always 'true' */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (a[mid] <= a[n - 1]) hi = mid;
        else                    lo = mid + 1;
    }
    return lo;
}

static long search_rotated(const int *a, size_t n, int x)
{
    if (n == 0) return -1;
    size_t r = rotation_point(a, n);
    /* sorted halves: a[r..n-1] (all <= last) and a[0..r-1] (all > last) */
    size_t lo = (x <= a[n - 1]) ? r : 0;
    size_t hi = (x <= a[n - 1]) ? n : r;
    size_t i = lo + lower_bound(a + lo, hi - lo, x);
    return (i < hi && a[i] == x) ? (long)i : -1;
}

/* ---- brute-force references ---------------------------------------------- */
static size_t lb_linear(const int *a, size_t n, int x)
{ size_t i = 0; while (i < n && a[i] < x) i++; return i; }
static size_t ub_linear(const int *a, size_t n, int x)
{ size_t i = 0; while (i < n && a[i] <= x) i++; return i; }

static int cmp_int(const void *p, const void *q)
{
    int a = *(const int *)p, b = *(const int *)q;
    return (a > b) - (a < b);
}

int main(void)
{
    printf("=== Binary search ===\n\n");
    const int a[] = {1, 2, 4, 4, 4, 7, 9};
    const size_t n = sizeof a / sizeof a[0];

    printf("Classic search for 7 in [1 2 4 4 4 7 9]:\n");
    long idx = bsearch_classic(a, (long)n, 7, 1);
    printf("  found at %ld\n", idx);
    assert(idx == 5);
    assert(bsearch_classic(a, (long)n, 5, 0) == -1);
    assert(bsearch_classic(a, 0, 5, 0) == -1);   /* empty array: hi = -1, no wrap */

    printf("\nlower/upper bound for x = 4:\n");
    printf("  lower_bound = %zu, upper_bound = %zu, count = %zu\n",
           lower_bound(a, n, 4), upper_bound(a, n, 4), upper_bound(a, n, 4) - lower_bound(a, n, 4));
    assert(lower_bound(a, n, 4) == 2 && upper_bound(a, n, 4) == 5);
    assert(lower_bound(a, n, 0) == 0 && lower_bound(a, n, 10) == n);
    assert(first_occurrence(a, n, 4) == 2 && last_occurrence(a, n, 4) == 4);
    assert(first_occurrence(a, n, 3) == -1 && last_occurrence(a, n, 3) == -1);

    /* random test of the bounds against linear scans */
    enum { MAXN = 300 };
    static int r[MAXN];
    for (int t = 0; t < 2000; t++) {
        size_t m = xorshift32() % MAXN;
        for (size_t i = 0; i < m; i++) r[i] = (int)(xorshift32() % 50);
        qsort(r, m, sizeof *r, cmp_int);
        int x = (int)(xorshift32() % 54) - 2;
        assert(lower_bound(r, m, x) == lb_linear(r, m, x));
        assert(upper_bound(r, m, x) == ub_linear(r, m, x));
        long c = bsearch_classic(r, (long)m, x, 0);
        assert(c == -1 ? lb_linear(r, m, x) == ub_linear(r, m, x) : r[c] == x);
    }
    printf("  lower/upper bound and classic search == linear scan on 2000 random arrays\n");

    printf("\nSearch on the answer — min slot capacity for packets in D slots:\n");
    const int w[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    long cap = min_capacity(w, 10, 5);
    printf("  packets 1..10, 5 slots -> capacity %ld\n", cap);
    assert(cap == 15);   /* [1..5][6 7][8][9][10] */
    for (int t = 0; t < 300; t++) {
        int ww[20];
        size_t m = 1 + xorshift32() % 20;
        for (size_t i = 0; i < m; i++) ww[i] = 1 + (int)(xorshift32() % 30);
        int days = 1 + (int)(xorshift32() % (uint32_t)m);
        long brute = 0;                     /* linear scan from the largest packet up */
        for (size_t i = 0; i < m; i++) if (ww[i] > brute) brute = ww[i];
        while (slots_needed(ww, m, brute) > days) brute++;
        assert(min_capacity(ww, m, days) == brute);
    }
    printf("  matches a linear scan of every capacity on 300 random cases\n");

    printf("\nInteger sqrt by search on answer:\n");
    printf("  isqrt(0)=%u isqrt(15)=%u isqrt(16)=%u isqrt(UINT32_MAX)=%u\n",
           isqrt(0), isqrt(15), isqrt(16), isqrt(UINT32_MAX));
    assert(isqrt(0) == 0 && isqrt(15) == 3 && isqrt(16) == 4 && isqrt(UINT32_MAX) == 65535);
    for (uint32_t x = 0; x < 100000; x++) {
        uint64_t s = isqrt(x);
        assert(s * s <= x && (s + 1) * (s + 1) > x);
    }

    printf("\nRotated sorted array [4 5 6 7 0 1 2]:\n");
    const int rot[] = {4, 5, 6, 7, 0, 1, 2};
    printf("  rotation point (min) at %zu, search(0)=%ld, search(5)=%ld, search(3)=%ld\n",
           rotation_point(rot, 7), search_rotated(rot, 7, 0), search_rotated(rot, 7, 5),
           search_rotated(rot, 7, 3));
    assert(rotation_point(rot, 7) == 4);
    assert(search_rotated(rot, 7, 0) == 4 && search_rotated(rot, 7, 5) == 1);
    assert(search_rotated(rot, 7, 3) == -1);
    for (int t = 0; t < 2000; t++) {
        int s[60], v[60];
        size_t m = 1 + xorshift32() % 60;
        int val = 0;
        for (size_t i = 0; i < m; i++) { val += 1 + (int)(xorshift32() % 5); s[i] = val; } /* distinct, sorted */
        size_t k = xorshift32() % m;
        for (size_t i = 0; i < m; i++) v[i] = s[(i + k) % m];                          /* rotate by k */
        assert(rotation_point(v, m) == (k == 0 ? 0 : m - k));
        int x = (int)(xorshift32() % (uint32_t)(val + 3)) - 1;
        long want = -1;
        for (size_t i = 0; i < m; i++) if (v[i] == x) want = (long)i;
        assert(search_rotated(v, m, x) == want);
    }
    printf("  rotation point and search == linear scan on 2000 random rotations\n");

    printf("\nAll binary search checks passed.\n");
    return 0;
}
