/*
 * 05_reverse_and_rotate.c — in-place reversal and rotation
 *
 * Concept : Reverse = two pointers swapping inward, O(n) time, O(1) space.
 *           Rotate left by k with the "three reversals" trick:
 *              reverse(0..k-1), reverse(k..n-1), reverse(0..n-1)
 *           also O(n)/O(1). Compared with the naive O(n*k) shift and the
 *           O(n)-extra-space copy, plus the cycle ("juggling") method.
 *           Rotation is how ring buffers are "unwrapped" into linear order.
 * Build   : make run P=dsa/05-arrays-and-strings/c/05_reverse_and_rotate.c
 * Read    : ../README.md  (section "Reversal and rotation")
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

/* Reverse a[lo..hi) in place. O(hi - lo), O(1) space.
 *
 *   [1 2 3 4 5]      swap ends, move inward, stop when they meet
 *    L       R  ->  [5 2 3 4 1]
 *      L   R    ->  [5 4 3 2 1]
 *        LR         done (middle stays)
 *
 * Half-open [lo, hi) avoids the classic bug of hi = n-1 underflowing
 * when n == 0 with size_t indices. */
static void reverse(int *a, size_t lo, size_t hi)
{
    while (lo + 1 < hi) { /* at least two elements left */
        hi--;
        int t = a[lo];
        a[lo] = a[hi];
        a[hi] = t;
        lo++;
    }
}

/* Rotate left by k with three reversals. O(n), O(1) space.
 *
 *   k = 2,  [1 2 | 3 4 5 6 7]
 *   rev A:  [2 1 | 3 4 5 6 7]
 *   rev B:  [2 1 | 7 6 5 4 3]
 *   rev all:[3 4 5 6 7 | 1 2]     <- (A^r B^r)^r = B A
 *
 * Why: reversing the whole thing puts B first and A last but each piece
 * is backwards; pre-reversing each piece cancels that out. */
static void rotate_left(int *a, size_t n, size_t k)
{
    if (n == 0)
        return;
    k %= n; /* rotating by n is the identity */
    reverse(a, 0, k);
    reverse(a, k, n);
    reverse(a, 0, n);
}

static void rotate_right(int *a, size_t n, size_t k)
{
    if (n == 0)
        return;
    rotate_left(a, n, n - k % n); /* right by k == left by n-k */
}

/* Naive: shift by one, k times. O(n*k) — fine for k=1, terrible for k=n/2. */
static void rotate_left_naive(int *a, size_t n, size_t k)
{
    if (n == 0)
        return;
    k %= n;
    for (size_t s = 0; s < k; s++) {
        int first = a[0];
        memmove(a, a + 1, (n - 1) * sizeof *a);
        a[n - 1] = first;
    }
}

/* Copy method: O(n) time but O(n) extra memory. */
static void rotate_left_copy(int *a, size_t n, size_t k)
{
    if (n == 0)
        return;
    k %= n;
    int *tmp = malloc(n * sizeof *tmp); /* owned here, freed below */
    CHECK(tmp != NULL);
    for (size_t i = 0; i < n; i++)
        tmp[i] = a[(i + k) % n];
    memcpy(a, tmp, n * sizeof *a);
    free(tmp);
}

static size_t gcd(size_t x, size_t y)
{
    while (y) {
        size_t t = x % y;
        x = y;
        y = t;
    }
    return x;
}

/* Juggling / cycle-leader: element at i+k moves to i. The index chain
 * i -> i+k -> i+2k ... (mod n) forms gcd(n,k) separate cycles; walk each
 * once. O(n) time, O(1) space, each element moved exactly once (fewest
 * writes of all methods — matters for slow memories like flash). */
static void rotate_left_juggle(int *a, size_t n, size_t k)
{
    if (n == 0)
        return;
    k %= n;
    if (k == 0)
        return;
    size_t cycles = gcd(n, k);
    for (size_t start = 0; start < cycles; start++) {
        int t = a[start];
        size_t i = start;
        for (;;) {
            size_t next = (i + k) % n;
            if (next == start)
                break;
            a[i] = a[next];
            i = next;
        }
        a[i] = t;
    }
}

/* Reverse a C string in place (bytes). O(n). */
static void reverse_str(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n / 2; i++) {
        char t = s[i];
        s[i] = s[n - 1 - i];
        s[n - 1 - i] = t;
    }
}

static void fill(int *a, size_t n)
{
    for (size_t i = 0; i < n; i++)
        a[i] = (int)i + 1;
}

static void print(const char *label, const int *a, size_t n)
{
    printf("  %-18s[", label);
    for (size_t i = 0; i < n; i++)
        printf("%s%d", i ? " " : "", a[i]);
    puts("]");
}

int main(void)
{
    puts("== 1. reverse ==");
    int a[7];
    fill(a, 7);
    reverse(a, 0, 7);
    print("reverse all", a, 7);
    for (int i = 0; i < 7; i++)
        CHECK(a[i] == 7 - i);
    reverse(a, 0, 0); /* empty range: no-op, no underflow */
    reverse(a, 3, 4); /* one element: no-op */
    CHECK(a[3] == 4);

    char s[] = "BeagleBone"; /* array copy on the stack: writable */
    reverse_str(s);
    CHECK(strcmp(s, "enoBelgaeB") == 0);
    printf("  reverse_str       \"%s\"\n", s);

    puts("\n== 2. rotate (three reversals) ==");
    fill(a, 7);
    rotate_left(a, 7, 2);
    print("rotate_left 2", a, 7);
    CHECK(a[0] == 3 && a[4] == 7 && a[5] == 1 && a[6] == 2);
    rotate_right(a, 7, 2);
    print("rotate_right 2", a, 7);
    for (int i = 0; i < 7; i++)
        CHECK(a[i] == i + 1);
    rotate_left(a, 7, 7 + 3); /* k > n wraps */
    CHECK(a[0] == 4);

    puts("\n== 3. all four methods agree for every n <= 30, every k ==");
    for (size_t n = 0; n <= 30; n++) {
        for (size_t k = 0; k <= 2 * n + 1; k++) {
            int x1[31], x2[31], x3[31], x4[31];
            fill(x1, n);
            fill(x2, n);
            fill(x3, n);
            fill(x4, n);
            rotate_left(x1, n, k);
            rotate_left_naive(x2, n, k);
            rotate_left_copy(x3, n, k);
            rotate_left_juggle(x4, n, k);
            for (size_t i = 0; i < n; i++) {
                int want = (int)((i + k) % n) + 1;
                CHECK(x1[i] == want && x2[i] == want);
                CHECK(x3[i] == want && x4[i] == want);
            }
        }
    }
    puts("  reversal / naive / copy / juggling: identical results");

    puts("\n== 4. real use: unwrap a ring buffer into linear order ==");
    /* A full ring buffer of 8 audio samples whose oldest element sits at
     * index 5 (head). Rotating left by head yields oldest-first order. */
    int ring[8] = {6, 7, 8, 9, 10, 1, 2, 3};
    /*            ^newest        ^head (oldest)                        */
    rotate_left(ring, 8, 5);
    print("unwrapped", ring, 8);
    int want[8] = {1, 2, 3, 6, 7, 8, 9, 10};
    CHECK(memcmp(ring, want, sizeof want) == 0);

    puts("\nall checks passed");
    return 0;
}
