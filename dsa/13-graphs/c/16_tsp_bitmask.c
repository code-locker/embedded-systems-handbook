/*
 * 16_tsp_bitmask.c — travelling salesman: exact Held-Karp bitmask DP for
 *                    small n, vs brute force, vs a greedy heuristic
 *
 * Concept : TSP (shortest tour visiting every vertex once and returning) is
 *           NP-hard: no known algorithm is polynomial. Brute force tries all
 *           (n-1)! tours. Held-Karp notices that the future of a partial tour
 *           depends only on (WHICH cities are done, WHERE we are now), not on
 *           the order we visited them in:
 *             dp[mask][v] = cheapest path that starts at 0, visits exactly the
 *                           cities in `mask`, and ends at v.
 *           2^n * n states, n transitions each: O(2^n * n^2). n = 20 is about
 *           4e8 steps (fine); brute force 19! is 1.2e17 (never).
 *           Greedy "go to the nearest unvisited city" is fast but can be bad.
 * Build   : make run P=dsa/13-graphs/c/16_tsp_bitmask.c
 * Read    : ../README.md and ../notes/08-advanced.md  (section "TSP")
 *
 *   4 cities, complete graph:            optimal tour 0-2-3-1-0 = 80 (or reversed)
 *
 *          0 ---10---- 1
 *          | \       / |
 *         15  20   25  35
 *          |     X     |
 *          2 ---30---- 3      (0-3: 20, 1-2: 35, 1-3: 25, 2-3: 30)
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 12 };
#define INF (INT_MAX / 4)

/* Returns optimal tour cost; tour[] gets the n+1 vertices (0 ... 0).
 * dp and parent are heap-allocated: 2^n * n ints each. Owner: this function. */
static int held_karp(int n, int d[MAXN][MAXN], int *tour, int trace)
{
    size_t states = (size_t)1 << n;
    int *dp  = malloc(sizeof(int) * states * (size_t)n);
    int *par = malloc(sizeof(int) * states * (size_t)n);
    CHECK(dp && par);
#define DP(m, v)  dp[(size_t)(m) * (size_t)n + (size_t)(v)]
#define PAR(m, v) par[(size_t)(m) * (size_t)n + (size_t)(v)]
    for (size_t i = 0; i < states * (size_t)n; i++) { dp[i] = INF; par[i] = -1; }
    DP(1, 0) = 0;                                   /* at city 0, only 0 visited */

    for (unsigned mask = 1; mask < states; mask += 2)        /* masks containing city 0 */
        for (int v = 0; v < n; v++) {
            if (!(mask >> v & 1) || DP(mask, v) >= INF) continue;
            for (int w = 0; w < n; w++) {                    /* extend to unvisited w */
                if (mask >> w & 1) continue;
                unsigned nm = mask | 1u << w;
                int c = DP(mask, v) + d[v][w];
                if (c < DP(nm, w)) { DP(nm, w) = c; PAR(nm, w) = v; }
            }
        }

    unsigned full = (unsigned)states - 1;
    int best = INF, last = -1;
    for (int v = 1; v < n; v++)                              /* close the tour: v -> 0 */
        if (DP(full, v) + d[v][0] < best) { best = DP(full, v) + d[v][0]; last = v; }
    if (n == 1) { best = 0; last = 0; }

    if (trace) {
        printf("  dp[mask][end] (mask = visited set as bits 3210, only reachable states):\n");
        for (unsigned mask = 1; mask < states; mask += 2)
            for (int v = 0; v < n; v++)
                if (DP(mask, v) < INF && mask != 1) {
                    printf("    {");
                    for (int b = 0; b < n; b++) if (mask >> b & 1) printf("%d", b);
                    printf("} end %d : %3d  (came from %d)\n", v, DP(mask, v), PAR(mask, v));
                }
    }

    /* rebuild the tour backwards from (full, last) */
    unsigned mask = full;
    int v = last, k = n;
    tour[n] = 0;
    while (v != -1 && k > 0) {
        tour[--k] = v;
        int p = PAR(mask, v);
        mask &= ~(1u << v);
        v = p;
    }
    tour[0] = 0;
#undef DP
#undef PAR
    free(dp);
    free(par);
    return best;
}

/* brute force: all permutations of cities 1..n-1 (Heap's algorithm would do;
 * recursion with a used[] set is clearer). O(n!). */
static int brute_rec(int n, int d[MAXN][MAXN], int cur, int used, int count, int cost)
{
    if (count == n) return cost + d[cur][0];
    int best = INF;
    for (int w = 1; w < n; w++)
        if (!(used >> w & 1)) {
            int c = brute_rec(n, d, w, used | 1 << w, count + 1, cost + d[cur][w]);
            if (c < best) best = c;
        }
    return best;
}

static int nearest_neighbour(int n, int d[MAXN][MAXN])
{
    int used = 1, cur = 0, cost = 0;
    for (int step = 1; step < n; step++) {
        int nx = -1;
        for (int w = 0; w < n; w++) if (!(used >> w & 1) && (nx < 0 || d[cur][w] < d[cur][nx])) nx = w;
        cost += d[cur][nx]; used |= 1 << nx; cur = nx;
    }
    return cost + d[cur][0];
}

static int tour_cost(int n, int d[MAXN][MAXN], const int *t)
{
    int c = 0, seen = 0;
    for (int i = 0; i < n; i++) { c += d[t[i]][t[i + 1]]; seen |= 1 << t[i]; }
    CHECK(seen == (1 << n) - 1 && t[0] == 0 && t[n] == 0);   /* a real tour */
    return c;
}

static unsigned rng_state = 31415u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    int d[MAXN][MAXN] = {
        { 0, 10, 15, 20},
        {10,  0, 35, 25},
        {15, 35,  0, 30},
        {20, 25, 30,  0},
    };
    int tour[MAXN + 1];
    printf("Held-Karp on the 4-city example:\n");
    int best = held_karp(4, d, tour, 1);
    printf("  optimal tour:");
    for (int i = 0; i <= 4; i++) printf(" %d", tour[i]);
    printf("  cost %d\n", best);
    CHECK(best == 80 && tour_cost(4, d, tour) == 80);
    CHECK(brute_rec(4, d, 0, 1, 1, 0) == 80);

    /* A case where greedy nearest-neighbour is fooled. */
    int g[MAXN][MAXN] = {
        { 0, 1, 2, 4, 50},     /* greedy walks 0-1-2-3-4 (all cheap)...      */
        { 1, 0, 1, 9,  9},     /* ...and is then forced to pay 4->0 = 50      */
        { 2, 1, 0, 1,  3},
        { 4, 9, 1, 0,  1},
        {50, 9, 3, 1,  0},
    };
    int nn = nearest_neighbour(5, g), opt = held_karp(5, g, tour, 0);
    printf("\ngreedy trap (5 cities): nearest-neighbour = %d, optimal = %d\n", nn, opt);
    CHECK(opt == brute_rec(5, g, 0, 1, 1, 0) && nn > opt);

    for (int t = 0; t < 200; t++) {
        int n = 1 + (int)(rnd() % 8);
        for (int i = 0; i < n; i++)
            for (int j = 0; j <= i; j++) d[i][j] = d[j][i] = (i == j) ? 0 : 1 + (int)(rnd() % 100);
        int h = held_karp(n, d, tour, 0);
        int b = n == 1 ? 0 : brute_rec(n, d, 0, 1, 1, 0);
        CHECK(h == b);
        CHECK(tour_cost(n, d, tour) == h);
        CHECK(nearest_neighbour(n, d) >= h);
    }
    printf("random cross-check: 200 instances (n <= 8), Held-Karp == brute force,\n"
           "greedy never beats it\n");

    printf("\nwork estimate:   n   brute (n-1)!         Held-Karp 2^n*n^2\n");
    for (int n = 5; n <= 20; n += 5) {
        double f = 1; for (int i = 2; i < n; i++) f *= i;
        printf("                %2d   %-20.3g %.3g\n", n, f, (double)(1u << n) * n * n);
    }
    printf("OK\n");
    return 0;
}
