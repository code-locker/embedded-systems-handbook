/*
 * 02_shifts_and_masks.c — shifts (logical vs arithmetic, UB cases) and the
 *                         four single-bit operations: set, clear, toggle, test
 *
 * Concept : x << n multiplies by 2^n, x >> n divides by 2^n. Unsigned right
 *           shift fills with 0 (logical); signed right shift of a negative is
 *           implementation-defined (gcc: arithmetic, copies the sign bit).
 *           Shifting by >= the width, or a negative amount, or shifting a 1
 *           into/past the sign bit of a signed int is UNDEFINED behaviour.
 *           Masks: OR sets, AND-NOT clears, XOR toggles, AND tests.
 * Build   : make run P=dsa/18-bit-manipulation/c/02_shifts_and_masks.c
 * Read    : ../README.md  (sections "3.2 Shifts" and "3.3 Masks")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* The classic four. The "1u" (or 1ull for 64-bit) matters: plain `1 << 31`
 * shifts a 1 into the sign bit of an int -> undefined behaviour. */
#define BIT(n)            (1u << (n))
#define SET_BIT(x, n)     ((x) |  BIT(n))
#define CLEAR_BIT(x, n)   ((x) & ~BIT(n))
#define TOGGLE_BIT(x, n)  ((x) ^  BIT(n))
#define TEST_BIT(x, n)    (((x) >> (n)) & 1u)

/* GENMASK(h, l): bits h..l inclusive set, like the Linux kernel macro.
 * Built from all-ones so it works for h == 31 without shifting by 32:
 *   ~0u << l           = 1111..1 000   (clear bits below l)
 *   ~0u >> (31 - h)    = 000 1..1111   (clear bits above h)
 *   AND                = 000 1..1 000 */
#define GENMASK(h, l)     ((~0u << (l)) & (~0u >> (31 - (h))))

/* Safe shift: defines the result for any count (UB-free). Real code that may
 * receive a shift count from data (protocol parsers!) needs this guard. */
static uint32_t shl_safe(uint32_t x, unsigned n)
{
    return n >= 32 ? 0u : x << n;
}

/* Portable arithmetic right shift for int32_t: no reliance on
 * implementation-defined behaviour. For negative x, shift the bitwise NOT
 * (which is non-negative) and NOT the result back:  ~(~x >> n). */
static int32_t sar32(int32_t x, unsigned n)
{
    return x < 0 ? ~(~x >> n) : x >> n;
}

/* Rotate left: bits falling off the top come back at the bottom.
 * The "& 31" and "-n & 31" keep both shifts in range even for n == 0,
 * and gcc recognises this exact pattern and emits a single ROL instruction. */
static uint32_t rotl32(uint32_t x, unsigned n)
{
    n &= 31u;
    return (x << n) | (x >> ((32u - n) & 31u));
}

int main(void)
{
    puts("== 1. Shift = multiply / divide by a power of two ==");
    assert((13u << 3) == 13u * 8u);
    assert((200u >> 2) == 200u / 4u);
    printf("13 << 3 = %u, 200 >> 2 = %u\n", 13u << 3, 200u >> 2);

    puts("\n== 2. Logical vs arithmetic right shift ==");
    /*
     *   uint8 0xF0 >> 2  : 1111 0000 -> 0011 1100   (zeros come in: logical)
     *   int8  -16 >> 2   : 1111 0000 -> 1111 1100   (sign copies in: arithmetic) = -4
     */
    uint8_t u = 0xF0;
    int32_t s = -16;
    printf("unsigned 0xF0 >> 2 = 0x%02X (logical)\n", (unsigned)(uint8_t)(u >> 2));
    printf("signed   -16  >> 2 = %d   (gcc: arithmetic, impl-defined in C11)\n", (int)(s >> 2));
    assert((uint8_t)(u >> 2) == 0x3C);
    assert(sar32(-16, 2) == -4);
    /* Arithmetic shift rounds toward -infinity, division rounds toward 0! */
    printf("-7 >> 1 = %d but -7 / 2 = %d  (floor vs truncate)\n", (int)sar32(-7, 1), -7 / 2);
    assert(sar32(-7, 1) == -4 && -7 / 2 == -3);
    for (int32_t x = -1000; x <= 1000; x++)
        for (unsigned n = 0; n < 31; n++) {
            /* reference: floor division by 2^n done with integer math */
            int32_t d = (int32_t)(1u << n);
            int32_t q = x / d;
            if ((x % d != 0) && (x < 0))
                q -= 1;
            assert(sar32(x, n) == q);
        }
    puts("portable sar32 == floor(x / 2^n) for x in [-1000,1000], n in [0,30]");

    puts("\n== 3. Undefined-behaviour shift cases (do NOT write these) ==");
    puts("  x << 32   for 32-bit x      : UB (x86 masks count to 0 -> returns x!)");
    puts("  x << -1                     : UB");
    puts("  1 << 31   with int 1        : UB (shifts into sign bit) -> use 1u << 31");
    puts("  -1 << 1   (negative signed) : UB in C11 -> shift unsigned instead");
    puts("  uint8_t b; b << 24          : b promotes to int; 0xFF << 24 is UB!");
    /* The promotion trap, done right: cast to the wide unsigned type first. */
    uint8_t b = 0xFF;
    uint32_t w = (uint32_t)b << 24;
    assert(w == 0xFF000000u);
    assert(shl_safe(1u, 32) == 0u && shl_safe(1u, 31) == 0x80000000u);
    puts("  safe versions: (uint32_t)b << 24 and shl_safe(x, n>=32) -> 0");

    puts("\n== 4. set / clear / toggle / test ==");
    /*
     *  x          = 0000 1010   (0x0A)
     *  set bit 0  : x | 0000 0001 = 0000 1011
     *  clear bit 3: x & 1111 0111 = 0000 0010
     *  toggle 7   : x ^ 1000 0000 = 1000 1010
     *  test bit 1 : (x >> 1) & 1  = 1
     */
    uint32_t x = 0x0A;
    assert(SET_BIT(x, 0) == 0x0B);
    assert(CLEAR_BIT(x, 3) == 0x02);
    assert(TOGGLE_BIT(x, 7) == 0x8A);
    assert(TEST_BIT(x, 1) == 1 && TEST_BIT(x, 0) == 0);
    assert(TOGGLE_BIT(TOGGLE_BIT(x, 5), 5) == x);   /* toggle twice = identity */
    printf("x=0x%02X set0=0x%02X clr3=0x%02X tog7=0x%02X test1=%u\n",
           (unsigned)x, SET_BIT(x, 0), CLEAR_BIT(x, 3), TOGGLE_BIT(x, 7), TEST_BIT(x, 1));
    /* Exhaustive check against a bool-array model for all 32 bit positions. */
    for (unsigned n = 0; n < 32; n++) {
        uint32_t v = 0xA5A5A5A5u;
        bool before = (v >> n) & 1u;
        assert(TEST_BIT(SET_BIT(v, n), n) == 1);
        assert(TEST_BIT(CLEAR_BIT(v, n), n) == 0);
        assert(TEST_BIT(TOGGLE_BIT(v, n), n) == (unsigned)!before);
        /* and no OTHER bit changed: */
        assert((SET_BIT(v, n) & ~BIT(n)) == (v & ~BIT(n)));
        assert((CLEAR_BIT(v, n) & ~BIT(n)) == (v & ~BIT(n)));
    }
    puts("single-bit ops checked on all 32 positions (and no neighbour disturbed)");

    puts("\n== 5. Multi-bit masks (GENMASK) and field extract / insert ==");
    /*
     *   GENMASK(7,4) = 0000 0000 ... 1111 0000 = 0xF0
     *   extract field [7:4] of 0xABCD:  (0xABCD & 0xF0) >> 4 = 0xC
     *   insert 0x3 into [7:4]       :  (v & ~mask) | (0x3 << 4)
     */
    assert(GENMASK(7, 4) == 0xF0u);
    assert(GENMASK(31, 0) == 0xFFFFFFFFu);
    assert(GENMASK(31, 31) == 0x80000000u);
    assert(GENMASK(0, 0) == 1u);
    uint32_t v = 0xABCD;
    uint32_t field = (v & GENMASK(7, 4)) >> 4;
    uint32_t v2 = (v & ~GENMASK(7, 4)) | ((0x3u << 4) & GENMASK(7, 4));
    printf("0xABCD field[7:4] = 0x%X, after inserting 3 -> 0x%X\n", (unsigned)field, (unsigned)v2);
    assert(field == 0xC && v2 == 0xAB3D);
    for (unsigned h = 0; h < 32; h++)
        for (unsigned l = 0; l <= h; l++) {
            uint32_t ref = 0;
            for (unsigned i = l; i <= h; i++)
                ref |= BIT(i);
            assert(GENMASK(h, l) == ref);
        }
    puts("GENMASK(h,l) checked against a loop for all 528 (h,l) pairs");

    puts("\n== 6. Rotate (used in hashes, CRC, crypto) ==");
    assert(rotl32(0x80000001u, 1) == 0x00000003u);
    assert(rotl32(0x12345678u, 0) == 0x12345678u);
    assert(rotl32(0x12345678u, 8) == 0x34567812u);
    printf("rotl32(0x12345678, 8) = 0x%08X\n", (unsigned)rotl32(0x12345678u, 8));

    puts("\nall checks passed");
    return 0;
}
