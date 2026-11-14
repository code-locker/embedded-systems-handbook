/*
 * 03_knapsack.c — 0/1 knapsack (2-D table, reconstruction, 1-D) and
 *                 unbounded knapsack
 *
 * Concept : dp[i][w] = best value using the first i items with capacity w.
 *           Each item is either skipped or taken once. The 1-D version must
 *           loop w DOWNWARD for 0/1 (so an item is not reused) and UPWARD for
 *           unbounded (so it can be). That one loop direction is the whole
 *           difference between the two problems.
 * Build   : make run P=dsa/17-dynamic-programming/c/03_knapsack.c
 * Read    : ../README.md  (section "0/1 and unbounded knapsack")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define MAXN 16
#define MAXW 64

static unsigned rng_state = 2024u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int max_i(int a, int b) { return a > b ? a : b; }

/* ------------------------------------------------------------ 0/1, 2-D ---
 * State  : dp[i][w] = max value from items 0..i-1 with capacity w
 * Base   : dp[0][w] = 0 (no items), dp[i][0] = 0 (no room)
 * Choice : item i-1 is skipped  -> dp[i-1][w]
 *          item i-1 is taken    -> dp[i-1][w - wt] + val   (if wt <= w)
 *
 *                 w-wt         w
 *   row i-1   ... [take] ...  [skip]
 *                     \         |
 *   row i     .............  [ max ]
 *
 * Every cell reads only the row above, so rows are filled top to bottom.
 * Time O(n*W), space O(n*W). `taken` gets which items form the optimum.
 */
static int knap01_table(const int *wt, const int *val, int n, int W,
                        int *taken, int trace)
{
    static int dp[MAXN + 1][MAXW + 1];
    for (int w = 0; w <= W; w++) dp[0][w] = 0;
    for (int i = 1; i <= n; i++) {
        for (int w = 0; w <= W; w++) {
            dp[i][w] = dp[i - 1][w];                         /* skip */
            if (wt[i - 1] <= w)
                dp[i][w] = max_i(dp[i][w], dp[i - 1][w - wt[i - 1]] + val[i - 1]);
        }
        if (trace) {
            printf("  row i=%d (item wt=%d val=%2d):", i, wt[i - 1], val[i - 1]);
            for (int w = 0; w <= W; w++) printf(" %2d", dp[i][w]);
            printf("\n");
        }
    }
    /* Reconstruction: walk back up. If the value changed between row i-1
     * and row i at column w, item i-1 must have been taken. */
    if (taken) {
        int w = W;
        for (int i = n; i >= 1; i--) {
            taken[i - 1] = dp[i][w] != dp[i - 1][w];
            if (taken[i - 1]) w -= wt[i - 1];
        }
    }
    return dp[n][W];
}

/* ------------------------------------------------------------ 0/1, 1-D ---
 * Keep one row. Iterate w from W DOWN to wt: dp[w - wt] on the right-hand
 * side is still the OLD row (item not yet used). If we went upward, dp[w-wt]
 * would already include this item and we would take it twice.
 *
 *   wrong (upward), item wt=2 val=3, W=4:
 *     dp[2] = dp[0]+3 = 3,   dp[4] = dp[2]+3 = 6   <- item used twice!
 * Time O(n*W), space O(W).
 */
static int knap01_1d(const int *wt, const int *val, int n, int W)
{
    int dp[MAXW + 1] = {0};
    for (int i = 0; i < n; i++)
        for (int w = W; w >= wt[i]; w--)               /* DOWNWARD */
            dp[w] = max_i(dp[w], dp[w - wt[i]] + val[i]);
    return dp[W];
}

/* ----------------------------------------------------------- unbounded ---
 * Unlimited copies of each item. Same 1-D code with w going UPWARD, which is
 * exactly the "reuse" that was a bug above. (Coin change = unbounded knapsack
 * where every item has value -1 and you minimise.)
 */
static int knap_unbounded(const int *wt, const int *val, int n, int W)
{
    int dp[MAXW + 1] = {0};
    for (int i = 0; i < n; i++)
        for (int w = wt[i]; w <= W; w++)               /* UPWARD */
            dp[w] = max_i(dp[w], dp[w - wt[i]] + val[i]);
    return dp[W];
}

/* --------------------------------------------------------- brute force ---*/
static int knap01_brute(const int *wt, const int *val, int n, int W)
{
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {  /* every subset */
        int sw = 0, sv = 0;
        for (int i = 0; i < n; i++)
            if (mask & (1u << i)) { sw += wt[i]; sv += val[i]; }
        if (sw <= W && sv > best) best = sv;
    }
    return best;
}

static int unbounded_brute(const int *wt, const int *val, int n, int i, int W)
{
    if (i == n) return 0;
    int best = 0;
    for (int copies = 0; copies * wt[i] <= W; copies++)
        best = max_i(best, copies * val[i] +
                               unbounded_brute(wt, val, n, i + 1, W - copies * wt[i]));
    return best;
}

int main(void)
{
    printf("== 0/1 knapsack, W = 7, table filled row by row ==\n");
    const int wt[] = {1, 3, 4, 5}, val[] = {1, 4, 5, 7};
    int taken[4];
    printf("%-29s", "  capacity w:");
    for (int w = 0; w <= 7; w++) printf(" %2d", w);
    printf("\n");
    int best = knap01_table(wt, val, 4, 7, taken, 1);
    printf("best value = %d using items:", best);
    int sw = 0, sv = 0;
    for (int i = 0; i < 4; i++)
        if (taken[i]) { printf(" #%d(wt %d,val %d)", i, wt[i], val[i]); sw += wt[i]; sv += val[i]; }
    printf("\n");
    CHECK(best == 9);                  /* 3+4 -> 4+5 = 9 */
    CHECK(sv == best && sw <= 7);      /* reconstruction is consistent */
    CHECK(knap01_1d(wt, val, 4, 7) == 9);

    printf("\n== Unbounded knapsack, same items, W = 7 ==\n");
    int ub = knap_unbounded(wt, val, 4, 7);
    printf("best value = %d  (e.g. 3+4 -> 9, or 1+1+5 -> 9)\n", ub);
    CHECK(ub == 9);
    const int wt2[] = {2, 3}, val2[] = {3, 5};
    CHECK(knap_unbounded(wt2, val2, 2, 7) == 11);       /* 2+2+3 */
    CHECK(knap01_1d(wt2, val2, 2, 7) == 8);             /* 2+3 only once each */

    /* Randomised check against brute force. */
    for (int t = 0; t < 400; t++) {
        int n = 1 + (int)(rng() % 12), W = (int)(rng() % 50);
        int w8[MAXN], v8[MAXN], tk[MAXN];
        for (int i = 0; i < n; i++) {
            w8[i] = 1 + (int)(rng() % 15);
            v8[i] = (int)(rng() % 30);
        }
        int bf = knap01_brute(w8, v8, n, W);
        int tb = knap01_table(w8, v8, n, W, tk, 0);
        CHECK(tb == bf);
        CHECK(knap01_1d(w8, v8, n, W) == bf);
        int rw = 0, rv = 0;
        for (int i = 0; i < n; i++)
            if (tk[i]) { rw += w8[i]; rv += v8[i]; }
        CHECK(rw <= W && rv == bf);
        if (n <= 5) CHECK(knap_unbounded(w8, v8, n, W) == unbounded_brute(w8, v8, n, 0, W));
    }
    printf("400 random instances: table == 1-D == brute force; reconstruction valid\n");

    printf("\nall checks passed\n");
    return 0;
}
