/*
 * 03_integer_promotion_overflow.c — promotions, usual arithmetic conversions, wrap-around
 *
 * Concept : Before arithmetic, anything smaller than int is PROMOTED to int.
 *           When signed meets unsigned of the same rank, the signed one is
 *           CONVERTED to unsigned (-1 becomes UINT_MAX). Unsigned overflow
 *           wraps (defined); signed overflow is undefined behaviour.
 * Build   : make run P=dsa/00-foundations/c/03_integer_promotion_overflow.c
 * Read    : ../README.md  (section "Integer promotion, conversion and overflow")
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Safe signed add: returns false instead of invoking UB.
 * __builtin_add_overflow (gcc/clang) does the check with the CPU's overflow
 * flag — it is what the kernel's check_add_overflow() wraps.               */
static bool safe_add(int a, int b, int *out)
{
    return !__builtin_add_overflow(a, b, out);
}

/* Portable version without builtins: check BEFORE adding. */
static bool safe_add_portable(int a, int b, int *out)
{
    if ((b > 0 && a > INT_MAX - b) || (b < 0 && a < INT_MIN - b))
        return false;
    *out = a + b;
    return true;
}

int main(void)
{
    /* 1. Integer promotion: uint8_t + uint8_t is computed as int. ------------- */
    uint8_t a = 200, b = 100;
    int sum = a + b;               /* 300: no wrap, both were promoted to int */
    uint8_t stored = (uint8_t)(a + b); /* 300 mod 256 = 44: wrap happens on STORE */
    printf("uint8_t 200 + 100 = %d as int, %u when stored back in uint8_t\n", sum, stored);
    assert(sum == 300 && stored == 44);

    /* Promotion surprise with ~ : ~x on a uint8_t is an int with the high bits set. */
    uint8_t mask = 0x0F;
    printf("~(uint8_t)0x0F = 0x%X (int!), (uint8_t)~0x0F = 0x%X\n",
           (unsigned)~mask, (unsigned)(uint8_t)~mask);
    assert((uint8_t)~mask == 0xF0);
    int inverted = ~mask;            /* int 0xFFFFFFF0 = -16, not 0xF0 */
    assert(inverted == -16);

    /* 2. Signed vs unsigned comparison: -1 < 1u is FALSE. ------------------- */
    int neg = -1;
    unsigned one = 1u;
    /* -Wextra warns about this exact line (-Wsign-compare); we cast explicitly
     * to show what the compiler does implicitly.                             */
    bool surprising = (unsigned)neg < one;
    printf("-1 < 1u evaluates as %u < 1u -> %s\n", (unsigned)neg, surprising ? "true" : "false");
    assert(!surprising);

    /* Classic loop bug: for (unsigned i = n - 1; i >= 0; i--) never ends,
     * because an unsigned i is ALWAYS >= 0. Correct countdown idiom:        */
    unsigned count = 0;
    for (unsigned i = 5; i-- > 0;)   /* tests then decrements: visits 4..0 */
        count++;
    assert(count == 5);

    /* size_t minus size_t never goes negative: strlen(a) - strlen(b) < 0 is never true. */
    size_t la = 3, lb = 5;
    printf("(size_t)3 - (size_t)5 = %zu (wrapped, not -2)\n", la - lb);
    assert(la - lb == SIZE_MAX - 1);

    /* 3. Unsigned overflow wraps modulo 2^N — DEFINED, useful for counters. */
    uint32_t jiffies = UINT32_MAX - 1;
    uint32_t start = jiffies;
    jiffies += 5;                                 /* wraps to 3 */
    uint32_t elapsed = jiffies - start;           /* still 5! modular arithmetic */
    printf("wrapping timer: start=%u now=%u elapsed=%u\n", start, jiffies, elapsed);
    assert(elapsed == 5);   /* this is why the kernel's time_after() works across wrap */

    /* 4. Signed overflow is UB: we never execute INT_MAX + 1. We CHECK it. */
    int r = 0;
    assert(!safe_add(INT_MAX, 1, &r));
    assert(safe_add(40, 2, &r) && r == 42);
    assert(!safe_add_portable(INT_MIN, -1, &r));
    assert(safe_add_portable(-40, -2, &r) && r == -42);
    printf("safe_add(INT_MAX, 1) refused; safe_add(40, 2) = 42\n");

    /* 5. Multiplication overflow before widening: classic size bug. */
    int w = 70000, h = 70000;
    long long wrong_way_avoided = (long long)w * h;   /* widen FIRST, then multiply */
    printf("(long long)70000 * 70000 = %lld (w * h in int would be UB)\n", wrong_way_avoided);
    assert(wrong_way_avoided == 4900000000LL);

    /* 6. Shifts: 1 << 31 overflows int (UB before C++20). Use 1u << 31. */
    uint32_t bit31 = 1u << 31;
    assert(bit31 == 0x80000000u);
    printf("1u << 31 = 0x%08X (write 1u, not 1, for register bit masks)\n", bit31);

    /* 7. Division: integer division truncates toward zero (C99+). */
    assert(-7 / 2 == -3 && -7 % 2 == -1);
    printf("-7 / 2 = %d, -7 %% 2 = %d (truncates toward zero)\n", -7 / 2, -7 % 2);

    printf("\nAll checks passed.\n");
    return 0;
}
