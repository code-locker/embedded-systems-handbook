/*
 * 04_undefined_behaviour.c — the catalogue of UB, and the safe version of each
 *
 * Concept : Undefined behaviour (UB) means the standard places NO requirement
 *           on the program. The optimiser assumes UB never happens and uses
 *           that to delete checks. This file only executes the SAFE versions;
 *           each UB version is shown in a comment so you can recognise it.
 *           To watch UBSan catch real UB see dsa/03-debugging/cpp/04_ubsan_bug.cpp.
 * Build   : make run P=dsa/00-foundations/c/04_undefined_behaviour.c
 * Read    : ../README.md  (section "Undefined behaviour")
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* UB #1: signed overflow.
 *   int f(int x) { return x + 1 > x; }
 * gcc -O2 compiles this to "return 1": it may assume x + 1 never overflows. */
static int next_is_bigger_safe(int x)
{
    return x != INT_MAX;          /* the honest version of the question */
}

/* UB #2: out-of-bounds access.
 *   int a[4]; a[4] = 0;        // writes past the end: may clobber anything
 * Safe: check the index against the element count.                          */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
static int get_or_default(const int *a, size_t len, size_t idx, int dflt)
{
    return idx < len ? a[idx] : dflt;
}

/* UB #3: strict aliasing / type punning through a cast pointer.
 *   float f = 1.0f; uint32_t bits = *(uint32_t *)&f;   // UB
 * Safe: memcpy (compilers turn it into a single register move).            */
static uint32_t float_bits(float f)
{
    uint32_t bits;
    _Static_assert(sizeof bits == sizeof f, "float must be 32-bit");
    memcpy(&bits, &f, sizeof bits);
    return bits;
}

/* UB #4: shifting by >= the width, or shifting a negative value left.
 *   uint32_t x = 1u << 32;    // UB (x86 actually computes 1 << 0 = 1!)     */
static uint32_t shl_safe(uint32_t v, unsigned n)
{
    return n < 32 ? v << n : 0;
}

/* UB #5: modifying a variable twice without a sequence point.
 *   i = i++ + 1;   a[i] = i++;   printf("%d %d", i++, i++);
 * Safe: one side effect per statement.                                      */

/* UB #6: reading an uninitialised automatic variable, using a pointer after
 * free, dereferencing NULL, returning the address of a local. All covered
 * with live demos in dsa/03-debugging.                                       */

/* UB #7: division by zero, and INT_MIN / -1 (the result does not fit). */
static int safe_div(int a, int b, int *out)
{
    if (b == 0 || (a == INT_MIN && b == -1))
        return 0;
    *out = a / b;
    return 1;
}

int main(void)
{
    assert(next_is_bigger_safe(5) && !next_is_bigger_safe(INT_MAX));

    int a[4] = {10, 20, 30, 40};
    assert(get_or_default(a, ARRAY_LEN(a), 3, -1) == 40);
    assert(get_or_default(a, ARRAY_LEN(a), 4, -1) == -1);   /* not a[4]! */

    uint32_t bits = float_bits(1.0f);
    printf("bits of 1.0f = 0x%08X (sign 0, exponent 127, mantissa 0)\n", bits);
    assert(bits == 0x3F800000u);

    assert(shl_safe(1u, 31) == 0x80000000u);
    assert(shl_safe(1u, 32) == 0);

    int q = 0;
    assert(safe_div(7, 2, &q) && q == 3);
    assert(!safe_div(1, 0, &q));
    assert(!safe_div(INT_MIN, -1, &q));

    /* Implementation-defined is NOT undefined: the compiler must document it.
     * Right-shifting a negative number is implementation-defined (gcc: arithmetic). */
    int neg = -8;
    printf("-8 >> 1 = %d (implementation-defined; gcc does an arithmetic shift)\n", neg >> 1);

    /* Unspecified: order of evaluation of function arguments. f(g(), h())
     * may call h first. Legal, but do not depend on it.                       */

    printf("All safe versions behave. Read the comments for the UB versions.\n");
    return 0;
}
