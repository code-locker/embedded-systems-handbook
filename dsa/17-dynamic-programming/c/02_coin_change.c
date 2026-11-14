/*
 * 02_coin_change.c — coin change: minimum number of coins, and number of ways
 *
 * Concept : dp[a] = best answer for amount a, built from smaller amounts.
 *           Min coins:   dp[a] = 1 + min over coins c of dp[a-c].
 *           Count ways:  loop COINS outside, AMOUNTS inside, so each multiset
 *           of coins is counted once (swapping the loops counts orderings).
 *           Also shows why greedy fails for {1,3,4}.
 * Build   : make run P=dsa/17-dynamic-programming/c/02_coin_change.c
 * Read    : ../README.md  (section "Coin change")
 */
#include <limits.h>
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

#define MAXA 512
#define INF  (INT_MAX / 2)   /* "unreachable"; /2 so INF+1 cannot overflow */

static unsigned rng_state = 12345u;
static unsigned rng(void)    /* xorshift32: deterministic, so failures reproduce */
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ------------------------------------------------------------ min coins --
 * State   : dp[a] = fewest coins summing to exactly a (INF if impossible)
 * Base    : dp[0] = 0
 * Choice  : the last coin used is some c <= a, leaving a-c to pay
 * Order   : a = 1..amount, since dp[a] reads only smaller amounts
 * Time O(amount * k), space O(amount).
 */
static int min_coins(const int *coins, int k, int amount, int trace)
{
    int dp[MAXA + 1];
    dp[0] = 0;
    for (int a = 1; a <= amount; a++) {
        dp[a] = INF;
        for (int j = 0; j < k; j++)
            if (coins[j] <= a && dp[a - coins[j]] + 1 < dp[a])
                dp[a] = dp[a - coins[j]] + 1;
        if (trace) {
            printf("  a=%2d  dp:", a);
            for (int x = 0; x <= amount; x++) {
                if (x > a) printf("  .");
                else if (dp[x] >= INF) printf(x == a ? " [∞]" : "  ∞");
                else printf(x == a ? " [%d]" : " %2d", dp[x]);
            }
            printf("\n");
        }
    }
    return dp[amount] >= INF ? -1 : dp[amount];
}

/* Brute force: try every coin as the next coin, recursively. Exponential. */
static int min_coins_brute(const int *coins, int k, int amount)
{
    if (amount == 0) return 0;
    int best = INF;
    for (int j = 0; j < k; j++) {
        if (coins[j] > amount) continue;
        int sub = min_coins_brute(coins, k, amount - coins[j]);
        if (sub != INF && sub + 1 < best) best = sub + 1;
    }
    return best;
}

/* Greedy: always take the largest coin that fits. Coins sorted descending.
 * Works for "canonical" systems like {1,2,5,10,...} but NOT in general. */
static int greedy_coins(const int *desc, int k, int amount)
{
    int count = 0;
    for (int j = 0; j < k; j++)
        while (desc[j] <= amount) { amount -= desc[j]; count++; }
    return amount == 0 ? count : -1;
}

/* --------------------------------------------------------- count ways ----
 * Number of MULTISETS of coins that sum to amount (order does not matter:
 * 1+2 and 2+1 are the same way).
 *
 * Coins in the OUTER loop: after processing coin j, ways[a] counts the ways
 * to make a using only coins[0..j]. A coin can be reused (inner loop goes
 * upward, so ways[a-c] already includes coin c) — unbounded choice.
 *
 *   coins {1,2,5}, amount 5:
 *              a: 0 1 2 3 4 5
 *   start         1 0 0 0 0 0
 *   after c=1     1 1 1 1 1 1     only 1s
 *   after c=2     1 1 2 2 3 3     e.g. 4 = 1111, 112, 22
 *   after c=5     1 1 2 2 3 4     5 = 11111, 1112, 122, 5
 */
static long long count_ways(const int *coins, int k, int amount)
{
    long long ways[MAXA + 1] = {0};
    ways[0] = 1;                              /* the empty selection */
    for (int j = 0; j < k; j++)
        for (int a = coins[j]; a <= amount; a++)
            ways[a] += ways[a - coins[j]];
    return ways[amount];
}

/* Amount OUTER, coins inner: counts ordered sequences (1+2 != 2+1).
 * Same code, loops swapped, different answer — the classic interview trap. */
static long long count_orderings(const int *coins, int k, int amount)
{
    long long ways[MAXA + 1] = {0};
    ways[0] = 1;
    for (int a = 1; a <= amount; a++)
        for (int j = 0; j < k; j++)
            if (coins[j] <= a) ways[a] += ways[a - coins[j]];
    return ways[amount];
}

/* Brute force for multisets: decide how many of coin j to take, then move on
 * to coin j+1. Never goes back to earlier coins, so each multiset once. */
static long long count_ways_brute(const int *coins, int k, int j, int amount)
{
    if (amount == 0) return 1;
    if (j == k) return 0;
    long long total = 0;
    for (int used = 0; used * coins[j] <= amount; used++)
        total += count_ways_brute(coins, k, j + 1, amount - used * coins[j]);
    return total;
}

int main(void)
{
    printf("== Min coins, coins {1,2,5}, amount 11, table filled cell by cell ==\n");
    const int c125[] = {1, 2, 5};
    int r = min_coins(c125, 3, 11, 1);
    printf("min coins for 11 = %d  (5+5+1)\n", r);
    CHECK(r == 3);

    printf("\n== Greedy fails for coins {1,3,4}, amount 6 ==\n");
    const int c134[] = {1, 3, 4}, c431[] = {4, 3, 1};
    int g = greedy_coins(c431, 3, 6), d = min_coins(c134, 3, 6, 0);
    printf("greedy: 4+1+1 = %d coins, DP: 3+3 = %d coins\n", g, d);
    CHECK(g == 3 && d == 2);

    const int c2[] = {2};
    CHECK(min_coins(c2, 1, 3, 0) == -1);      /* odd amount, only 2s: impossible */

    printf("\n== Number of ways ==\n");
    long long w = count_ways(c125, 3, 5), o = count_orderings(c125, 3, 5);
    printf("coins {1,2,5}, amount 5: %lld multisets, %lld ordered sequences\n", w, o);
    CHECK(w == 4 && o == 9);
    printf("coins {1,2,5}, amount 100: %lld ways\n", count_ways(c125, 3, 100));
    CHECK(count_ways(c125, 3, 100) == 541);

    /* Randomised check against brute force. */
    for (int t = 0; t < 300; t++) {
        int k = 1 + (int)(rng() % 4), coins[4];
        for (int j = 0; j < k; j++) coins[j] = 1 + (int)(rng() % 9);
        int amount = (int)(rng() % 25);
        int bf = min_coins_brute(coins, k, amount);
        int dp = min_coins(coins, k, amount, 0);
        CHECK(dp == (bf == INF ? -1 : bf));
        /* duplicate coin values would double-count in both versions equally,
         * so the comparison is still fair */
        CHECK(count_ways(coins, k, amount) == count_ways_brute(coins, k, 0, amount));
    }
    printf("300 random coin systems: DP == brute force for min coins and ways\n");

    printf("\nall checks passed\n");
    return 0;
}
