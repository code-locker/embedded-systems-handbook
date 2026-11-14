/*
 * 02_segment_tree_lazy.c — segment tree with lazy propagation: range add + range sum
 *
 * Concept : Adding v to every element of [l, r] would touch O(n) leaves.
 *           Instead, when a node's slice is fully inside [l, r] we update the
 *           node's sum and leave a sticky note ("lazy[node] += v": my children
 *           still owe v each). The note is pushed down only when someone
 *           later needs to go below that node. Both operations are O(log n).
 * Build   : make run P=dsa/12-advanced-trees/c/02_segment_tree_lazy.c
 * Read    : ../README.md  (section "Lazy propagation")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int n;
    long long *sum;  /* true sum of node's slice (already includes its own lazy) */
    long long *lazy; /* pending add that the CHILDREN have not received yet      */
} LazySeg;

static LazySeg lseg_create(int n)
{
    /* calloc: all zeros = valid tree for an all-zero array, no build needed */
    LazySeg t = { n, calloc((size_t)(4 * n), sizeof(long long)), calloc((size_t)(4 * n), sizeof(long long)) };
    assert(t.sum && t.lazy);
    return t;
}

static void lseg_free(LazySeg *t)
{
    free(t->sum);
    free(t->lazy);
}

/* Apply "+v to every element" to a whole node in O(1): sum grows by v*len. */
static void apply(LazySeg *t, int node, int lo, int hi, long long v)
{
    t->sum[node] += v * (hi - lo + 1);
    t->lazy[node] += v; /* remember that the children still need it */
}

/*
 * push: hand the sticky note to both children, then clear it. O(1).
 *
 *        [0..7] sum=40 lazy=+3          [0..7] sum=40 lazy=0
 *          /        \           push       /        \
 *   [0..3] s=8   [4..7] s=8    ----->  [0..3] s=20  [4..7] s=20
 *                                       lazy=+3      lazy=+3
 */
static void push(LazySeg *t, int node, int lo, int hi)
{
    if (t->lazy[node] != 0) {
        int mid = lo + (hi - lo) / 2;
        apply(t, 2 * node, lo, mid, t->lazy[node]);
        apply(t, 2 * node + 1, mid + 1, hi, t->lazy[node]);
        t->lazy[node] = 0;
    }
}

/* O(log n): same 3-case split as a query; full-cover nodes stop early. */
static void range_add(LazySeg *t, int node, int lo, int hi, int l, int r, long long v)
{
    if (r < lo || hi < l)
        return;
    if (l <= lo && hi <= r) {
        apply(t, node, lo, hi, v); /* stop here: children stay stale, note left */
        return;
    }
    push(t, node, lo, hi);         /* must go deeper -> children must be correct first */
    int mid = lo + (hi - lo) / 2;
    range_add(t, 2 * node, lo, mid, l, r, v);
    range_add(t, 2 * node + 1, mid + 1, hi, l, r, v);
    t->sum[node] = t->sum[2 * node] + t->sum[2 * node + 1];
}

/* O(log n) */
static long long range_sum(LazySeg *t, int node, int lo, int hi, int l, int r)
{
    if (r < lo || hi < l)
        return 0;
    if (l <= lo && hi <= r)
        return t->sum[node];
    push(t, node, lo, hi); /* forgetting this is THE classic lazy bug */
    int mid = lo + (hi - lo) / 2;
    return range_sum(t, 2 * node, lo, mid, l, r) + range_sum(t, 2 * node + 1, mid + 1, hi, l, r);
}

static uint32_t rng_state = 2463534242u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    /* worked example: 8 zeros, add 3 to [0..7], add 2 to [2..4], query */
    LazySeg d = lseg_create(8);
    range_add(&d, 1, 0, 7, 0, 7, 3);
    printf("after +3 on [0..7]: root sum=%lld lazy[1]=%lld (children untouched)\n", d.sum[1], d.lazy[1]);
    assert(d.sum[1] == 24 && d.lazy[1] == 3 && d.sum[2] == 0);
    range_add(&d, 1, 0, 7, 2, 4, 2);
    long long s = range_sum(&d, 1, 0, 7, 3, 5);
    printf("after +2 on [2..4]: sum[3..5] = 5+5+3 = %lld\n", s);
    assert(s == 13);
    lseg_free(&d);

    /* randomised check vs brute force */
    enum { N = 500, OPS = 20000 };
    long long ref[N] = { 0 };
    LazySeg t = lseg_create(N);
    for (int op = 0; op < OPS; op++) {
        int l = (int)(rnd() % N), r = (int)(rnd() % N);
        if (l > r) { int tmp = l; l = r; r = tmp; }
        if (rnd() % 2) {
            long long v = (long long)(rnd() % 201) - 100;
            for (int i = l; i <= r; i++) ref[i] += v; /* O(n) reference */
            range_add(&t, 1, 0, N - 1, l, r, v);
        } else {
            long long want = 0;
            for (int i = l; i <= r; i++) want += ref[i];
            assert(range_sum(&t, 1, 0, N - 1, l, r) == want);
        }
    }
    printf("random test: %d range-add / range-sum ops on n=%d match brute force\n", OPS, N);
    lseg_free(&t);
    printf("OK\n");
    return 0;
}
