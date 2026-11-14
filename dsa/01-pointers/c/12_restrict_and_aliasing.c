/*
 * 12_restrict_and_aliasing.c — when two pointers may point to the same memory
 *
 * Concept : ALIASING = two names for the same bytes. If the compiler cannot
 *           prove dst and src do not overlap, it must reload memory after
 *           every store. 'restrict' is YOUR promise that they don't overlap,
 *           so it can keep values in registers and vectorise.
 *           STRICT ALIASING: the compiler may assume an int* and a float*
 *           never point to the same object (only char/unsigned char may
 *           alias everything). Breaking it is UB; use memcpy or a union.
 * Build   : make run P=dsa/01-pointers/c/12_restrict_and_aliasing.c
 * Read    : ../README.md  (section "3.12 restrict and strict aliasing")
 *
 * See the codegen difference:
 *   gcc -O2 -S dsa/01-pointers/c/12_restrict_and_aliasing.c -o - | less
 *   look at add_twice vs add_twice_restrict: the restrict one loads *src once.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Without restrict: if dst == src, the second line must see the first write. */
static void add_twice(int *dst, const int *src)
{
    *dst += *src;
    *dst += *src;       /* compiler must RE-READ *src: dst may alias it */
}

/* With restrict: promise that dst and src never overlap. */
static void add_twice_restrict(int *restrict dst, const int *restrict src)
{
    *dst += *src;
    *dst += *src;       /* compiler may reuse the first load: dst += 2 * src */
}

/* memcpy has restrict parameters (overlap is UB); memmove does not (handles overlap). */

static uint32_t float_bits_memcpy(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);       /* the blessed way: no aliasing issue */
    return u;
}

static uint32_t float_bits_union(float f)
{
    union { float f; uint32_t u; } pun = {.f = f};   /* allowed in C (not in C++) */
    return pun.u;
}

int main(void)
{
    int a = 1, b = 10;
    add_twice(&a, &b);
    assert(a == 21);

    /* Aliased call: dst and src are THE SAME object.
     *   1st: a = 5 + 5 = 10;  2nd: a = 10 + 10 = 20  (re-read sees 10)   */
    a = 5;
    add_twice(&a, &a);
    printf("add_twice(&a, &a) with a=5 -> %d (re-reads after each store)\n", a);
    assert(a == 20);

    /* Calling add_twice_restrict(&a, &a) would break the promise: UB.
     * At -O2 it typically yields 15 (5 + 2*5) instead of 20. Not run here. */
    a = 5; b = 5;
    add_twice_restrict(&a, &b);
    assert(a == 15);

    /* Overlapping copy: memmove is correct, memcpy would be UB. */
    char buf[] = "abcdef";
    memmove(buf + 2, buf, 4);           /* "ababcd" */
    assert(strcmp(buf, "ababcd") == 0);

    /* Type punning */
    assert(float_bits_memcpy(1.0f) == 0x3F800000u);
    assert(float_bits_union(-2.0f) == 0xC0000000u);
    /* BAD: *(uint32_t *)&f  — UB under strict aliasing. gcc -O2 -Wstrict-aliasing
     * warns, and the Linux kernel builds with -fno-strict-aliasing because so
     * much old code does this.                                                */

    /* unsigned char* may inspect ANY object's bytes: always legal. */
    double d = 1.0;
    const unsigned char *bytes = (const unsigned char *)&d;
    assert(bytes[7] == 0x3F);           /* little-endian: sign+exponent in the top byte */

    printf("restrict and aliasing: all checks passed.\n");
    return 0;
}
