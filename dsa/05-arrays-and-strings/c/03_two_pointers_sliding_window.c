/*
 * 03_two_pointers_sliding_window.c — two pointers and sliding window
 *
 * Concept : Two indices walking an array let you replace an O(n^2) "try
 *           every pair / every subarray" loop with one O(n) pass, because
 *           each index only ever moves forward (or inward), so total moves
 *           <= 2n. Shown: pair-sum in a sorted array, remove duplicates in
 *           place, partition (Dutch flag), fixed window max-sum, variable
 *           window (shortest subarray with sum >= target, longest substring
 *           without repeats). Each is checked against a brute-force version.
 * Build   : make run P=dsa/05-arrays-and-strings/c/03_two_pointers_sliding_window.c
 * Read    : ../README.md  (sections "Two pointers" and "Sliding window")
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/* ---------------------------------------------------------------------------
 * 1. Pair with given sum in a SORTED array — opposite-end pointers. O(n).
 *
 *    target = 13
 *    [1  3  4  6  8  11]
 *     L               R     1+11=12 < 13  -> need bigger:  L++
 *        L            R     3+11=14 > 13  -> need smaller: R--
 *        L         R        3+8 =11 < 13  -> L++
 *           L      R        4+8 =12 < 13  -> L++
 *              L   R        6+8 =14 > 13  -> R--
 *              LR           L == R: pointers met -> no pair sums to 13
 *
 *    Why it is correct: when a[L]+a[R] < target, a[L] cannot pair with ANY
 *    element (a[R] was the biggest still possible), so discarding L is safe.
 * ------------------------------------------------------------------------- */
static bool pair_sum_sorted(const int *a, size_t n, int target, size_t *i,
                            size_t *j)
{
    if (n < 2)
        return false;
    size_t L = 0, R = n - 1;
    while (L < R) {
        int s = a[L] + a[R];
        if (s == target) {
            *i = L;
            *j = R;
            return true;
        }
        if (s < target)
            L++;
        else
            R--;
    }
    return false;
}

static bool pair_sum_brute(const int *a, size_t n, int target) /* O(n^2) */
{
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (a[i] + a[j] == target)
                return true;
    return false;
}

/* ---------------------------------------------------------------------------
 * 2. Remove duplicates from a sorted array in place — same-direction
 *    "read/write" pointers. O(n) time, O(1) space. Returns new length.
 *
 *    [1 1 2 2 2 3]       w = write slot, r = reader
 *     w r               a[r]==a[w]   skip
 *     w   r             a[r]!=a[w]   w++, a[w]=a[r]  -> [1 2 2 2 2 3]
 *       w       r       ...                          -> [1 2 3 | 2 2 3]
 *                                                        new len = 3
 * ------------------------------------------------------------------------- */
static size_t dedup_sorted(int *a, size_t n)
{
    if (n == 0)
        return 0;
    size_t w = 0;
    for (size_t r = 1; r < n; r++)
        if (a[r] != a[w])
            a[++w] = a[r];
    return w + 1;
}

/* ---------------------------------------------------------------------------
 * 3. Dutch national flag: sort 0/1/2 in one pass, three pointers. O(n).
 *
 *    invariant:  [0 .. lo)  all 0
 *                [lo .. mid) all 1
 *                [mid .. hi] unknown
 *                (hi .. n)  all 2
 *    (This is exactly quicksort's 3-way partition.)
 * ------------------------------------------------------------------------- */
static void swap_int(int *x, int *y)
{
    int t = *x;
    *x = *y;
    *y = t;
}

static void dutch_flag(int *a, size_t n)
{
    size_t lo = 0, mid = 0, hi = n; /* hi is one past the unknown region */
    while (mid < hi) {
        if (a[mid] == 0)
            swap_int(&a[lo++], &a[mid++]);
        else if (a[mid] == 1)
            mid++;
        else
            swap_int(&a[mid], &a[--hi]); /* don't advance mid: new a[mid] unseen */
    }
}

/* ---------------------------------------------------------------------------
 * 4. Fixed-size window: maximum sum of k consecutive elements. O(n).
 *
 *    k = 3      [2 1 5 1 3 2]
 *               [-----]          sum = 8
 *                 [-----]        sum = 8 - 2 + 1 = 7   (drop left, add right)
 *                   [-----]      sum = 7 - 1 + 3 = 9
 *    Each step is O(1) instead of re-summing k elements.
 * ------------------------------------------------------------------------- */
static long max_window_sum(const int *a, size_t n, size_t k)
{
    long s = 0;
    for (size_t i = 0; i < k; i++)
        s += a[i];
    long best = s;
    for (size_t r = k; r < n; r++) {
        s += a[r] - a[r - k];
        if (s > best)
            best = s;
    }
    return best;
}

static long max_window_brute(const int *a, size_t n, size_t k) /* O(n*k) */
{
    long best = 0;
    for (size_t i = 0; i + k <= n; i++) {
        long s = 0;
        for (size_t j = i; j < i + k; j++)
            s += a[j];
        if (i == 0 || s > best)
            best = s;
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * 5. Variable window: shortest subarray with sum >= target (all positive).
 *    Grow right until valid, then shrink left while still valid. O(n):
 *    L and R each move at most n times.
 *
 *    target 7, [2 3 1 2 4 3]
 *               L     R        sum 8 >= 7 -> record len 4, shrink: L++
 *                 L   R        sum 6 <  7 -> grow R
 *                 L     R      sum 10     -> len 4; shrink -> sum 7 len 3 ...
 *                         ...  best = [4 3], len 2
 * ------------------------------------------------------------------------- */
static size_t min_len_sum_at_least(const int *a, size_t n, long target)
{
    size_t best = 0; /* 0 means "no such window" */
    long s = 0;
    size_t L = 0;
    for (size_t R = 0; R < n; R++) {
        s += a[R];
        while (s >= target) {
            size_t len = R - L + 1;
            if (best == 0 || len < best)
                best = len;
            s -= a[L++];
        }
    }
    return best;
}

static size_t min_len_brute(const int *a, size_t n, long target)
{
    size_t best = 0;
    for (size_t i = 0; i < n; i++) {
        long s = 0;
        for (size_t j = i; j < n; j++) {
            s += a[j];
            if (s >= target) {
                if (best == 0 || j - i + 1 < best)
                    best = j - i + 1;
                break;
            }
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * 6. Longest substring without repeating characters. O(n), O(256) space.
 *    last[c] = last index where byte c was seen. When s[R] was seen inside
 *    the window, jump L just past that earlier occurrence.
 *
 *    "abcabcbb":  a b c a ...
 *                 L   R      window "abc", next 'a' was at 0 -> L = 1
 * ------------------------------------------------------------------------- */
static size_t longest_unique(const char *s)
{
    long last[256];
    for (int i = 0; i < 256; i++)
        last[i] = -1;
    size_t best = 0, L = 0;
    for (size_t R = 0; s[R]; R++) {
        unsigned char c = (unsigned char)s[R];
        if (last[c] >= (long)L)
            L = (size_t)last[c] + 1;
        last[c] = (long)R;
        if (R - L + 1 > best)
            best = R - L + 1;
    }
    return best;
}

static size_t longest_unique_brute(const char *s)
{
    size_t n = strlen(s), best = 0;
    for (size_t i = 0; i < n; i++) {
        bool seen[256] = {false};
        size_t j = i;
        while (j < n && !seen[(unsigned char)s[j]])
            seen[(unsigned char)s[j++]] = true;
        if (j - i > best)
            best = j - i;
    }
    return best;
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void)
{
    srand(12345); /* fixed seed: deterministic test */

    puts("== 1. pair sum in sorted array ==");
    int s1[] = {1, 3, 4, 6, 8, 11};
    size_t i, j;
    CHECK(pair_sum_sorted(s1, 6, 10, &i, &j) && s1[i] + s1[j] == 10);
    printf("  target 10 -> a[%zu]=%d + a[%zu]=%d\n", i, s1[i], j, s1[j]);
    CHECK(!pair_sum_sorted(s1, 6, 100, &i, &j));
    for (int round = 0; round < 500; round++) {
        int a[40];
        size_t n = (size_t)(rand() % 40);
        for (size_t k = 0; k < n; k++)
            a[k] = rand() % 50;
        qsort(a, n, sizeof a[0], cmp_int);
        int t = rand() % 100;
        CHECK(pair_sum_sorted(a, n, t, &i, &j) == pair_sum_brute(a, n, t));
    }
    puts("  500 random cases agree with brute force");

    puts("\n== 2. dedup sorted in place ==");
    int d[] = {1, 1, 2, 2, 2, 3, 5, 5};
    size_t dn = dedup_sorted(d, 8);
    CHECK(dn == 4 && d[0] == 1 && d[1] == 2 && d[2] == 3 && d[3] == 5);
    printf("  -> %zu unique: %d %d %d %d\n", dn, d[0], d[1], d[2], d[3]);

    puts("\n== 3. Dutch national flag ==");
    for (int round = 0; round < 200; round++) {
        int a[50], cnt[3] = {0};
        size_t n = (size_t)(rand() % 50);
        for (size_t k = 0; k < n; k++)
            cnt[a[k] = rand() % 3]++;
        dutch_flag(a, n);
        for (size_t k = 0; k < n; k++)
            CHECK(a[k] == (k < (size_t)cnt[0] ? 0
                           : k < (size_t)(cnt[0] + cnt[1]) ? 1 : 2));
    }
    puts("  200 random arrays of 0/1/2 sorted in one pass");

    puts("\n== 4. fixed window max sum ==");
    int w[] = {2, 1, 5, 1, 3, 2};
    CHECK(max_window_sum(w, 6, 3) == 9);
    for (int round = 0; round < 300; round++) {
        int a[60];
        size_t n = 1 + (size_t)(rand() % 60), k = 1 + (size_t)rand() % n;
        for (size_t q = 0; q < n; q++)
            a[q] = rand() % 21 - 10;
        CHECK(max_window_sum(a, n, k) == max_window_brute(a, n, k));
    }
    puts("  k=3 on [2 1 5 1 3 2] -> 9; 300 random cases agree");

    puts("\n== 5. shortest subarray with sum >= target ==");
    int v[] = {2, 3, 1, 2, 4, 3};
    CHECK(min_len_sum_at_least(v, 6, 7) == 2);
    CHECK(min_len_sum_at_least(v, 6, 100) == 0);
    for (int round = 0; round < 300; round++) {
        int a[60];
        size_t n = (size_t)(rand() % 60);
        for (size_t q = 0; q < n; q++)
            a[q] = 1 + rand() % 9; /* window trick needs positives */
        long t = 1 + rand() % 80;
        CHECK(min_len_sum_at_least(a, n, t) == min_len_brute(a, n, t));
    }
    puts("  target 7 on [2 3 1 2 4 3] -> 2; 300 random cases agree");

    puts("\n== 6. longest substring without repeats ==");
    CHECK(longest_unique("abcabcbb") == 3);
    CHECK(longest_unique("bbbbb") == 1);
    CHECK(longest_unique("pwwkew") == 3);
    CHECK(longest_unique("") == 0);
    for (int round = 0; round < 300; round++) {
        char str[64];
        size_t n = (size_t)(rand() % 63);
        for (size_t q = 0; q < n; q++)
            str[q] = (char)('a' + rand() % 6);
        str[n] = '\0';
        CHECK(longest_unique(str) == longest_unique_brute(str));
    }
    puts("  \"abcabcbb\" -> 3; 300 random strings agree");

    puts("\nall checks passed");
    return 0;
}
