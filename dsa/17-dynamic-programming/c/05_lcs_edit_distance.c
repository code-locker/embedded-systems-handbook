/*
 * 05_lcs_edit_distance.c — longest common subsequence and edit distance
 *                          (Levenshtein), tables printed, answers rebuilt
 *
 * Concept : Two-string DP. dp[i][j] = answer for the PREFIXES a[0..i) and
 *           b[0..j). Look at the last characters: if they match, use the
 *           diagonal; otherwise take the best of the neighbours. LCS is what
 *           `diff` is built on; edit distance powers spell checkers and DNA
 *           alignment.
 * Build   : make run P=dsa/17-dynamic-programming/c/05_lcs_edit_distance.c
 * Read    : ../README.md  (sections "LCS" and "Edit distance")
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

#define MAXL 32

static unsigned rng_state = 99u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int max_i(int a, int b) { return a > b ? a : b; }
static int min3(int a, int b, int c)
{
    int m = a < b ? a : b;
    return m < c ? m : c;
}

/* Prints dp with a's characters down the side and b's across the top.
 * '-' is the empty prefix (row 0 / column 0). */
static void print_table(const char *a, const char *b, int dp[MAXL + 1][MAXL + 1])
{
    int n = (int)strlen(a), m = (int)strlen(b);
    printf("         -");
    for (int j = 0; j < m; j++) printf("  %c", b[j]);
    printf("\n");
    for (int i = 0; i <= n; i++) {
        printf("    %c  ", i ? a[i - 1] : '-');
        for (int j = 0; j <= m; j++) printf(" %2d", dp[i][j]);
        printf("\n");
    }
}

/* ----------------------------------------------------------------- LCS ---
 * dp[i][j] = LCS length of a[0..i) and b[0..j)
 *   a[i-1] == b[j-1]:  dp[i][j] = dp[i-1][j-1] + 1        (use the match)
 *   otherwise       :  dp[i][j] = max(dp[i-1][j], dp[i][j-1])  (drop one char)
 *
 *        dp[i-1][j-1]  dp[i-1][j]
 *                    \     |
 *        dp[i][j-1] -- dp[i][j]
 *
 * Time O(n*m), space O(n*m) (O(min) with two rows, see lcs_two_rows).
 * out receives one LCS string (caller provides MAXL+1 bytes).
 */
static int lcs(const char *a, const char *b, char *out, int trace)
{
    static int dp[MAXL + 1][MAXL + 1];
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= m; j++) {
            if (i == 0 || j == 0) dp[i][j] = 0;
            else if (a[i - 1] == b[j - 1]) dp[i][j] = dp[i - 1][j - 1] + 1;
            else dp[i][j] = max_i(dp[i - 1][j], dp[i][j - 1]);
        }
    if (trace) print_table(a, b, dp);
    /* Walk back from the corner: a diagonal step on a match emits a char. */
    int len = dp[n][m], k = len, i = n, j = m;
    out[len] = '\0';
    while (i > 0 && j > 0) {
        if (a[i - 1] == b[j - 1]) { out[--k] = a[i - 1]; i--; j--; }
        else if (dp[i - 1][j] >= dp[i][j - 1]) i--;
        else j--;
    }
    return len;
}

/* Only the previous row is ever read, so two rows suffice: O(m) space. */
static int lcs_two_rows(const char *a, const char *b)
{
    int prev[MAXL + 1] = {0}, cur[MAXL + 1] = {0};
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int i = 1; i <= n; i++) {
        cur[0] = 0;
        for (int j = 1; j <= m; j++)
            cur[j] = a[i - 1] == b[j - 1] ? prev[j - 1] + 1 : max_i(prev[j], cur[j - 1]);
        memcpy(prev, cur, sizeof cur);
    }
    return prev[m];
}

static int lcs_brute(const char *a, const char *b)   /* the raw recursion */
{
    if (!*a || !*b) return 0;
    if (*a == *b) return 1 + lcs_brute(a + 1, b + 1);
    return max_i(lcs_brute(a + 1, b), lcs_brute(a, b + 1));
}

static int is_subsequence(const char *s, const char *t)   /* s in t? */
{
    while (*s && *t) { if (*s == *t) s++; t++; }
    return *s == '\0';
}

/* ------------------------------------------------------- edit distance ---
 * dp[i][j] = min edits to turn a[0..i) into b[0..j)
 * Base: dp[i][0] = i (delete all), dp[0][j] = j (insert all)
 *   a[i-1] == b[j-1]: dp[i-1][j-1]                       (free)
 *   else 1 + min( dp[i-1][j-1]   replace a[i-1] by b[j-1]
 *                 dp[i-1][j]     delete  a[i-1]
 *                 dp[i][j-1] )   insert  b[j-1]
 * Time O(n*m), space O(n*m).
 */
static int edit_distance(const char *a, const char *b, int trace)
{
    static int dp[MAXL + 1][MAXL + 1];
    int n = (int)strlen(a), m = (int)strlen(b);
    for (int i = 0; i <= n; i++) dp[i][0] = i;
    for (int j = 0; j <= m; j++) dp[0][j] = j;
    for (int i = 1; i <= n; i++)
        for (int j = 1; j <= m; j++)
            dp[i][j] = a[i - 1] == b[j - 1]
                           ? dp[i - 1][j - 1]
                           : 1 + min3(dp[i - 1][j - 1], dp[i - 1][j], dp[i][j - 1]);
    if (trace) {
        print_table(a, b, dp);
        /* Recover the edit script by walking back from the corner. Collected
         * in reverse, printed forwards. */
        char ops[2 * MAXL][48];
        int nops = 0, i = n, j = m;
        while (i > 0 || j > 0) {
            if (i > 0 && j > 0 && a[i - 1] == b[j - 1] && dp[i][j] == dp[i - 1][j - 1]) {
                i--; j--;
            } else if (i > 0 && j > 0 && dp[i][j] == dp[i - 1][j - 1] + 1) {
                snprintf(ops[nops++], sizeof ops[0], "replace '%c' -> '%c' (pos %d)", a[i - 1], b[j - 1], i - 1);
                i--; j--;
            } else if (i > 0 && dp[i][j] == dp[i - 1][j] + 1) {
                snprintf(ops[nops++], sizeof ops[0], "delete  '%c'        (pos %d)", a[i - 1], i - 1);
                i--;
            } else {
                snprintf(ops[nops++], sizeof ops[0], "insert  '%c'        (after pos %d)", b[j - 1], i - 1);
                j--;
            }
        }
        for (int k = nops - 1; k >= 0; k--) printf("    %s\n", ops[k]);
        CHECK(nops == dp[n][m]);
    }
    return dp[n][m];
}

static int edit_brute(const char *a, const char *b)
{
    if (!*a) return (int)strlen(b);
    if (!*b) return (int)strlen(a);
    if (*a == *b) return edit_brute(a + 1, b + 1);
    return 1 + min3(edit_brute(a + 1, b + 1), edit_brute(a + 1, b), edit_brute(a, b + 1));
}

static void random_string(char *s, int len)
{
    for (int i = 0; i < len; i++) s[i] = (char)('a' + rng() % 3);
    s[len] = '\0';
}

int main(void)
{
    char out[MAXL + 1];

    printf("== LCS of ABCBDAB and BDCABA ==\n");
    int l = lcs("ABCBDAB", "BDCABA", out, 1);
    printf("LCS length %d, one LCS = \"%s\"\n", l, out);
    CHECK(l == 4);
    CHECK(is_subsequence(out, "ABCBDAB") && is_subsequence(out, "BDCABA"));
    CHECK(lcs_two_rows("ABCBDAB", "BDCABA") == 4);

    printf("\n== Edit distance kitten -> sitting ==\n");
    int e = edit_distance("kitten", "sitting", 1);
    printf("edit distance = %d\n", e);
    CHECK(e == 3);
    printf("\n== Edit distance sunday -> saturday ==\n");
    CHECK(edit_distance("sunday", "saturday", 1) == 3);
    CHECK(edit_distance("", "abc", 0) == 3 && edit_distance("abc", "", 0) == 3);

    char a[MAXL + 1], b[MAXL + 1];
    for (int t = 0; t < 400; t++) {
        random_string(a, (int)(rng() % 8));
        random_string(b, (int)(rng() % 8));
        int L = lcs(a, b, out, 0);
        CHECK(L == lcs_brute(a, b));
        CHECK(L == lcs_two_rows(a, b));
        CHECK((int)strlen(out) == L && is_subsequence(out, a) && is_subsequence(out, b));
        int E = edit_distance(a, b, 0);
        CHECK(E == edit_brute(a, b));
        /* relation: with only insert+delete, distance = n + m - 2*LCS;
         * replace can only help, so E <= that. */
        CHECK(E <= (int)(strlen(a) + strlen(b)) - 2 * L);
    }
    printf("\n400 random string pairs: LCS and edit distance == brute-force recursion\n");

    printf("\nall checks passed\n");
    return 0;
}
