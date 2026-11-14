/*
 * 07_subset_enumeration.c — bitmasks as sets: enumerate all subsets, all
 *                           submasks of a mask, all k-subsets (Gosper's hack)
 *
 * Concept : A set drawn from n <= 64 items IS an n-bit integer: bit i set
 *           means "item i is in". Union = |, intersection = &, difference
 *           = & ~, complement = ^ full, size = popcount. Counting 0..2^n-1
 *           visits every subset exactly once. (s - 1) & m walks every submask
 *           of m; total over all m is 3^n, the basis of bitmask DP (lesson 17).
 * Build   : make run P=dsa/18-bit-manipulation/c/07_subset_enumeration.c
 * Read    : ../README.md  (section "3.9 Subset enumeration with bitmasks")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void print_set(uint32_t s, const char *names)
{
    putchar('{');
    int first = 1;
    for (int i = 0; names[i]; i++)
        if (s >> i & 1u) {
            printf(first ? "%c" : ",%c", names[i]);
            first = 0;
        }
    putchar('}');
}

/* Subset sum by brute force over all 2^n masks: O(2^n * n).
 * Returns the number of subsets whose sum == target. */
static int count_subset_sums(const int *a, int n, int target)
{
    int count = 0;
    for (uint32_t mask = 0; mask < (1u << n); mask++) {
        int sum = 0;
        for (int i = 0; i < n; i++)
            if (mask >> i & 1u)
                sum += a[i];
        count += (sum == target);
    }
    return count;
}

/* Recursive include/exclude reference (independent method to check against). */
static int count_rec(const int *a, int n, int i, int target)
{
    if (i == n)
        return target == 0;
    return count_rec(a, n, i + 1, target) + count_rec(a, n, i + 1, target - a[i]);
}

/* Gosper's hack: next larger integer with the same popcount.
 *   x = 0011 0110
 *   c = x & -x           = 0000 0010   lowest set bit
 *   r = x + c            = 0011 1000   carry ripples through the lowest run of 1s
 *   (r ^ x)              = 0000 1110   the bits that changed (run + 1)
 *   >> 2 / c             = shift that run down to bit 0, dropping 2 extra bits
 *   next = r | that      = 0011 1001
 * Precondition: x != 0. */
static uint32_t gosper_next(uint32_t x)
{
    uint32_t c = x & (0u - x);
    uint32_t r = x + c;
    return (((r ^ x) >> 2) / c) | r;
}

static long binom(int n, int k)
{
    long b = 1;
    for (int i = 1; i <= k; i++)
        b = b * (n - k + i) / i;
    return b;
}

int main(void)
{
    const char *names = "ABC";

    puts("== 1. All subsets of {A,B,C}: count mask from 0 to 2^3-1 ==");
    /*
     *  mask  bits(CBA)  subset
     *   0     000       {}
     *   1     001       {A}
     *   2     010       {B}
     *   3     011       {A,B}
     *   ...
     *   7     111       {A,B,C}
     */
    for (uint32_t m = 0; m < 8; m++) {
        printf("  %u  %u%u%u  ", (unsigned)m, (unsigned)(m >> 2 & 1u), (unsigned)(m >> 1 & 1u), (unsigned)(m & 1u));
        print_set(m, names);
        putchar('\n');
    }

    puts("\n== 2. Set algebra with bit operators ==");
    uint32_t X = 0x0B, Y = 0x06;   /* {A,B,D} and {B,C} */
    assert((X | Y) == 0x0F);       /* union        {A,B,C,D} */
    assert((X & Y) == 0x02);       /* intersection {B}       */
    assert((X & ~Y) == 0x09);      /* difference   {A,D}     */
    assert((X ^ Y) == 0x0D);       /* symmetric difference   */
    assert(((X & Y) == Y) == 0);   /* Y subset of X?  no     */
    assert((0x02u & X) == 0x02u);  /* {B} subset of X? yes   */
    puts("union, intersection, difference, subset test: OK");

    puts("\n== 3. Subset-sum count: 2^n bitmask loop vs recursion ==");
    int a[] = { 3, 34, 4, 12, 5, 2, 7, 8, 1, 9, 6, 11, 10, 13, 14, 15 };
    int n = (int)(sizeof a / sizeof a[0]);
    for (int target = 0; target <= 60; target += 3) {
        int bm = count_subset_sums(a, n, target);
        assert(bm == count_rec(a, n, 0, target));
    }
    printf("subsets of 16 numbers summing to 9: %d (bitmask == recursion for 21 targets)\n",
           count_subset_sums(a, n, 9));

    puts("\n== 4. Enumerate submasks of m: s = (s - 1) & m ==");
    /* Subtracting 1 clears the lowest set bit and sets all lower bits; & m
     * throws away bits not in m. So s steps DOWN through exactly the submasks
     * of m, in decreasing order. Loop must handle the empty set specially
     * because (0 - 1) & m == m would loop forever. */
    uint32_t m = 0x0D;   /* 1101 = {A,C,D} */
    printf("submasks of 1101:");
    int cnt = 0;
    for (uint32_t s = m;; s = (s - 1u) & m) {
        printf(" %u%u%u%u", (unsigned)(s >> 3 & 1u), (unsigned)(s >> 2 & 1u),
               (unsigned)(s >> 1 & 1u), (unsigned)(s & 1u));
        assert((s & ~m) == 0);
        cnt++;
        if (s == 0)
            break;
    }
    putchar('\n');
    assert(cnt == 8);   /* 2^popcount(m) */
    /* Sum over every mask of 2^popcount(mask) == 3^n: each item is either
     * out of m, in m but not s, or in both. */
    for (int bits = 1; bits <= 12; bits++) {
        long total = 0, three = 1;
        for (uint32_t mm = 0; mm < (1u << bits); mm++) {
            for (uint32_t s = mm;; s = (s - 1u) & mm) {
                total++;
                if (s == 0)
                    break;
            }
        }
        for (int i = 0; i < bits; i++)
            three *= 3;
        assert(total == three);
    }
    puts("total submask iterations over all masks of n bits == 3^n for n=1..12: OK");

    puts("\n== 5. All k-subsets in increasing order (Gosper's hack) ==");
    printf("3-subsets of 5 items:");
    int k = 3, nn = 5, seen = 0;
    for (uint32_t x = (1u << k) - 1u; x < (1u << nn); x = gosper_next(x)) {
        printf(" %02X", (unsigned)x);
        assert(__builtin_popcount(x) == k);
        seen++;
    }
    putchar('\n');
    assert(seen == 10);
    for (nn = 1; nn <= 16; nn++)
        for (k = 1; k <= nn; k++) {
            long c = 0;
            uint32_t prev = 0;
            for (uint32_t x = (1u << k) - 1u; x < (1u << nn); x = gosper_next(x)) {
                assert(x > prev);                   /* strictly increasing */
                assert(__builtin_popcount(x) == k);
                prev = x;
                c++;
                if (x == ((1u << nn) - 1u))         /* last one: avoid overflow in gosper_next at 32 bits */
                    break;
            }
            assert(c == binom(nn, k));
        }
    puts("Gosper count == C(n,k) for all 1<=k<=n<=16: OK");

    puts("\nall checks passed");
    return 0;
}
