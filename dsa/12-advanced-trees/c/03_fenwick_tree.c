/*
 * 03_fenwick_tree.c — Fenwick tree (Binary Indexed Tree): prefix sums with updates
 *
 * Concept : bit[i] (1-based) stores the sum of the LOWBIT(i) elements ending
 *           at i, where lowbit(i) = i & -i is the value of i's lowest set bit.
 *           Query walks DOWN by stripping the lowest bit (i -= i & -i);
 *           update walks UP by adding it (i += i & -i). Each loop runs at
 *           most log2(n) times because one bit position changes per step.
 * Build   : make run P=dsa/12-advanced-trees/c/03_fenwick_tree.c
 * Read    : ../README.md  (section "Fenwick tree")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Which range does bit[i] cover?   [i - lowbit(i) + 1, i]
 *
 *   i  binary  lowbit  covers
 *   1  0001     1      [1,1]
 *   2  0010     2      [1,2]
 *   3  0011     1      [3,3]
 *   4  0100     4      [1,4]
 *   5  0101     1      [5,5]
 *   6  0110     2      [5,6]
 *   7  0111     1      [7,7]
 *   8  1000     8      [1,8]
 *
 * Two's complement: -i = ~i + 1, so i & -i keeps only the lowest 1 bit.
 *   i  = 0110 1000
 *  ~i  = 1001 0111
 *  -i  = 1001 1000
 * i&-i = 0000 1000
 */
static unsigned lowbit(unsigned i) { return i & (0u - i); } /* unsigned avoids signed-overflow UB */

typedef struct {
    int n;
    long long *bit; /* 1-based, bit[0] unused */
} Fenwick;

static Fenwick fw_create(int n)
{
    Fenwick f = { n, calloc((size_t)n + 1, sizeof(long long)) };
    assert(f.bit);
    return f;
}

static void fw_free(Fenwick *f) { free(f->bit); }

/* a[i] += delta  (i is 1-based). O(log n): climb to every node covering i. */
static void fw_add(Fenwick *f, int i, long long delta)
{
    for (unsigned k = (unsigned)i; k <= (unsigned)f->n; k += lowbit(k))
        f->bit[k] += delta;
}

/* sum of a[1..i]. O(log n): split [1, i] into disjoint lowbit-sized blocks. */
static long long fw_prefix(const Fenwick *f, int i)
{
    long long s = 0;
    for (unsigned k = (unsigned)i; k > 0; k -= lowbit(k))
        s += f->bit[k];
    return s;
}

/* sum of a[l..r] = prefix(r) - prefix(l-1). O(log n). */
static long long fw_range(const Fenwick *f, int l, int r) { return fw_prefix(f, r) - fw_prefix(f, l - 1); }

/*
 * O(n) build: each node pushes its total into its parent i + lowbit(i) once.
 * (Calling fw_add n times would be O(n log n).)
 */
static void fw_build(Fenwick *f, const long long *a /* 1-based */)
{
    for (int i = 1; i <= f->n; i++)
        f->bit[i] = a[i];
    for (unsigned i = 1; i <= (unsigned)f->n; i++) {
        unsigned parent = i + lowbit(i);
        if (parent <= (unsigned)f->n)
            f->bit[parent] += f->bit[i];
    }
}

static void print_bin4(unsigned x)
{
    for (int b = 3; b >= 0; b--)
        putchar((x >> b) & 1u ? '1' : '0');
}

static uint32_t rng_state = 88172645u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    /* Show the walk of a query and an update in binary. */
    printf("prefix(7) visits: ");
    for (unsigned k = 7; k > 0; k -= lowbit(k)) {
        print_bin4(k);
        printf("(%u) ", k);
    }
    printf("   <- strip lowest bit each step\n");
    printf("add(3)    visits: ");
    for (unsigned k = 3; k <= 8; k += lowbit(k)) {
        print_bin4(k);
        printf("(%u) ", k);
    }
    printf("   <- add lowest bit each step\n");
    assert(lowbit(12) == 4 && lowbit(7) == 1 && lowbit(8) == 8);

    /* small worked example: a = [_, 3, 2, -1, 6, 5, 4, -3, 3] (1-based) */
    long long a[] = { 0, 3, 2, -1, 6, 5, 4, -3, 3 };
    Fenwick f = fw_create(8);
    fw_build(&f, a);
    printf("bit[] = ");
    for (int i = 1; i <= 8; i++)
        printf("%lld ", f.bit[i]);
    printf("\n");
    assert(f.bit[4] == 3 + 2 - 1 + 6 && f.bit[8] == 19 && f.bit[6] == 9);
    assert(fw_prefix(&f, 7) == 16 && fw_range(&f, 3, 5) == 10);
    fw_add(&f, 3, 10); /* a[3] = 9 */
    assert(fw_range(&f, 3, 5) == 20 && fw_prefix(&f, 8) == 29);
    printf("prefix(7) = 16, sum[3..5] = 10, after a[3]+=10 -> sum[3..5] = %lld\n", fw_range(&f, 3, 5));
    fw_free(&f);

    /* randomised check: compare against a plain array */
    enum { N = 1000, OPS = 30000 };
    long long *ref = calloc(N + 1, sizeof *ref);
    assert(ref);
    for (int i = 1; i <= N; i++)
        ref[i] = (long long)(rnd() % 100);
    Fenwick g = fw_create(N);
    fw_build(&g, ref);
    for (int op = 0; op < OPS; op++) {
        int l = (int)(rnd() % N) + 1, r = (int)(rnd() % N) + 1;
        if (l > r) { int tmp = l; l = r; r = tmp; }
        if (rnd() % 2) {
            long long d = (long long)(rnd() % 201) - 100;
            ref[l] += d;
            fw_add(&g, l, d);
        } else {
            long long want = 0;
            for (int i = l; i <= r; i++) want += ref[i];
            assert(fw_range(&g, l, r) == want);
        }
    }
    printf("random test: %d ops on n=%d match brute force\n", OPS, N);
    fw_free(&g);
    free(ref);
    printf("OK\n");
    return 0;
}
