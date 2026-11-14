/*
 * 06_interval_dp.c — interval DP: matrix chain multiplication and longest
 *                    palindromic subsequence
 *
 * Concept : dp[i][j] = answer for the sub-range i..j. Fill by INCREASING
 *           LENGTH, because a range depends only on shorter ranges inside it.
 *           MCM tries every split point k; LPS shrinks from both ends.
 * Build   : make run P=dsa/17-dynamic-programming/c/06_interval_dp.c
 * Read    : ../README.md  (sections "Matrix chain multiplication" and
 *           "Longest palindromic subsequence")
 */
#include <limits.h>
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

#define MAXM 16

static unsigned rng_state = 31337u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* --------------------------------------------------------------- MCM -----
 * Matrices A1..An, Ai is dims[i-1] x dims[i]. Multiplying (p x q)(q x r)
 * costs p*q*r scalar multiplications. Parenthesisation changes the cost a
 * lot, never the result.
 *
 * dp[i][j] = min cost to multiply Ai..Aj (1-based)
 * dp[i][i] = 0
 * dp[i][j] = min over k in [i, j) of
 *            dp[i][k] + dp[k+1][j] + dims[i-1]*dims[k]*dims[j]
 *                (Ai..Ak)   (Ak+1..Aj)   cost of the final multiply
 *
 * Fill order: by chain length len = 2..n (diagonals of the table):
 *
 *          j=1  2  3  4
 *    i=1  [ 0  a  d  f ]     a,b,c: len 2
 *    i=2  [    0  b  e ]     d,e  : len 3 (need a,b / b,c)
 *    i=3  [       0  c ]     f    : len 4
 *    i=4  [          0 ]
 *
 * Time O(n^3), space O(n^2).
 */
static long long mdp[MAXM + 1][MAXM + 1];
static int split[MAXM + 1][MAXM + 1];

static long long mcm(const int *dims, int n)
{
    for (int i = 1; i <= n; i++) mdp[i][i] = 0;
    for (int len = 2; len <= n; len++)
        for (int i = 1; i + len - 1 <= n; i++) {
            int j = i + len - 1;
            mdp[i][j] = LLONG_MAX;
            for (int k = i; k < j; k++) {
                long long c = mdp[i][k] + mdp[k + 1][j] +
                              (long long)dims[i - 1] * dims[k] * dims[j];
                if (c < mdp[i][j]) { mdp[i][j] = c; split[i][j] = k; }
            }
        }
    return mdp[1][n];
}

/* Rebuild "((A1A2)A3)" from the split table. pos indexes into buf. */
static void paren(int i, int j, char *buf, int *pos)
{
    if (i == j) { *pos += sprintf(buf + *pos, "A%d", i); return; }
    buf[(*pos)++] = '(';
    paren(i, split[i][j], buf, pos);
    paren(split[i][j] + 1, j, buf, pos);
    buf[(*pos)++] = ')';
    buf[*pos] = '\0';
}

/* Brute force: the same recursion, no memo — Catalan-many evaluations. */
static long long mcm_brute(const int *dims, int i, int j)
{
    if (i == j) return 0;
    long long best = LLONG_MAX;
    for (int k = i; k < j; k++) {
        long long c = mcm_brute(dims, i, k) + mcm_brute(dims, k + 1, j) +
                      (long long)dims[i - 1] * dims[k] * dims[j];
        if (c < best) best = c;
    }
    return best;
}

/* --------------------------------------------------------------- LPS -----
 * Longest palindromic subsequence of s[i..j].
 *   i == j            : 1
 *   s[i] == s[j]      : 2 + dp[i+1][j-1]      (both ends pair up)
 *   otherwise         : max(dp[i+1][j], dp[i][j-1])   (drop one end)
 * Same "by length" fill order. Time O(n^2), space O(n^2).
 * Fact: LPS(s) = LCS(s, reverse(s)) — checked below.
 */
#define MAXS 40
static int lps(const char *s, int trace)
{
    static int dp[MAXS][MAXS];
    int n = (int)strlen(s);
    if (n == 0) return 0;
    for (int i = 0; i < n; i++) dp[i][i] = 1;
    for (int len = 2; len <= n; len++)
        for (int i = 0; i + len - 1 < n; i++) {
            int j = i + len - 1;
            if (s[i] == s[j]) dp[i][j] = 2 + (len == 2 ? 0 : dp[i + 1][j - 1]);
            else dp[i][j] = dp[i + 1][j] > dp[i][j - 1] ? dp[i + 1][j] : dp[i][j - 1];
        }
    if (trace) {
        printf("       ");
        for (int j = 0; j < n; j++) printf("  %c", s[j]);
        printf("\n");
        for (int i = 0; i < n; i++) {
            printf("    %c  ", s[i]);
            for (int j = 0; j < n; j++)
                if (j < i) printf("  .");
                else printf(" %2d", dp[i][j]);
            printf("\n");
        }
    }
    return dp[0][n - 1];
}

static int lcs_len(const char *a, const char *b)
{
    static int dp[MAXS + 1][MAXS + 1];
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= m; j++)
            dp[i][j] = (!i || !j) ? 0
                       : a[i - 1] == b[j - 1] ? dp[i - 1][j - 1] + 1
                       : dp[i - 1][j] > dp[i][j - 1] ? dp[i - 1][j] : dp[i][j - 1];
    return dp[n][m];
}

static int lps_brute(const char *s)       /* every subsequence, n <= 14 */
{
    int n = (int)strlen(s), best = 0;
    char sub[MAXS];
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        int k = 0;
        for (int i = 0; i < n; i++)
            if (mask & (1u << i)) sub[k++] = s[i];
        int ok = 1;
        for (int x = 0, y = k - 1; x < y && ok; x++, y--) ok = sub[x] == sub[y];
        if (ok && k > best) best = k;
    }
    return best;
}

int main(void)
{
    printf("== Matrix chain: A1(10x30) A2(30x5) A3(5x60) ==\n");
    const int d3[] = {10, 30, 5, 60};
    char buf[128];
    int pos = 0;
    long long c = mcm(d3, 3);
    paren(1, 3, buf, &pos);
    printf("min cost %lld with %s   ((A1A2)A3 = 1500+3000; A1(A2A3) = 9000+18000)\n", c, buf);
    CHECK(c == 4500 && strcmp(buf, "((A1A2)A3)") == 0);

    const int d6[] = {30, 35, 15, 5, 10, 20, 25};   /* the CLRS example */
    pos = 0;
    c = mcm(d6, 6);
    paren(1, 6, buf, &pos);
    printf("CLRS 6-matrix chain: min cost %lld with %s\n", c, buf);
    CHECK(c == 15125 && strcmp(buf, "((A1(A2A3))((A4A5)A6))") == 0);
    printf("  dp table (cost of Ai..Aj), filled diagonal by diagonal:\n");
    for (int i = 1; i <= 6; i++) {
        printf("    ");
        for (int j = 1; j <= 6; j++)
            if (j < i) printf("      .");
            else printf(" %6lld", mdp[i][j]);
        printf("\n");
    }

    for (int t = 0; t < 200; t++) {
        int n = 1 + (int)(rng() % 8), dims[MAXM + 1];
        for (int i = 0; i <= n; i++) dims[i] = 1 + (int)(rng() % 20);
        CHECK(mcm(dims, n) == mcm_brute(dims, 1, n));
    }
    printf("200 random chains: DP == brute-force recursion\n");

    printf("\n== Longest palindromic subsequence of \"bbbab\" and \"character\" ==\n");
    int p = lps("bbbab", 1);
    printf("LPS(bbbab) = %d  (bbbb)\n", p);
    CHECK(p == 4);
    p = lps("character", 0);
    printf("LPS(character) = %d  (carac)\n", p);
    CHECK(p == 5);

    char s[MAXS], r[MAXS];
    for (int t = 0; t < 300; t++) {
        int n = (int)(rng() % 13);
        for (int i = 0; i < n; i++) s[i] = (char)('a' + rng() % 3);
        s[n] = '\0';
        for (int i = 0; i < n; i++) r[i] = s[n - 1 - i];
        r[n] = '\0';
        int v = lps(s, 0);
        CHECK(v == lps_brute(s));
        CHECK(v == lcs_len(s, r));
    }
    printf("300 random strings: LPS == brute force == LCS(s, reverse(s))\n");

    printf("\nall checks passed\n");
    return 0;
}
