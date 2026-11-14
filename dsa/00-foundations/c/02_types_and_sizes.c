/*
 * 02_types_and_sizes.c — how big each C type really is, and how to stop guessing
 *
 * Concept : C only guarantees MINIMUM sizes (char>=8, short>=16, int>=16,
 *           long>=32, long long>=64 bits). Real sizes depend on the ABI:
 *           x86_64 and aarch64 Linux are both "LP64" (long and pointers = 64 bit).
 *           For hardware registers and file formats use <stdint.h> exact types.
 * Build   : make run P=dsa/00-foundations/c/02_types_and_sizes.c
 * Read    : ../README.md  (section "Types and sizes")
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

/* Compile-time checks cost nothing at run time and fail the BUILD, not the
 * board. Use them for every assumption a driver or protocol relies on.     */
_Static_assert(CHAR_BIT == 8, "this code assumes 8-bit bytes");
_Static_assert(sizeof(uint32_t) == 4, "uint32_t must be exactly 4 bytes");

#define ROW(T) printf("  %-20s %2zu byte(s)  %3zu bits\n", #T, sizeof(T), sizeof(T) * CHAR_BIT)

int main(void)
{
    printf("Fundamental types on this machine:\n");
    ROW(char);
    ROW(short);
    ROW(int);
    ROW(long);
    ROW(long long);
    ROW(float);
    ROW(double);
    ROW(long double);
    ROW(_Bool);
    ROW(void *);
    ROW(size_t);
    ROW(ptrdiff_t);
    ROW(intptr_t);

    printf("\nExact-width types (<stdint.h>) — use these for registers & wire formats:\n");
    ROW(int8_t);
    ROW(uint16_t);
    ROW(uint32_t);
    ROW(uint64_t);

    printf("\nRanges (<limits.h>, <stdint.h>):\n");
    printf("  INT_MIN..INT_MAX       = %d .. %d\n", INT_MIN, INT_MAX);
    printf("  UINT_MAX               = %u\n", UINT_MAX);
    printf("  LLONG_MAX              = %lld\n", LLONG_MAX);
    printf("  UINT64_MAX             = %" PRIu64 "\n", UINT64_MAX);
    printf("  SIZE_MAX               = %zu\n", SIZE_MAX);
    printf("  char is %s here       (implementation-defined! x86: signed, ARM: unsigned)\n",
           CHAR_MIN < 0 ? "signed  " : "unsigned");

    /* The rules the standard DOES guarantee: */
    assert(sizeof(char) == 1);                  /* by definition: sizeof counts in chars */
    assert(sizeof(short) <= sizeof(int));
    assert(sizeof(int) <= sizeof(long));
    assert(sizeof(long) <= sizeof(long long));
    assert(sizeof(long long) * CHAR_BIT >= 64);

    /* sizeof is a compile-time operator (except for VLAs): the expression
     * inside is NOT evaluated. i stays 0.                                  */
    int i = 0;
    size_t n = sizeof(i++);
    assert(i == 0 && n == sizeof(int));
    printf("\n  sizeof(i++) = %zu and i is still %d (operand not evaluated)\n", n, i);

    /* Literal types: 'A' is an int in C (a char in C++!). */
    printf("  sizeof('A') = %zu in C (it is an int; in C++ it is 1)\n", sizeof('A'));
    assert(sizeof('A') == sizeof(int));

    /* Printing fixed-width types portably needs <inttypes.h> macros. */
    uint32_t reg = 0xDEADBEEFu;
    printf("  uint32_t reg = 0x%08" PRIX32 "  (PRIX32 expands to the right length modifier)\n", reg);

    /* bool in C11 is _Bool; any non-zero value converts to 1. */
    bool flag = 42;
    assert(flag == 1);
    printf("  bool flag = 42 stores %d\n", flag);

    printf("\nAll checks passed.\n");
    return 0;
}
