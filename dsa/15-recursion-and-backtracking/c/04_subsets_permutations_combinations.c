/*
 * 04_subsets_permutations_combinations.c — the backtracking template applied to
 *                                          the three classic enumeration problems
 *
 * Concept : Backtracking = depth-first walk of a "decision tree":
 *               CHOOSE   an option (push it onto the partial solution)
 *               EXPLORE  recurse on the rest
 *               UN-CHOOSE (pop it) so the next option starts clean
 *           Subsets: 2^n leaves. Permutations: n! leaves. Combinations C(n,k).
 *           Every result is checked against a counting formula and against
 *           an independent method (bitmasks / lexicographic next_permutation).
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/04_subsets_permutations_combinations.c
 * Read    : ../README.md  (sections "Backtracking template", "Subsets",
 *           "Permutations", "Combinations")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 8

/* Every solution is encoded as a small integer "fingerprint" so we can store
   them and compare sets: for subsets a bitmask, for permutations a base-n
   number. Storage: fixed global arrays (no heap). */
static int path[MAXN], plen;               /* the partial solution ("path") */

/* ------------------------------------------------------------------------- */
/* Subsets — "take it or leave it" for each element                          */
/* ------------------------------------------------------------------------- */
/*
 * Decision tree for {a,b,c}: at depth i decide about element i.
 *
 *                         []
 *              take a /        \ skip a
 *                [a]              []
 *            /       \         /      \
 *        [a,b]       [a]     [b]       []
 *        /  \        / \     / \       / \
 *  [a,b,c][a,b] [a,c] [a] [b,c] [b]  [c]  []     <- 2^3 = 8 leaves
 */
static unsigned subset_masks[1 << MAXN];
static int nsub;

static void subsets(const int *a, int n, int i)
{
    if (i == n) {                               /* leaf: one complete subset */
        unsigned m = 0;
        for (int j = 0; j < plen; ++j) m |= 1u << path[j];
        subset_masks[nsub++] = m;
        return;
    }
    path[plen++] = a[i];                        /* CHOOSE: take a[i]          */
    subsets(a, n, i + 1);                       /* EXPLORE                    */
    --plen;                                     /* UN-CHOOSE                  */
    subsets(a, n, i + 1);                       /* the "skip a[i]" branch     */
}

/* ------------------------------------------------------------------------- */
/* Permutations — "which unused element goes in the next slot?"              */
/* ------------------------------------------------------------------------- */
/*
 * Decision tree for {1,2,3}: depth = slot index, branch = which unused value.
 *
 *                       _ _ _
 *             1/          2|          \3
 *          1 _ _        2 _ _        3 _ _
 *          2/ \3        1/ \3        1/ \2
 *       1 2 _ 1 3 _   2 1 _ 2 3 _   3 1 _ 3 2 _
 *         |     |       |     |       |     |
 *       123   132     213   231     312   321      <- 3! = 6 leaves
 *
 * used[] is the state we must restore on the way back up.
 */
static long perm_codes[40320];
static int nperm;
static int used[MAXN];

static long encode(const int *p, int n)
{
    long code = 0;
    for (int j = 0; j < n; ++j) code = code * n + p[j];
    return code;
}

static void permute(const int *a, int n)
{
    if (plen == n) { perm_codes[nperm++] = encode(path, n); return; }
    for (int i = 0; i < n; ++i) {
        if (used[i]) continue;                  /* constraint: each once      */
        used[i] = 1; path[plen++] = a[i];       /* CHOOSE                     */
        permute(a, n);                          /* EXPLORE                    */
        --plen; used[i] = 0;                    /* UN-CHOOSE (both pieces!)   */
    }
}

/* Swap-based permutations: no used[] needed. Slot k picks any element from
   k..n-1 by swapping it into position k, recurses, then swaps it back. */
static long swap_codes[40320];
static int nswap;

static void swap_i(int *x, int *y) { int t = *x; *x = *y; *y = t; }

static void permute_swap(int *a, int n, int k)
{
    if (k == n) { swap_codes[nswap++] = encode(a, n); return; }
    for (int i = k; i < n; ++i) {
        swap_i(&a[k], &a[i]);                   /* CHOOSE a[i] for slot k     */
        permute_swap(a, n, k + 1);              /* EXPLORE                    */
        swap_i(&a[k], &a[i]);                   /* UN-CHOOSE: restore order   */
    }
}

/* ------------------------------------------------------------------------- */
/* Combinations C(n,k) — subsets of a fixed size, in increasing order         */
/* ------------------------------------------------------------------------- */
/*
 * "start" makes each combination appear once ({1,2} but never {2,1}).
 * Pruning: if fewer than (k - plen) numbers remain, no leaf below can reach
 * size k — stop. Loop bound i <= n - (k - plen) + 1 does exactly that.
 */
static unsigned comb_masks[256];
static int ncomb;
static long comb_nodes;                         /* how many calls we made */

static void combine(int n, int k, int start)
{
    ++comb_nodes;
    if (plen == k) {
        unsigned m = 0;
        for (int j = 0; j < plen; ++j) m |= 1u << (path[j] - 1);
        comb_masks[ncomb++] = m;
        return;
    }
    for (int i = start; i <= n - (k - plen) + 1; ++i) {   /* pruned bound */
        path[plen++] = i;
        combine(n, k, i + 1);
        --plen;
    }
}

/* ---- helpers for checking ---------------------------------------------- */
static int cmp_u(const void *x, const void *y)
{
    unsigned a = *(const unsigned *)x, b = *(const unsigned *)y;
    return (a > b) - (a < b);
}
static int cmp_l(const void *x, const void *y)
{
    long a = *(const long *)x, b = *(const long *)y;
    return (a > b) - (a < b);
}
static long factorial(int n) { return n <= 1 ? 1 : n * factorial(n - 1); }
static long binom(int n, int k)
{
    long r = 1;
    for (int i = 1; i <= k; ++i) r = r * (n - k + i) / i;   /* exact at each step */
    return r;
}

/* Reference permutation generator: lexicographic next_permutation. */
static int next_perm(int *p, int n)
{
    int i = n - 2;
    while (i >= 0 && p[i] >= p[i + 1]) --i;
    if (i < 0) return 0;
    int j = n - 1;
    while (p[j] <= p[i]) --j;
    swap_i(&p[i], &p[j]);
    for (int l = i + 1, r = n - 1; l < r; ++l, --r) swap_i(&p[l], &p[r]);
    return 1;
}

int main(void)
{
    int a[MAXN];
    for (int i = 0; i < MAXN; ++i) a[i] = i;

    printf("== subsets of {0,1,2} ==\n");
    nsub = 0; plen = 0;
    subsets(a, 3, 0);
    for (int s = 0; s < nsub; ++s) {
        printf("  {");
        for (int j = 0, first = 1; j < 3; ++j)
            if (subset_masks[s] & (1u << j)) { printf(first ? "%d" : ",%d", j); first = 0; }
        printf("}\n");
    }
    for (int n = 0; n <= MAXN; ++n) {
        nsub = 0; plen = 0;
        subsets(a, n, 0);
        assert(nsub == 1 << n);
        qsort(subset_masks, (size_t)nsub, sizeof subset_masks[0], cmp_u);
        for (int m = 0; m < nsub; ++m) assert(subset_masks[m] == (unsigned)m);
    }
    printf("subsets: count 2^n and exactly the masks 0..2^n-1, n = 0..%d\n", MAXN);

    printf("\n== permutations of {0,1,2} (used[] version) ==\n");
    nperm = 0; plen = 0; memset(used, 0, sizeof used);
    permute(a, 3);
    for (int i = 0; i < nperm; ++i) printf("  %03ld (base 3 code %ld)\n",
        perm_codes[i] / 9 * 100 + perm_codes[i] / 3 % 3 * 10 + perm_codes[i] % 3, perm_codes[i]);
    for (int n = 1; n <= MAXN; ++n) {
        nperm = 0; plen = 0; memset(used, 0, sizeof used);
        permute(a, n);
        int b[MAXN];
        for (int i = 0; i < n; ++i) b[i] = i;
        nswap = 0;
        permute_swap(b, n, 0);
        for (int i = 0; i < n; ++i) assert(b[i] == i);  /* swaps fully undone */
        assert(nperm == factorial(n) && nswap == nperm);
        /* used[] version already emits in lexicographic order: compare
           with next_permutation directly, element by element. */
        int ref[MAXN];
        for (int i = 0; i < n; ++i) ref[i] = i;
        int idx = 0;
        do { assert(perm_codes[idx++] == encode(ref, n)); } while (next_perm(ref, n));
        assert(idx == nperm);
        qsort(swap_codes, (size_t)nswap, sizeof swap_codes[0], cmp_l);
        assert(memcmp(swap_codes, perm_codes, sizeof(long) * (size_t)nperm) == 0);
    }
    printf("permutations: n! results, used[] == swap == next_permutation, n = 1..%d\n", MAXN);

    printf("\n== combinations C(5,3) ==\n");
    ncomb = 0; plen = 0; comb_nodes = 0;
    combine(5, 3, 1);
    for (int i = 0; i < ncomb; ++i) {
        printf("  {");
        for (int j = 0, first = 1; j < 5; ++j)
            if (comb_masks[i] & (1u << j)) { printf(first ? "%d" : ",%d", j + 1); first = 0; }
        printf("}\n");
    }
    for (int n = 0; n <= MAXN; ++n)
        for (int k = 0; k <= n; ++k) {
            ncomb = 0; plen = 0;
            combine(n, k, 1);
            assert(ncomb == binom(n, k));
            /* reference: every mask with popcount k, in some order */
            int cnt = 0;
            for (unsigned m = 0; m < (1u << n); ++m)
                if (__builtin_popcount(m) == k) ++cnt;
            assert(cnt == ncomb);
            for (int i = 0; i < ncomb; ++i) assert(__builtin_popcount(comb_masks[i]) == k);
            qsort(comb_masks, (size_t)ncomb, sizeof comb_masks[0], cmp_u);
            for (int i = 1; i < ncomb; ++i) assert(comb_masks[i] != comb_masks[i - 1]);
        }
    printf("combinations: C(n,k) distinct size-k sets for all n<=%d\n", MAXN);

    printf("\nall subset/permutation/combination checks passed\n");
    return 0;
}
