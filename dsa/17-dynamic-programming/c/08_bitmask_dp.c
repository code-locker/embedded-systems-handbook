/*
 * 08_bitmask_dp.c — bitmask DP: travelling salesman (Held-Karp) and the
 *                   assignment problem
 *
 * Concept : When the state is "which items have been used so far", store
 *           that set as the bits of an integer. 2^n subsets instead of n!
 *           orderings: TSP drops from O(n!) to O(2^n * n^2), which turns
 *           n = 15 from "never" into "a few milliseconds".
 * Build   : make run P=dsa/17-dynamic-programming/c/08_bitmask_dp.c
 * Read    : ../README.md  (section "Bitmask DP")
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define MAXN 15
#define INF (1 << 29)

static unsigned rng_state = 8086u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------------- Held-Karp TSP ---
 * Visit every city exactly once starting and ending at city 0, min length.
 *
 * State : dp[mask][v] = shortest path that starts at 0, visits exactly the
 *         cities in `mask` (bit i set = city i visited), and ends at v.
 * Base  : dp[1][0] = 0            (only city 0 visited, standing on it)
 * Move  : from (mask, v) go to u not in mask:
 *         dp[mask | 1<<u][u] = min(..., dp[mask][v] + d[v][u])
 * Answer: min over v of dp[full][v] + d[v][0]
 *
 *   n = 4, mask 0b1011 = {0,1,3} visited, standing at 3:
 *     the ORDER of 0,1 before 3 no longer matters — only the set and the
 *     endpoint. That is the whole trick: many orderings collapse into one
 *     state.
 *
 * Masks only grow, so iterating mask in increasing numeric order processes
 * every subset before its supersets.
 * Time O(2^n * n^2), space O(2^n * n).
 */
static int dp[1 << MAXN][MAXN];

static int tsp(int d[][MAXN], int n)
{
    int full = (1 << n) - 1;
    for (int m = 0; m <= full; m++)
        for (int v = 0; v < n; v++) dp[m][v] = INF;
    dp[1][0] = 0;
    for (int mask = 1; mask <= full; mask++) {
        if (!(mask & 1)) continue;                 /* every tour includes 0 */
        for (int v = 0; v < n; v++) {
            if (dp[mask][v] >= INF || !(mask & (1 << v))) continue;
            for (int u = 0; u < n; u++) {
                if (mask & (1 << u)) continue;      /* already visited */
                int nm = mask | (1 << u), c = dp[mask][v] + d[v][u];
                if (c < dp[nm][u]) dp[nm][u] = c;
            }
        }
    }
    int best = INF;
    for (int v = 0; v < n; v++)
        if (dp[full][v] + d[v][0] < best) best = dp[full][v] + d[v][0];
    return n == 1 ? 0 : best;
}

/* Brute force: every permutation of cities 1..n-1 (Heap-free recursion). */
static int tsp_brute_rec(int d[][MAXN], int n, int *perm, int k, int used)
{
    if (k == n) {
        int len = 0;
        for (int i = 0; i + 1 < n; i++) len += d[perm[i]][perm[i + 1]];
        return len + d[perm[n - 1]][perm[0]];
    }
    int best = INF;
    for (int c = 1; c < n; c++) {
        if (used & (1 << c)) continue;
        perm[k] = c;
        int v = tsp_brute_rec(d, n, perm, k + 1, used | (1 << c));
        if (v < best) best = v;
    }
    return best;
}

static int tsp_brute(int d[][MAXN], int n)
{
    int perm[MAXN] = {0};
    if (n == 1) return 0;
    return tsp_brute_rec(d, n, perm, 1, 1);
}

/* --------------------------------------------------- assignment problem --
 * n workers, n jobs, cost[w][j]. Give each worker a distinct job, min total.
 * Worker index = popcount(mask): the workers are assigned in order 0,1,2..,
 * so "how many jobs are taken" tells you whose turn it is. The state is just
 * the set of taken jobs.
 *   best[mask] = min cost when workers 0..popcount(mask)-1 got the jobs in mask
 * Time O(2^n * n), space O(2^n).
 */
static int assignment(int cost[][MAXN], int n)
{
    static int best[1 << MAXN];
    int full = (1 << n) - 1;
    for (int m = 0; m <= full; m++) best[m] = INF;
    best[0] = 0;
    for (int mask = 0; mask < full; mask++) {
        if (best[mask] >= INF) continue;
        int w = __builtin_popcount((unsigned)mask);   /* next worker */
        for (int j = 0; j < n; j++)
            if (!(mask & (1 << j)) && best[mask] + cost[w][j] < best[mask | (1 << j)])
                best[mask | (1 << j)] = best[mask] + cost[w][j];
    }
    return best[full];
}

static int assign_brute(int cost[][MAXN], int n, int w, int used)
{
    if (w == n) return 0;
    int best = INF;
    for (int j = 0; j < n; j++)
        if (!(used & (1 << j))) {
            int v = cost[w][j] + assign_brute(cost, n, w + 1, used | (1 << j));
            if (v < best) best = v;
        }
    return best;
}

int main(void)
{
    printf("== TSP on 4 cities ==\n");
    int d[MAXN][MAXN] = {
        {0, 10, 15, 20},
        {10, 0, 35, 25},
        {15, 35, 0, 30},
        {20, 25, 30, 0},
    };
    int t = tsp(d, 4);
    printf("shortest tour = %d  (0->1->3->2->0 = 10+25+30+15)\n", t);
    CHECK(t == 80 && tsp_brute(d, 4) == 80);
    printf("some dp[mask][v] entries (mask in binary, city 0 = lowest bit):\n");
    for (int mask = 1; mask < 16; mask += 2)
        for (int v = 1; v < 4; v++)
            if (dp[mask][v] < INF && __builtin_popcount((unsigned)mask) == 3)
                printf("  mask=%d%d%d%d end=%d  dp=%d\n", (mask >> 3) & 1, (mask >> 2) & 1,
                       (mask >> 1) & 1, mask & 1, v, dp[mask][v]);

    for (int it = 0; it < 60; it++) {
        int n = 1 + (int)(rng() % 8);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) d[i][j] = i == j ? 0 : 1 + (int)(rng() % 50);
        CHECK(tsp(d, n) == tsp_brute(d, n));
    }
    printf("60 random (asymmetric) instances, n <= 8: Held-Karp == all permutations\n");

    /* n = 15 is hopeless by brute force (14! = 87 billion tours) but quick here. */
    for (int i = 0; i < 15; i++)
        for (int j = 0; j < 15; j++) d[i][j] = i == j ? 0 : 1 + (int)(rng() % 100);
    printf("n = 15 tour length = %d  (2^15 * 15 states; brute force would need 14! tours)\n",
           tsp(d, 15));

    printf("\n== Assignment problem ==\n");
    int cost[MAXN][MAXN] = {
        {9, 2, 7, 8},
        {6, 4, 3, 7},
        {5, 8, 1, 8},
        {7, 6, 9, 4},
    };
    int a = assignment(cost, 4);
    printf("min cost = %d  (w0->j1 2, w1->j0 6, w2->j2 1, w3->j3 4)\n", a);
    CHECK(a == 13 && assign_brute(cost, 4, 0, 0) == 13);
    for (int it = 0; it < 100; it++) {
        int n = 1 + (int)(rng() % 8);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) cost[i][j] = (int)(rng() % 30);
        CHECK(assignment(cost, n) == assign_brute(cost, n, 0, 0));
    }
    printf("100 random instances: bitmask DP == brute force over all assignments\n");

    printf("\nall checks passed\n");
    return 0;
}
