/*
 * 07_grid_and_partition.c — grid DP (unique paths with obstacles, minimum
 *                           path sum) and subset-sum / partition problems
 *
 * Concept : Grid DP: a cell is reached only from above or from the left, so
 *           dp[r][c] combines dp[r-1][c] and dp[r][c-1]; fill row by row.
 *           Partition: "can a subset reach sum s?" is a boolean 0/1 knapsack.
 *           Equal partition, minimum-difference partition and "count subsets
 *           with sum s" all fall out of the same table.
 * Build   : make run P=dsa/17-dynamic-programming/c/07_grid_and_partition.c
 * Read    : ../README.md  (sections "Grid paths" and "Partition problems")
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

#define MAXG 12
#define MAXSUM 1024

static unsigned rng_state = 4242u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------- unique paths, blocked --
 * Robot at top-left, moves only right or down, '#' cells are blocked.
 * dp[r][c] = number of paths from (0,0) to (r,c).
 *   blocked          : 0
 *   (0,0)            : 1
 *   otherwise        : dp[r-1][c] (came from above) + dp[r][c-1] (from left)
 * Time O(R*C), space O(R*C) (one row would do: dp[c] += dp[c-1]).
 */
static long long unique_paths(const char g[][MAXG + 1], int R, int C, int trace)
{
    long long dp[MAXG][MAXG];
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) {
            if (g[r][c] == '#') dp[r][c] = 0;
            else if (r == 0 && c == 0) dp[r][c] = 1;
            else dp[r][c] = (r ? dp[r - 1][c] : 0) + (c ? dp[r][c - 1] : 0);
        }
    if (trace)
        for (int r = 0; r < R; r++) {
            printf("    ");
            for (int c = 0; c < C; c++)
                if (g[r][c] == '#') printf("   #");
                else printf(" %3lld", dp[r][c]);
            printf("\n");
        }
    return dp[R - 1][C - 1];
}

static long long paths_brute(const char g[][MAXG + 1], int R, int C, int r, int c)
{
    if (r >= R || c >= C || g[r][c] == '#') return 0;
    if (r == R - 1 && c == C - 1) return 1;
    return paths_brute(g, R, C, r + 1, c) + paths_brute(g, R, C, r, c + 1);
}

/* ------------------------------------------------------ min path sum ----
 * dp[r][c] = cheapest cost to reach (r,c) = cost[r][c] + min(above, left).
 * Space-optimised to ONE row: before the update dp[c] still holds "above",
 * dp[c-1] already holds "left" of the current row.
 */
static int min_path_sum(const int cost[][MAXG], int R, int C)
{
    int dp[MAXG];
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++) {
            if (r == 0 && c == 0) dp[c] = cost[0][0];
            else if (r == 0) dp[c] = dp[c - 1] + cost[r][c];
            else if (c == 0) dp[c] = dp[c] + cost[r][c];
            else dp[c] = (dp[c] < dp[c - 1] ? dp[c] : dp[c - 1]) + cost[r][c];
        }
    return dp[C - 1];
}

static int min_path_brute(const int cost[][MAXG], int R, int C, int r, int c)
{
    if (r == R - 1 && c == C - 1) return cost[r][c];
    int best = 1 << 30;
    if (r + 1 < R) { int v = min_path_brute(cost, R, C, r + 1, c); if (v < best) best = v; }
    if (c + 1 < C) { int v = min_path_brute(cost, R, C, r, c + 1); if (v < best) best = v; }
    return best + cost[r][c];
}

/* --------------------------------------------------------- subset sum ---
 * can[s] = 1 if some subset of the items seen so far sums to s.
 * Each item used at most once -> loop s DOWNWARD (0/1 knapsack rule).
 *
 *   items {3,1,5}:      s: 0 1 2 3 4 5 6 7 8 9
 *   start                  1 . . . . . . . . .
 *   +3                     1 . . 1 . . . . . .
 *   +1                     1 1 . 1 1 . . . . .
 *   +5                     1 1 . 1 1 1 1 . 1 1
 *
 * Time O(n*S), space O(S). (Pseudo-polynomial: fine while S is small.)
 */
static void subset_sums(const int *a, int n, unsigned char *can, int S)
{
    memset(can, 0, (size_t)S + 1);
    can[0] = 1;
    for (int i = 0; i < n; i++)
        for (int s = S; s >= a[i]; s--)
            if (can[s - a[i]]) can[s] = 1;
}

/* Equal partition: total must be even and total/2 must be reachable. */
static int can_partition(const int *a, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++) total += a[i];
    if (total % 2) return 0;
    unsigned char can[MAXSUM + 1];
    subset_sums(a, n, can, total / 2);
    return can[total / 2];
}

/* Min |sumA - sumB|: the reachable s closest to total/2 from below. */
static int min_partition_diff(const int *a, int n)
{
    int total = 0;
    for (int i = 0; i < n; i++) total += a[i];
    unsigned char can[MAXSUM + 1];
    subset_sums(a, n, can, total / 2);
    for (int s = total / 2; s >= 0; s--)
        if (can[s]) return total - 2 * s;
    return total;
}

/* Number of subsets with sum exactly S: replace OR by +. */
static long long count_subsets(const int *a, int n, int S)
{
    long long ways[MAXSUM + 1] = {0};
    ways[0] = 1;
    for (int i = 0; i < n; i++)
        for (int s = S; s >= a[i]; s--) ways[s] += ways[s - a[i]];
    return ways[S];
}

int main(void)
{
    printf("== Unique paths with obstacles (counts per cell) ==\n");
    const char g[4][MAXG + 1] = {"....", ".#..", "...#", "#..."};
    long long p = unique_paths(g, 4, 4, 1);
    printf("paths to bottom-right = %lld\n", p);
    CHECK(p == paths_brute(g, 4, 4, 0, 0));
    const char open[3][MAXG + 1] = {".......", ".......", "......."};
    CHECK(unique_paths(open, 3, 7, 0) == 28);          /* C(8,2) */

    printf("\n== Minimum path sum ==\n");
    const int cost[3][MAXG] = {{1, 3, 1}, {1, 5, 1}, {4, 2, 1}};
    int mp = min_path_sum(cost, 3, 3);
    printf("cheapest path cost = %d  (1->3->1->1->1)\n", mp);
    CHECK(mp == 7);

    char rg[MAXG][MAXG + 1];
    int rc[MAXG][MAXG];
    for (int t = 0; t < 200; t++) {
        int R = 1 + (int)(rng() % 7), C = 1 + (int)(rng() % 7);
        for (int r = 0; r < R; r++) {
            for (int c = 0; c < C; c++) {
                rg[r][c] = (rng() % 5 == 0) ? '#' : '.';
                rc[r][c] = (int)(rng() % 10);
            }
            rg[r][C] = '\0';
        }
        CHECK(unique_paths((const char(*)[MAXG + 1])rg, R, C, 0) ==
              paths_brute((const char(*)[MAXG + 1])rg, R, C, 0, 0));
        CHECK(min_path_sum((const int(*)[MAXG])rc, R, C) ==
              min_path_brute((const int(*)[MAXG])rc, R, C, 0, 0));
    }
    printf("200 random grids: path counts and min sums == brute force\n");

    printf("\n== Partition problems ==\n");
    const int a1[] = {1, 5, 11, 5}, a2[] = {1, 2, 3, 5}, a3[] = {1, 6, 11, 5};
    printf("{1,5,11,5}: equal partition? %s  ({11} vs {1,5,5})\n", can_partition(a1, 4) ? "yes" : "no");
    printf("{1,2,3,5} : equal partition? %s  (total 11 is odd)\n", can_partition(a2, 4) ? "yes" : "no");
    printf("{1,6,11,5}: min difference = %d  ({1,5,6}=12 vs {11})\n", min_partition_diff(a3, 4));
    CHECK(can_partition(a1, 4) && !can_partition(a2, 4));
    CHECK(min_partition_diff(a3, 4) == 1);
    const int a4[] = {2, 3, 5, 6, 8, 10};
    printf("{2,3,5,6,8,10}: subsets summing to 10 = %lld  ({10},{2,8},{2,3,5})\n",
           count_subsets(a4, 6, 10));
    CHECK(count_subsets(a4, 6, 10) == 3);

    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rng() % 12), a[16], total = 0;
        for (int i = 0; i < n; i++) { a[i] = 1 + (int)(rng() % 20); total += a[i]; }
        int S = (int)(rng() % (unsigned)(total + 1));
        int bf_equal = 0, bf_diff = total;
        long long bf_count = 0;
        for (unsigned mask = 0; mask < (1u << n); mask++) {
            int s = 0;
            for (int i = 0; i < n; i++) if (mask & (1u << i)) s += a[i];
            if (2 * s == total) bf_equal = 1;
            int d = abs(total - 2 * s);
            if (d < bf_diff) bf_diff = d;
            if (s == S) bf_count++;
        }
        CHECK(can_partition(a, n) == bf_equal);
        CHECK(min_partition_diff(a, n) == bf_diff);
        CHECK(count_subsets(a, n, S) == bf_count);
    }
    printf("300 random multisets: partition / min-diff / count == brute force\n");

    printf("\nall checks passed\n");
    return 0;
}
