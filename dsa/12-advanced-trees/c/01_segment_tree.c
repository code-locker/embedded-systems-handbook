/*
 * 01_segment_tree.c — segment tree: range sum + range min with point update
 *
 * Concept : A segment tree stores, for every node, the answer (sum, min...)
 *           of one contiguous slice of the array. Any query range [l, r] is
 *           covered by O(log n) such slices, and changing one element only
 *           touches the O(log n) nodes on the path from its leaf to the root.
 * Build   : make run P=dsa/12-advanced-trees/c/01_segment_tree.c
 * Read    : ../README.md  (section "Segment tree")
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Layout: implicit binary tree in an array, exactly like a heap.
 *   node 1 is the root and covers [0, n-1]
 *   node i covers [lo, hi]; its children are 2i  -> [lo, mid]
 *                                            2i+1 -> [mid+1, hi]
 * 4n slots are always enough (2 * next_pow2(n) < 4n).
 *
 * Example a = [5, 2, 7, 1]   (sum tree)
 *
 *                 [0..3] 15
 *                /         \
 *          [0..1] 7      [2..3] 8
 *          /    \         /    \
 *       [0]5  [1]2     [2]7   [3]1
 */
typedef struct {
    int n;
    long long *sum; /* sum[i] = sum of the slice node i covers   */
    int *mn;        /* mn[i]  = min of the slice node i covers   */
} SegTree;

/* O(n): every node is visited exactly once (2n-1 nodes). */
static void build(SegTree *t, const int *a, int node, int lo, int hi)
{
    if (lo == hi) {                 /* leaf: the slice is one element */
        t->sum[node] = a[lo];
        t->mn[node] = a[lo];
        return;
    }
    int mid = lo + (hi - lo) / 2;
    build(t, a, 2 * node, lo, mid);
    build(t, a, 2 * node + 1, mid + 1, hi);
    /* "pull": a parent is just the combine of its two children */
    t->sum[node] = t->sum[2 * node] + t->sum[2 * node + 1];
    t->mn[node] = t->mn[2 * node] < t->mn[2 * node + 1] ? t->mn[2 * node] : t->mn[2 * node + 1];
}

/* Allocates the arrays; caller must call seg_free(). O(n). */
static SegTree seg_create(const int *a, int n)
{
    SegTree t = { n, calloc((size_t)(4 * n), sizeof(long long)), calloc((size_t)(4 * n), sizeof(int)) };
    assert(t.sum && t.mn);
    build(&t, a, 1, 0, n - 1);
    return t;
}

static void seg_free(SegTree *t)
{
    free(t->sum);
    free(t->mn);
    t->sum = NULL;
    t->mn = NULL;
}

/* O(log n): walk root -> leaf, then re-pull every node on the way back up. */
static void update(SegTree *t, int node, int lo, int hi, int pos, int val)
{
    if (lo == hi) {
        t->sum[node] = val;
        t->mn[node] = val;
        return;
    }
    int mid = lo + (hi - lo) / 2;
    if (pos <= mid)
        update(t, 2 * node, lo, mid, pos, val);
    else
        update(t, 2 * node + 1, mid + 1, hi, pos, val);
    t->sum[node] = t->sum[2 * node] + t->sum[2 * node + 1];
    t->mn[node] = t->mn[2 * node] < t->mn[2 * node + 1] ? t->mn[2 * node] : t->mn[2 * node + 1];
}

/*
 * O(log n) query. Three cases per node:
 *   1. node slice is completely OUTSIDE [l, r]  -> return identity (0 / +inf)
 *   2. node slice is completely INSIDE  [l, r]  -> return stored value, stop
 *   3. partial overlap                          -> ask both children
 * At most 2 nodes per level are "partial", so ~4 log n nodes are visited.
 */
static long long query_sum(const SegTree *t, int node, int lo, int hi, int l, int r)
{
    if (r < lo || hi < l)
        return 0;                       /* identity for + */
    if (l <= lo && hi <= r)
        return t->sum[node];
    int mid = lo + (hi - lo) / 2;
    return query_sum(t, 2 * node, lo, mid, l, r) + query_sum(t, 2 * node + 1, mid + 1, hi, l, r);
}

static int query_min(const SegTree *t, int node, int lo, int hi, int l, int r)
{
    if (r < lo || hi < l)
        return INT_MAX;                 /* identity for min */
    if (l <= lo && hi <= r)
        return t->mn[node];
    int mid = lo + (hi - lo) / 2;
    int a = query_min(t, 2 * node, lo, mid, l, r);
    int b = query_min(t, 2 * node + 1, mid + 1, hi, l, r);
    return a < b ? a : b;
}

/* Public wrappers so callers never see node/lo/hi. */
static long long seg_sum(const SegTree *t, int l, int r) { return query_sum(t, 1, 0, t->n - 1, l, r); }
static int seg_min(const SegTree *t, int l, int r) { return query_min(t, 1, 0, t->n - 1, l, r); }
static void seg_set(SegTree *t, int pos, int val) { update(t, 1, 0, t->n - 1, pos, val); }

/* tiny deterministic PRNG so runs are reproducible */
static uint32_t rng_state = 12345u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    /* --- the worked example from the README --- */
    int demo[] = { 5, 2, 7, 1 };
    SegTree d = seg_create(demo, 4);
    printf("a = [5 2 7 1]\n");
    printf("root sum (node 1)            = %lld\n", d.sum[1]);
    printf("sum[1..3] = 2+7+1            = %lld\n", seg_sum(&d, 1, 3));
    printf("min[0..2]                    = %d\n", seg_min(&d, 0, 2));
    assert(d.sum[1] == 15 && seg_sum(&d, 1, 3) == 10 && seg_min(&d, 0, 2) == 2);
    seg_set(&d, 1, 9); /* a = [5 9 7 1] */
    printf("after a[1]=9: sum[0..1] = %lld, min[0..2] = %d\n", seg_sum(&d, 0, 1), seg_min(&d, 0, 2));
    assert(seg_sum(&d, 0, 1) == 14 && seg_min(&d, 0, 2) == 5);
    seg_free(&d);

    /* --- randomised check against a brute-force array --- */
    enum { N = 1000, OPS = 20000 };
    int *ref = malloc(N * sizeof *ref);
    assert(ref);
    for (int i = 0; i < N; i++)
        ref[i] = (int)(rnd() % 2001) - 1000;
    SegTree t = seg_create(ref, N);
    for (int op = 0; op < OPS; op++) {
        if (rnd() % 2) {
            int pos = (int)(rnd() % N), val = (int)(rnd() % 2001) - 1000;
            ref[pos] = val;
            seg_set(&t, pos, val);
        } else {
            int l = (int)(rnd() % N), r = (int)(rnd() % N);
            if (l > r) { int tmp = l; l = r; r = tmp; }
            long long s = 0;
            int m = INT_MAX;
            for (int i = l; i <= r; i++) {  /* O(n) reference */
                s += ref[i];
                if (ref[i] < m) m = ref[i];
            }
            assert(seg_sum(&t, l, r) == s);
            assert(seg_min(&t, l, r) == m);
        }
    }
    printf("random test: %d mixed updates/queries on n=%d match brute force\n", OPS, N);
    seg_free(&t);
    free(ref);
    printf("OK\n");
    return 0;
}
