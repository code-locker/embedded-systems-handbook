/*
 * 04_lis.c — longest increasing subsequence: O(n^2) DP and O(n log n)
 *            patience sorting, both with reconstruction
 *
 * Concept : O(n^2): dp[i] = length of the LIS that ENDS at a[i].
 *           O(n log n): tails[L] = smallest possible tail of an increasing
 *           subsequence of length L+1. tails is sorted, so each new element
 *           finds its place with binary search (lower_bound).
 * Build   : make run P=dsa/17-dynamic-programming/c/04_lis.c
 * Read    : ../README.md  (section "Longest increasing subsequence")
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

#define MAXN 64

static unsigned rng_state = 77u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Is out[0..len-1] a strictly increasing subsequence of a[0..n-1]? */
static int is_increasing_subseq(const int *a, int n, const int *out, int len)
{
    int j = 0;
    for (int i = 0; i < n && j < len; i++)
        if (a[i] == out[j]) j++;
    if (j != len) return 0;
    for (int k = 1; k < len; k++)
        if (out[k - 1] >= out[k]) return 0;
    return 1;
}

/* ----------------------------------------------------------- O(n^2) -----
 * State  : dp[i] = length of the longest strictly increasing subsequence
 *          that ENDS exactly at index i ("ends at" makes the state local:
 *          we know its last value, so we know what may come before it).
 * Choice : the previous element j < i with a[j] < a[i]
 *          dp[i] = 1 + max(dp[j]),  or 1 if no such j
 * Answer : max over all i (the LIS can end anywhere).
 * Time O(n^2), space O(n).
 */
static int lis_n2(const int *a, int n, int *out, int trace)
{
    int dp[MAXN], parent[MAXN];
    int best = 0, best_end = -1;
    for (int i = 0; i < n; i++) {
        dp[i] = 1;
        parent[i] = -1;
        for (int j = 0; j < i; j++)
            if (a[j] < a[i] && dp[j] + 1 > dp[i]) {
                dp[i] = dp[j] + 1;
                parent[i] = j;                 /* remember where we came from */
            }
        if (dp[i] > best) { best = dp[i]; best_end = i; }
    }
    if (trace) {
        printf("  i     :"); for (int i = 0; i < n; i++) printf(" %3d", i);   printf("\n");
        printf("  a[i]  :"); for (int i = 0; i < n; i++) printf(" %3d", a[i]); printf("\n");
        printf("  dp[i] :"); for (int i = 0; i < n; i++) printf(" %3d", dp[i]); printf("\n");
        printf("  parent:"); for (int i = 0; i < n; i++) printf(" %3d", parent[i]); printf("\n");
    }
    /* Follow parent links backwards from the best end, fill out[] from the back. */
    for (int k = best - 1, i = best_end; k >= 0; k--, i = parent[i]) out[k] = a[i];
    return best;
}

/* -------------------------------------------------------- O(n log n) ----
 * tails[L] = smallest tail value of any increasing subsequence of length L+1
 * seen so far. Smaller tails are better: they leave more room to extend.
 *
 * For each x: find the first tail >= x (lower_bound).
 *   - none?  x extends the longest subsequence: append.
 *   - found? replace it with x: same length, smaller tail.
 *
 * tails is NOT the LIS itself (it mixes elements from different
 * subsequences). To recover the LIS, store tail INDICES and a parent link.
 * Time O(n log n), space O(n).
 */
static int lis_nlogn(const int *a, int n, int *out, int trace)
{
    int tail_idx[MAXN], parent[MAXN], len = 0;
    for (int i = 0; i < n; i++) {
        int lo = 0, hi = len;                  /* lower_bound on a[tail_idx[]] */
        while (lo < hi) {
            int mid = lo + (hi - lo) / 2;
            if (a[tail_idx[mid]] < a[i]) lo = mid + 1;
            else hi = mid;
        }
        parent[i] = lo > 0 ? tail_idx[lo - 1] : -1;
        int appended = lo == len;
        tail_idx[lo] = i;
        if (appended) len++;
        if (trace) {
            printf("  x=%3d -> %-7s tails[%d]   tails:", a[i], appended ? "append" : "replace", lo);
            for (int k = 0; k < len; k++) printf(" %d", a[tail_idx[k]]);
            printf("\n");
        }
    }
    for (int k = len - 1, i = len ? tail_idx[len - 1] : -1; k >= 0; k--, i = parent[i])
        out[k] = a[i];
    return len;
}

/* Brute force: every subset of indices (n <= 16). */
static int lis_brute(const int *a, int n)
{
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        int last = 0, have = 0, len = 0, ok = 1;
        for (int i = 0; i < n && ok; i++)
            if (mask & (1u << i)) {
                if (have && a[i] <= last) ok = 0;
                last = a[i]; have = 1; len++;
            }
        if (ok && len > best) best = len;
    }
    return best;
}

int main(void)
{
    const int a[] = {10, 9, 2, 5, 3, 7, 101, 18, 4, 19};
    const int n = (int)(sizeof a / sizeof a[0]);
    int out[MAXN];

    printf("== O(n^2) DP: dp[i] = LIS ending at i ==\n");
    int l1 = lis_n2(a, n, out, 1);
    printf("LIS length %d:", l1);
    for (int k = 0; k < l1; k++) printf(" %d", out[k]);
    printf("\n");
    CHECK(l1 == 5);                       /* 2 3 7 18 19 */
    CHECK(is_increasing_subseq(a, n, out, l1));

    printf("\n== O(n log n): tails array after each element ==\n");
    int l2 = lis_nlogn(a, n, out, 1);
    printf("LIS length %d:", l2);
    for (int k = 0; k < l2; k++) printf(" %d", out[k]);
    printf("\n");
    CHECK(l2 == 5);
    CHECK(is_increasing_subseq(a, n, out, l2));

    CHECK(lis_n2(a, 0, out, 0) == 0 && lis_nlogn(a, 0, out, 0) == 0);

    for (int t = 0; t < 500; t++) {
        int m = (int)(rng() % 15), b[MAXN];
        for (int i = 0; i < m; i++) b[i] = (int)(rng() % 10);   /* many duplicates */
        int bf = lis_brute(b, m);
        int x = lis_n2(b, m, out, 0);
        CHECK(x == bf && is_increasing_subseq(b, m, out, x));
        int y = lis_nlogn(b, m, out, 0);
        CHECK(y == bf && is_increasing_subseq(b, m, out, y));
    }
    printf("\n500 random arrays (with duplicates): both methods == brute force,\n"
           "and every reconstructed sequence is a valid strictly increasing subsequence\n");

    printf("\nall checks passed\n");
    return 0;
}
