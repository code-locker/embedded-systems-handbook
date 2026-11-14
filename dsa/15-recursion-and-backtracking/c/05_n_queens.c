/*
 * 05_n_queens.c — N-Queens: place N queens so none attack each other
 *
 * Concept : One queen per row (that constraint is built into the recursion:
 *           depth = row). For each column, check column + both diagonals in
 *           O(1) with three boolean arrays, place, recurse, remove.
 *           Pruning is what makes it feasible: brute force over all N^N
 *           placements is 10^10 for N=10; backtracking visits ~35,000 nodes.
 *           Results are checked against the known solution counts (OEIS A000170)
 *           and against a brute-force "generate every permutation, then test"
 *           solver for small N.
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/05_n_queens.c
 * Read    : ../README.md  (section "N-Queens")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXN 12

/*
 * Why diagonals are indexed by r+c and r-c:
 *
 *   r+c constant along "/"         r-c constant along "\"
 *     c: 0 1 2 3                     c: 0  1  2  3
 *   r0   0 1 2 3                   r0   0 -1 -2 -3
 *   r1   1 2 3 4                   r1   1  0 -1 -2
 *   r2   2 3 4 5                   r2   2  1  0 -1
 *   r3   3 4 5 6                   r3   3  2  1  0
 *
 * So 2n-1 anti-diagonals (index r+c) and 2n-1 diagonals (index r-c+n-1).
 */
static int  n;
static int  col_of[MAXN];            /* col_of[r] = column of queen in row r  */
static char col_used[MAXN];
static char diag_used[2 * MAXN];     /* index r + c          ("/" direction)  */
static char anti_used[2 * MAXN];     /* index r - c + n - 1  ("\" direction)  */
static long solutions, nodes;
static int  first_board[MAXN], have_first;

static void solve(int r)
{
    ++nodes;
    if (r == n) {                                   /* all rows filled */
        if (!have_first) { memcpy(first_board, col_of, sizeof col_of); have_first = 1; }
        ++solutions;
        return;
    }
    for (int c = 0; c < n; ++c) {
        if (col_used[c] || diag_used[r + c] || anti_used[r - c + n - 1])
            continue;                               /* PRUNE: attacked square */
        col_of[r] = c;                              /* CHOOSE                 */
        col_used[c] = diag_used[r + c] = anti_used[r - c + n - 1] = 1;
        solve(r + 1);                               /* EXPLORE next row       */
        col_used[c] = diag_used[r + c] = anti_used[r - c + n - 1] = 0;  /* UN-CHOOSE */
    }
}

/* Independent validator: O(n^2) pairwise check of a finished board. */
static int board_ok(const int *q, int sz)
{
    for (int i = 0; i < sz; ++i)
        for (int j = i + 1; j < sz; ++j)
            if (q[i] == q[j] || abs(q[i] - q[j]) == j - i) return 0;
    return 1;
}

/* Brute force: try every permutation (n! of them), validate at the END only.
   No pruning — this is the "generate and test" approach backtracking beats. */
static long brute_count, brute_nodes;
static void brute(int *q, int sz, int k)
{
    ++brute_nodes;
    if (k == sz) { brute_count += board_ok(q, sz); return; }
    for (int i = k; i < sz; ++i) {
        int t = q[k]; q[k] = q[i]; q[i] = t;
        brute(q, sz, k + 1);
        t = q[k]; q[k] = q[i]; q[i] = t;
    }
}

static void print_board(const int *q, int sz)
{
    for (int r = 0; r < sz; ++r) {
        printf("  ");
        for (int c = 0; c < sz; ++c) printf("%c ", q[r] == c ? 'Q' : '.');
        printf("\n");
    }
}

int main(void)
{
    static const long known[MAXN + 1] = { 1, 1, 0, 0, 2, 10, 4, 40, 92, 352, 724, 2680, 14200 };

    /* Trace the very first steps for n = 4 so the backtracking is visible:
     *
     *  row0 col0: Q . . .     row1: col2 ok -> Q . . .    row2: no column safe!
     *                                            . . Q .    -> back up, row1 col3
     *  ...eventually the first solution:  . Q . .
     *                                     . . . Q
     *                                     Q . . .
     *                                     . . Q .
     */
    printf("  N  solutions  backtrack nodes   (brute-force nodes, n! perms)\n");
    for (n = 1; n <= MAXN; ++n) {
        solutions = nodes = 0; have_first = 0;
        memset(col_used, 0, sizeof col_used);
        memset(diag_used, 0, sizeof diag_used);
        memset(anti_used, 0, sizeof anti_used);
        solve(0);
        assert(solutions == known[n]);
        if (have_first) assert(board_ok(first_board, n));
        if (n <= 8) {
            int q[MAXN];
            for (int i = 0; i < n; ++i) q[i] = i;
            brute_count = brute_nodes = 0;
            brute(q, n, 0);
            assert(brute_count == solutions);
            printf("  %-2d %-10ld %-16ld  %ld\n", n, solutions, nodes, brute_nodes);
        } else {
            printf("  %-2d %-10ld %-16ld\n", n, solutions, nodes);
        }
        if (n == 4 || n == 8) {
            printf("  first solution for N=%d:\n", n);
            print_board(first_board, n);
        }
    }
    printf("\nall N-Queens counts match OEIS A000170 and brute force\n");
    return 0;
}
