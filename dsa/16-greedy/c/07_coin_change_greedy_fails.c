/*
 * 07_coin_change_greedy_fails.c — when "take the biggest coin" is right and when it is wrong
 *
 * Concept : Greedy coin change is optimal for "canonical" systems like
 *           {1,2,5,10,20,50,100,200} (euro, rupee) but fails for odd ones like
 *           {1,3,4}: amount 6 -> greedy 4+1+1 (3 coins), optimum 3+3 (2 coins).
 *           We find counter-examples automatically by comparing greedy with DP,
 *           and use the Kozen-Zaks bound: if greedy is wrong at all, it is wrong
 *           for some amount below c_max + c_second_max.
 * Build   : make run P=dsa/16-greedy/c/07_coin_change_greedy_fails.c
 * Read    : ../README.md  (section "When greedy fails")
 */
#include <assert.h>
#include <stdio.h>

#define MAXAMT 1000
#define INF 1000000

/* coins sorted ascending, coins[0] == 1 so every amount is payable.
 * O(n) per amount (O(amount/coin) with subtraction, we use division). */
static int greedy_coins(const int *coins, int n, int amount)
{
    int count = 0;
    for (int i = n - 1; i >= 0; i--) {   /* largest coin first — the greedy choice */
        count += amount / coins[i];
        amount %= coins[i];
    }
    return amount == 0 ? count : INF;
}

/* dp[a] = min coins for amount a. O(amount * n). The correct answer (see 17-dynamic-programming). */
static void dp_coins(const int *coins, int n, int max_amount, int *dp)
{
    dp[0] = 0;
    for (int a = 1; a <= max_amount; a++) {
        dp[a] = INF;
        for (int i = 0; i < n; i++)
            if (coins[i] <= a && dp[a - coins[i]] + 1 < dp[a]) dp[a] = dp[a - coins[i]] + 1;
    }
}

/* Returns the smallest amount where greedy is worse than optimal, or -1. */
static int first_counterexample(const int *coins, int n)
{
    static int dp[MAXAMT + 1];
    int limit = n >= 2 ? coins[n - 1] + coins[n - 2] : 1; /* Kozen & Zaks 1994 */
    assert(limit <= MAXAMT);
    dp_coins(coins, n, limit, dp);
    for (int a = 1; a < limit; a++)
        if (greedy_coins(coins, n, a) != dp[a]) return a;
    return -1;
}

static void report(const char *name, const int *coins, int n)
{
    int ce = first_counterexample(coins, n);
    printf("%-28s ", name);
    if (ce < 0) {
        printf("greedy is OPTIMAL for every amount\n");
    } else {
        static int dp[MAXAMT + 1];
        dp_coins(coins, n, ce, dp);
        printf("greedy FAILS at %d: greedy %d coins, optimal %d coins\n",
               ce, greedy_coins(coins, n, ce), dp[ce]);
    }
}

int main(void)
{
    int euro[] = {1, 2, 5, 10, 20, 50, 100, 200};
    int us[] = {1, 5, 10, 25};
    int odd[] = {1, 3, 4};
    int old_uk[] = {1, 3, 6, 12, 24, 30}; /* pre-1971 British: penny..half-crown */
    int weird[] = {1, 7, 10};

    report("euro {1,2,5,...,200}", euro, 8);
    report("US {1,5,10,25}", us, 4);
    report("{1,3,4}", odd, 3);
    report("old UK {1,3,6,12,24,30}", old_uk, 6);
    report("{1,7,10}", weird, 3);

    assert(first_counterexample(euro, 8) == -1);
    assert(first_counterexample(us, 4) == -1);
    assert(first_counterexample(odd, 3) == 6);       /* 4+1+1 vs 3+3 */
    assert(first_counterexample(old_uk, 6) == 48);   /* 30+12+6 vs 24+24 */
    assert(first_counterexample(weird, 3) == 14);    /* 10+1+1+1+1 vs 7+7 */

    /* How often is greedy wrong for {1,3,4}? Not rare: count over 1..500. */
    static int dp[MAXAMT + 1];
    dp_coins(odd, 3, 500, dp);
    int fails = 0;
    for (int a = 1; a <= 500; a++) fails += greedy_coins(odd, 3, a) != dp[a];
    printf("{1,3,4}: greedy wrong for %d of the amounts 1..500\n", fails);
    assert(fails > 0);
    return 0;
}
