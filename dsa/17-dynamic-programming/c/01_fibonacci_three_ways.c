/*
 * 01_fibonacci_three_ways.c — Fibonacci and climbing stairs: naive recursion,
 *                             top-down memo, bottom-up table, O(1) space
 *
 * Concept : The same recurrence solved four ways. Naive recursion recomputes
 *           the same subproblems exponentially often; memoisation caches them
 *           (top-down); tabulation fills them in dependency order (bottom-up);
 *           space optimisation keeps only the cells the recurrence still reads.
 * Build   : make run P=dsa/17-dynamic-programming/c/01_fibonacci_three_ways.c
 * Read    : ../README.md  (sections "Top-down vs bottom-up vs space-optimised"
 *           and "Fibonacci and climbing stairs")
 */
#include <inttypes.h>
#include <stdint.h>
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

/* fib(93) overflows uint64_t, so every version here is limited to n <= 90. */
#define MAXN 91

/* ---------------------------------------------------------------- naive --
 * fib(n) = fib(n-1) + fib(n-2). The recursion tree for fib(5):
 *
 *                       fib5
 *                /                \
 *            fib4                  fib3          <- fib3 computed twice
 *          /      \              /      \
 *       fib3      fib2        fib2      fib1     <- fib2 computed 3 times
 *      /    \     /  \        /  \
 *   fib2  fib1 fib1 fib0  fib1 fib0
 *   /  \
 * fib1 fib0
 *
 * The tree has ~phi^n nodes (phi = 1.618), so time is O(1.618^n). Only n+1
 * DISTINCT nodes exist: that gap is exactly what DP removes.
 */
static unsigned long long naive_calls;

static uint64_t fib_naive(int n)
{
    naive_calls++;
    if (n < 2) return (uint64_t)n;           /* base cases: fib0=0, fib1=1 */
    return fib_naive(n - 1) + fib_naive(n - 2);
}

/* ---------------------------------------------------------------- memo ---
 * Top-down: same recursion, but each answer is written into memo[] the first
 * time it is computed. Every later call for that n returns in O(1).
 * Time O(n) (each n is computed once), space O(n) memo + O(n) call stack.
 */
static unsigned long long memo_calls;

static uint64_t fib_memo_rec(int n, uint64_t *memo, int *known)
{
    memo_calls++;
    if (n < 2) return (uint64_t)n;
    if (known[n]) return memo[n];            /* cache hit: no recursion */
    memo[n] = fib_memo_rec(n - 1, memo, known) + fib_memo_rec(n - 2, memo, known);
    known[n] = 1;
    return memo[n];
}

static uint64_t fib_memo(int n)
{
    uint64_t memo[MAXN];
    int known[MAXN];
    memset(known, 0, sizeof known);          /* "not computed yet" marker */
    return fib_memo_rec(n, memo, known);
}

/* ---------------------------------------------------------------- table --
 * Bottom-up: fill dp[0..n] left to right. dp[i] only depends on dp[i-1] and
 * dp[i-2], which are already filled when we reach i. No recursion, no stack.
 * Time O(n), space O(n).
 */
static uint64_t fib_table(int n, int trace)
{
    uint64_t dp[MAXN];
    dp[0] = 0;
    if (n >= 1) dp[1] = 1;
    for (int i = 2; i <= n; i++) {
        dp[i] = dp[i - 1] + dp[i - 2];
        if (trace) {
            /* print the table after filling cell i:  dp: 0 1 1 2 3 [5] . . */
            printf("  i=%2d  dp:", i);
            for (int j = 0; j <= n; j++) {
                if (j < i) printf(" %" PRIu64, dp[j]);
                else if (j == i) printf(" [%" PRIu64 "]", dp[j]);
                else printf(" .");
            }
            printf("\n");
        }
    }
    return dp[n];
}

/* ------------------------------------------------------------ two vars ---
 * Space optimisation: the recurrence only looks back 2 cells, so keep 2.
 *     prev2  prev1  ->  cur = prev1 + prev2, then slide the window right.
 * Time O(n), space O(1). This is the version you would ship.
 */
static uint64_t fib_two_vars(int n)
{
    if (n < 2) return (uint64_t)n;
    uint64_t prev2 = 0, prev1 = 1;
    for (int i = 2; i <= n; i++) {
        uint64_t cur = prev1 + prev2;
        prev2 = prev1;
        prev1 = cur;
    }
    return prev1;
}

/* ------------------------------------------------------ climbing stairs --
 * You climb n stairs taking 1 or 2 steps at a time. How many ways?
 * The LAST move was either a 1-step (from stair n-1) or a 2-step (from n-2):
 *     ways(n) = ways(n-1) + ways(n-2),  ways(0)=1, ways(1)=1
 * That is Fibonacci shifted by one: ways(n) = fib(n+1).
 *
 * Generalised: allowed steps given in an array (e.g. {1,3,5}).
 *     ways(n) = sum over step s <= n of ways(n-s)
 */
static uint64_t stairs_dp(int n, const int *steps, int k)
{
    uint64_t dp[MAXN];
    dp[0] = 1;                               /* one way to stand still */
    for (int i = 1; i <= n; i++) {
        dp[i] = 0;
        for (int j = 0; j < k; j++)
            if (steps[j] <= i) dp[i] += dp[i - steps[j]];
    }
    return dp[n];
}

/* Brute force: literally enumerate every sequence of moves. Exponential. */
static uint64_t stairs_brute(int n, const int *steps, int k)
{
    if (n == 0) return 1;
    uint64_t total = 0;
    for (int j = 0; j < k; j++)
        if (steps[j] <= n) total += stairs_brute(n - steps[j], steps, k);
    return total;
}

int main(void)
{
    printf("== Fibonacci: naive recursion vs memo vs table vs two variables ==\n");

    naive_calls = 0;
    uint64_t f25 = fib_naive(25);
    memo_calls = 0;
    uint64_t m25 = fib_memo(25);
    printf("fib(25) = %" PRIu64 "\n", f25);
    printf("  naive recursion made %llu calls  (exponential: ~1.618^n)\n", naive_calls);
    printf("  memoised recursion made %llu calls (linear: about 2n)\n", memo_calls);
    CHECK(f25 == 75025);
    CHECK(m25 == f25);
    CHECK(naive_calls == 242785);            /* 2*fib(26) - 1 */
    CHECK(memo_calls <= 2 * 25);

    printf("\nBottom-up table for fib(10), one cell per step:\n");
    uint64_t t10 = fib_table(10, 1);
    CHECK(t10 == 55);

    /* All four must agree. Naive is only affordable for small n. */
    for (int n = 0; n <= 90; n++) {
        uint64_t a = fib_memo(n), b = fib_table(n, 0), c = fib_two_vars(n);
        CHECK(a == b && b == c);
        if (n <= 22) CHECK(fib_naive(n) == a);
    }
    printf("\nmemo == table == two-vars for n = 0..90; naive agrees for n <= 22\n");
    printf("fib(90) = %" PRIu64 "\n", fib_two_vars(90));
    CHECK(fib_two_vars(90) == UINT64_C(2880067194370816120));

    printf("\n== Climbing stairs ==\n");
    const int s12[] = {1, 2};
    for (int n = 0; n <= 20; n++) {
        CHECK(stairs_dp(n, s12, 2) == fib_two_vars(n + 1));
        CHECK(stairs_dp(n, s12, 2) == stairs_brute(n, s12, 2));
    }
    printf("steps {1,2}: ways(10) = %" PRIu64 " = fib(11)\n", stairs_dp(10, s12, 2));

    const int s135[] = {1, 3, 5};
    for (int n = 0; n <= 25; n++)
        CHECK(stairs_dp(n, s135, 3) == stairs_brute(n, s135, 3));
    printf("steps {1,3,5}: ways(10) = %" PRIu64 " (DP agrees with brute force for n <= 25)\n",
           stairs_dp(10, s135, 3));

    printf("\nall checks passed\n");
    return 0;
}
