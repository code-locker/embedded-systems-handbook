/*
 * 04_prefix_sums_kadane.c — prefix sums (1D and 2D), difference arrays, Kadane
 *
 * Concept : Prefix sums pay O(n) once so that ANY range sum costs O(1):
 *           sum(l..r) = P[r+1] - P[l]. The 2D version (summed-area table)
 *           is what image processing uses for box filters. A difference
 *           array is the inverse trick: O(1) per range UPDATE.
 *           Kadane finds the maximum-sum subarray in one O(n) pass by
 *           asking at each index "extend the previous run, or restart here?"
 * Build   : make run P=dsa/05-arrays-and-strings/c/04_prefix_sums_kadane.c
 * Read    : ../README.md  (sections "Prefix sums" and "Kadane")
 */
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
 * 1D prefix sums. P has n+1 entries; P[0] = 0 removes the "l == 0" special
 * case (the same "sentinel" idea as a dummy list head).
 *
 *   a:      3   1   4   1   5
 *   P:  0   3   4   8   9  14
 *           ^               ^
 *   sum(a[1..3]) = 1+4+1 = P[4] - P[1] = 9 - 3 = 6
 * ------------------------------------------------------------------------- */
static void build_prefix(const int *a, size_t n, long *P) /* O(n) */
{
    P[0] = 0;
    for (size_t i = 0; i < n; i++)
        P[i + 1] = P[i] + a[i];
}

static long range_sum(const long *P, size_t l, size_t r) /* O(1), inclusive */
{
    return P[r + 1] - P[l];
}

/* ---------------------------------------------------------------------------
 * 2D prefix sums (summed-area table / integral image).
 * S[r][c] = sum of the rectangle from (0,0) to (r-1,c-1).
 *
 *   build:  S[r+1][c+1] = a[r][c] + S[r][c+1] + S[r+1][c] - S[r][c]
 *                                    (above)     (left)    (counted twice)
 *
 *   query rectangle (r1,c1)..(r2,c2):
 *        +-----------+-----+
 *        |     A     |  B  |       want D = total - B-strip - C-strip + A
 *        +-----------+-----+
 *        |     C     |  D  |   D = S[r2+1][c2+1] - S[r1][c2+1]
 *        +-----------+-----+         - S[r2+1][c1] + S[r1][c1]
 * ------------------------------------------------------------------------- */
#define R 6
#define C 7
static long S[R + 1][C + 1];

static void build_2d(int a[R][C])
{
    memset(S, 0, sizeof S);
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++)
            S[r + 1][c + 1] = a[r][c] + S[r][c + 1] + S[r + 1][c] - S[r][c];
}

static long rect_sum(int r1, int c1, int r2, int c2) /* O(1) */
{
    return S[r2 + 1][c2 + 1] - S[r1][c2 + 1] - S[r2 + 1][c1] + S[r1][c1];
}

/* ---------------------------------------------------------------------------
 * Difference array: "add v to every element in [l, r]" in O(1).
 *   D[l] += v; D[r+1] -= v;   later a = prefix-sum of D in one O(n) pass.
 *   Used for e.g. "how many bookings cover each time slot".
 * ------------------------------------------------------------------------- */
static void range_add(long *D, size_t l, size_t r, long v)
{
    D[l] += v;
    D[r + 1] -= v; /* D has n+1 slots so r+1 is always valid */
}

/* ---------------------------------------------------------------------------
 * Kadane: maximum subarray sum (non-empty). O(n) time, O(1) space.
 *
 *   cur = best sum of a subarray that ENDS at i.
 *   Either extend the run ending at i-1, or start fresh at i:
 *       cur = max(a[i], cur + a[i])
 *   If cur went negative, it can only hurt whatever comes next -> restart.
 *
 *   a:    -2   1  -3   4  -1   2   1  -5   4
 *   cur:  -2   1  -2   4   3   5   6   1   5
 *   best: -2   1   1   4   4   5   6   6   6     -> [4 -1 2 1] = 6
 * ------------------------------------------------------------------------- */
static long kadane(const int *a, size_t n, size_t *from, size_t *to)
{
    long cur = a[0], best = a[0];
    size_t start = 0;
    *from = *to = 0;
    for (size_t i = 1; i < n; i++) {
        if (cur + a[i] < a[i]) { /* i.e. cur < 0: restart */
            cur = a[i];
            start = i;
        } else {
            cur += a[i];
        }
        if (cur > best) {
            best = cur;
            *from = start;
            *to = i;
        }
    }
    return best;
}

static long max_sub_brute(const int *a, size_t n) /* O(n^2) */
{
    long best = a[0];
    for (size_t i = 0; i < n; i++) {
        long s = 0;
        for (size_t j = i; j < n; j++) {
            s += a[j];
            if (s > best)
                best = s;
        }
    }
    return best;
}

int main(void)
{
    srand(2024);

    puts("== 1. 1D prefix sums ==");
    int a[] = {3, 1, 4, 1, 5, 9, 2, 6};
    size_t n = sizeof a / sizeof a[0];
    long P[9];
    build_prefix(a, n, P);
    CHECK(range_sum(P, 1, 3) == 6);
    CHECK(range_sum(P, 0, n - 1) == 31);
    for (size_t l = 0; l < n; l++)
        for (size_t r = l; r < n; r++) {
            long s = 0;
            for (size_t k = l; k <= r; k++)
                s += a[k];
            CHECK(range_sum(P, l, r) == s);
        }
    puts("  every (l, r) range matches a direct loop; sum(1..3) = 6");

    puts("\n== 2. 2D prefix sums (summed-area table) ==");
    int m[R][C];
    for (int r = 0; r < R; r++)
        for (int c = 0; c < C; c++)
            m[r][c] = rand() % 10;
    build_2d(m);
    int queries = 0;
    for (int r1 = 0; r1 < R; r1++)
        for (int c1 = 0; c1 < C; c1++)
            for (int r2 = r1; r2 < R; r2++)
                for (int c2 = c1; c2 < C; c2++) {
                    long s = 0;
                    for (int r = r1; r <= r2; r++)
                        for (int c = c1; c <= c2; c++)
                            s += m[r][c];
                    CHECK(rect_sum(r1, c1, r2, c2) == s);
                    queries++;
                }
    printf("  all %d sub-rectangles answered in O(1) and verified\n", queries);

    puts("\n== 3. Difference array: range updates ==");
    long D[11] = {0}; /* n = 10, one extra slot */
    range_add(D, 2, 5, 3);
    range_add(D, 0, 9, 1);
    range_add(D, 4, 4, 10);
    long run = 0, out[10];
    for (int k = 0; k < 10; k++)
        out[k] = run += D[k];
    long expect[10] = {1, 1, 4, 4, 14, 4, 1, 1, 1, 1};
    for (int k = 0; k < 10; k++)
        CHECK(out[k] == expect[k]);
    puts("  3 range-adds applied in O(1) each, resolved in one O(n) pass");

    puts("\n== 4. Kadane ==");
    int k1[] = {-2, 1, -3, 4, -1, 2, 1, -5, 4};
    size_t from, to;
    CHECK(kadane(k1, 9, &from, &to) == 6 && from == 3 && to == 6);
    printf("  max subarray = 6 at [%zu..%zu]\n", from, to);
    int allneg[] = {-5, -2, -8};
    CHECK(kadane(allneg, 3, &from, &to) == -2 && from == 1 && to == 1);
    puts("  all-negative input -> picks the largest single element (-2)");
    for (int round = 0; round < 500; round++) {
        int x[50];
        size_t len = 1 + (size_t)(rand() % 50);
        for (size_t q = 0; q < len; q++)
            x[q] = rand() % 41 - 20;
        long got = kadane(x, len, &from, &to);
        CHECK(got == max_sub_brute(x, len));
        long s = 0;
        for (size_t q = from; q <= to; q++)
            s += x[q];
        CHECK(s == got); /* reported range really has that sum */
    }
    puts("  500 random arrays agree with O(n^2) brute force");

    puts("\nall checks passed");
    return 0;
}
