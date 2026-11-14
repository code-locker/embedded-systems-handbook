/*
 * 04_popcount_power_of_two.c — counting bits, power-of-two tests, lowest set
 *                              bit, highest set bit, round up to power of two
 *
 * Concept : x & (x - 1) deletes the LOWEST set bit. That one identity gives
 *           Kernighan's popcount (loop once per set bit) and the power-of-two
 *           test (a power of two has exactly one set bit). x & -x ISOLATES the
 *           lowest set bit. Compilers expose hardware instructions as
 *           __builtin_popcount / __builtin_ctz / __builtin_clz.
 * Build   : make run P=dsa/18-bit-manipulation/c/04_popcount_power_of_two.c
 * Read    : ../README.md  (section "3.5 Popcount, powers of two, lowest set bit")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* O(32) always: look at every bit. The reference everything is checked against. */
static int popcount_naive(uint32_t x)
{
    int c = 0;
    for (int i = 0; i < 32; i++)
        c += (int)((x >> i) & 1u);
    return c;
}

/* Kernighan: O(number of set bits).
 *
 *   x       = 0101 1000
 *   x - 1   = 0101 0111   <- borrowing flips the lowest 1 and every 0 below it
 *   x&(x-1) = 0101 0000   <- lowest 1 gone, everything above unchanged
 */
static int popcount_kernighan(uint32_t x)
{
    int c = 0;
    while (x) {
        x &= x - 1u;
        c++;
    }
    return c;
}

/* SWAR ("SIMD within a register"): O(1), 12 operations, no loop, no table.
 * Adds bits in parallel: pairs, then nibbles, then bytes, then sums bytes.
 *
 *   step 1: each 2-bit group holds the count of its 2 bits      (0..2)
 *   step 2: each 4-bit group holds the count of its 4 bits      (0..4)
 *   step 3: each byte holds the count of its 8 bits             (0..8)
 *   step 4: multiply by 0x01010101 sums all bytes into the top byte
 * This is what __builtin_popcount compiles to when there is no POPCNT/CNT
 * instruction (and what the kernel's hweight32() does). */
static int popcount_swar(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    return (int)((x * 0x01010101u) >> 24);
}

/* 256-entry table: one lookup per byte. Fast, but the table costs a cache line
 * or four; on modern CPUs the builtin wins. */
static uint8_t g_table[256];
static void table_init(void)
{
    for (int i = 1; i < 256; i++)
        g_table[i] = (uint8_t)((i & 1) + g_table[i / 2]); /* DP: pop(i) = lsb + pop(i/2) */
}
static int popcount_table(uint32_t x)
{
    return g_table[x & 0xFF] + g_table[(x >> 8) & 0xFF] +
           g_table[(x >> 16) & 0xFF] + g_table[x >> 24];
}

/* Power of two <=> exactly one bit set. The "x &&" excludes zero, which
 * otherwise passes (0 & -1 == 0). The most common bug in this test. */
static bool is_pow2(uint32_t x)
{
    return x && !(x & (x - 1u));
}

/* Isolate lowest set bit: two's complement makes -x = ~x + 1, which flips all
 * bits above the lowest 1 and keeps it (and the zeros below it) the same.
 *
 *   x  = 0101 1000
 *  -x  = 1010 1000
 *  x&-x= 0000 1000
 *
 * Fenwick trees (lesson 12) use exactly this to jump between nodes. */
static uint32_t lowest_set_bit(uint32_t x)
{
    return x & (0u - x);
}

/* Index of highest set bit (floor(log2 x)), x != 0.
 * __builtin_clz(0) is UNDEFINED, so the caller must guard. */
static int ilog2(uint32_t x)
{
    return 31 - __builtin_clz(x);
}

/* Smallest power of two >= x (x in 1..2^31). Used to size hash tables and
 * ring buffers so "% size" becomes "& (size-1)".
 * Smear the top bit into every lower bit, then add 1:
 *   x-1 = 0001 0010 0000
 *   smear->0001 1111 1111
 *   +1  = 0010 0000 0000  */
static uint32_t round_up_pow2(uint32_t x)
{
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    return x + 1u;
}

/* Tiny deterministic PRNG (xorshift32) so runs are reproducible. */
static uint32_t g_rng = 2463534242u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

int main(void)
{
    table_init();

    puts("== 1. Kernighan trace on x = 0x58 (0101 1000) ==");
    uint32_t t = 0x58;
    int step = 0;
    while (t) {
        uint32_t next = t & (t - 1u);
        printf("  step %d: x=0x%02X  x&(x-1)=0x%02X\n", ++step, (unsigned)t, (unsigned)next);
        t = next;
    }
    assert(step == 3);

    puts("\n== 2. Four popcounts + the builtin agree on 200000 random values ==");
    for (int i = 0; i < 200000; i++) {
        uint32_t x = rnd();
        if (i < 3)
            x = (i == 0) ? 0u : (i == 1 ? 0xFFFFFFFFu : 0x80000000u); /* edge cases */
        int ref = popcount_naive(x);
        assert(popcount_kernighan(x) == ref);
        assert(popcount_swar(x) == ref);
        assert(popcount_table(x) == ref);
        assert(__builtin_popcount(x) == ref);
    }
    assert(__builtin_popcountll(0xFFFFFFFFFFFFFFFFull) == 64);
    puts("naive == kernighan == swar == table == __builtin_popcount: OK");

    puts("\n== 3. Power of two ==");
    for (uint32_t x = 0; x < 100000; x++)
        assert(is_pow2(x) == (popcount_naive(x) == 1));
    for (int k = 0; k < 32; k++)
        assert(is_pow2(1u << k));
    assert(!is_pow2(0));
    puts("is_pow2(x) == (popcount == 1) for 0..99999 and every 2^k: OK");

    puts("\n== 4. Lowest set bit, ctz, ilog2 ==");
    printf("lowest_set_bit(0x58) = 0x%02X, ctz = %d, ilog2 = %d\n",
           (unsigned)lowest_set_bit(0x58), __builtin_ctz(0x58), ilog2(0x58));
    assert(lowest_set_bit(0x58) == 0x08 && __builtin_ctz(0x58) == 3 && ilog2(0x58) == 6);
    for (int i = 0; i < 100000; i++) {
        uint32_t x = rnd() | 1u << (i % 32);   /* never zero */
        int lo = 0, hi = 31;
        while (!((x >> lo) & 1u))
            lo++;
        while (!((x >> hi) & 1u))
            hi--;
        assert(lowest_set_bit(x) == (1u << lo));
        assert(__builtin_ctz(x) == lo);
        assert(ilog2(x) == hi);
        /* x & (x-1) clears exactly that bit */
        assert((x & (x - 1u)) == (x ^ lowest_set_bit(x)));
    }
    puts("x&-x, ctz, 31-clz checked on 100000 values: OK");

    puts("\n== 5. Round up to power of two ==");
    printf("round_up_pow2(1000) = %u, (1024) = %u, (1025) = %u\n",
           (unsigned)round_up_pow2(1000), (unsigned)round_up_pow2(1024), (unsigned)round_up_pow2(1025));
    for (uint32_t x = 1; x < 200000; x++) {
        uint32_t p = 1;
        while (p < x)
            p <<= 1;
        assert(round_up_pow2(x) == p);
    }
    assert(round_up_pow2(0x80000000u) == 0x80000000u);
    /* the payoff: modulo by a power of two is a mask */
    for (uint32_t x = 0; x < 10000; x++)
        assert(x % 1024u == (x & 1023u));
    puts("round_up_pow2 vs loop for 1..199999, and x % 2^k == x & (2^k-1): OK");

    puts("\nall checks passed");
    return 0;
}
