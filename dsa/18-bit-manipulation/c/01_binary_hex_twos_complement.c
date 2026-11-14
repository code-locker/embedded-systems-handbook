/*
 * 01_binary_hex_twos_complement.c — binary, hex and two's complement by hand
 *
 * Concept : A byte is just 8 switches. Hex is a shorthand where one hex digit
 *           = exactly 4 bits (a "nibble"). Negative integers are stored in
 *           two's complement: -x == ~x + 1, so the same adder hardware works
 *           for signed and unsigned numbers.
 * Build   : make run P=dsa/18-bit-manipulation/c/01_binary_hex_twos_complement.c
 * Read    : ../README.md  (section "3.1 Binary, hex, two's complement")
 */
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Write the `bits` lowest bits of v into buf as '0'/'1', MSB first, with a
 * space every 4 bits so nibbles (= hex digits) are visible.
 * buf must hold bits + bits/4 + 1 chars. Caller owns buf. O(bits). */
static void to_binary(uint32_t v, int bits, char *buf)
{
    int k = 0;
    for (int i = bits - 1; i >= 0; i--) {
        buf[k++] = ((v >> i) & 1u) ? '1' : '0';
        if (i % 4 == 0 && i != 0)
            buf[k++] = ' ';
    }
    buf[k] = '\0';
}

/* Parse a string of '0'/'1' (spaces ignored) back into a number. */
static uint32_t from_binary(const char *s)
{
    uint32_t v = 0;
    for (; *s; s++) {
        if (*s == ' ')
            continue;
        v = (v << 1) | (uint32_t)(*s - '0'); /* shift left = multiply by 2 */
    }
    return v;
}

/* Two's complement negate by hand: invert every bit, then add one.
 * Done on uint8_t so there is no signed overflow anywhere (well defined). */
static uint8_t negate8(uint8_t x)
{
    return (uint8_t)(~x + 1u);
}

/* Interpret an 8-bit pattern as signed WITHOUT relying on implementation
 * defined conversion: if the top bit (weight -128) is set, subtract 256. */
static int as_signed8(uint8_t x)
{
    return (x & 0x80u) ? (int)x - 256 : (int)x;
}

int main(void)
{
    char buf[64];

    puts("== 1. Binary <-> hex: one hex digit is one nibble (4 bits) ==");
    /*
     *   0xB5 =  1011 0101
     *            B    5        B = 8+2+1 = 11, 5 = 4+1
     */
    to_binary(0xB5, 8, buf);
    printf("0xB5 = %s = %u\n", buf, 0xB5u);
    assert(strcmp(buf, "1011 0101") == 0);
    assert(from_binary("1011 0101") == 0xB5u);
    assert(0xB5 == 181);

    /* Round-trip every 16-bit value through text: proves to/from agree. */
    for (uint32_t v = 0; v <= 0xFFFFu; v++) {
        to_binary(v, 16, buf);
        assert(from_binary(buf) == v);
    }
    puts("round-trip binary text for all 65536 16-bit values: OK");

    puts("\n== 2. Two's complement: -x == ~x + 1 ==");
    /*
     *    5  = 0000 0101
     *   ~5  = 1111 1010      (flip every bit)
     *   +1  = 1111 1011  = 0xFB = 251 unsigned = -5 signed
     *
     * Bit weights of an int8_t:  -128 64 32 16 8 4 2 1
     *   1111 1011 = -128 + 64+32+16+8+2+1 = -5
     */
    uint8_t five = 5, minus5 = negate8(five);
    to_binary(minus5, 8, buf);
    printf("-5 as 8 bits = %s = 0x%02X (unsigned %u, signed %d)\n",
           buf, minus5, minus5, as_signed8(minus5));
    assert(minus5 == 0xFB);
    assert(as_signed8(minus5) == -5);

    /* Check the identity for every 8-bit value against the compiler. */
    for (int v = -128; v <= 127; v++) {
        uint8_t pattern = (uint8_t)v;          /* conversion to unsigned is defined: mod 256 */
        assert(as_signed8(pattern) == v);
        if (v != -128)                         /* -(-128) does not fit in 8 bits! */
            assert(as_signed8(negate8(pattern)) == -v);
    }
    puts("~x+1 == -x checked for all int8 values (except -128, see below)");

    puts("\n== 3. The asymmetric range and the one value with no negative ==");
    /* 8 bits: -128 .. +127. There is one more negative than positive,
     * because zero takes a slot on the "positive" side. */
    uint8_t m128 = 0x80;
    printf("-(-128) in 8 bits = %d  (it wraps back to itself)\n", as_signed8(negate8(m128)));
    assert(negate8(m128) == 0x80);
    printf("INT_MIN = %d, INT_MAX = %d  -> -INT_MIN overflows (UB for int!)\n", INT_MIN, INT_MAX);
    /* abs(INT_MIN) is undefined behaviour; UBSan would flag it. Use unsigned: */
    unsigned mag = 0u - (unsigned)INT_MIN;     /* well defined: unsigned wraps */
    printf("magnitude of INT_MIN computed in unsigned = %u\n", mag);
    assert(mag == 2147483648u);

    puts("\n== 4. Same bits, different meaning ==");
    /* 0xFF is 255 as uint8_t and -1 as int8_t: the bits do not know their type. */
    uint8_t ff = 0xFF;
    printf("0xFF: unsigned %u, signed %d\n", ff, as_signed8(ff));
    assert(as_signed8(ff) == -1);
    /* -1 in any width is all ones: handy for building masks. */
    assert((uint32_t)-1 == 0xFFFFFFFFu);
    assert((uint64_t)-1 == UINT64_MAX);

    puts("\n== 5. Sign extension: widening a signed value copies the sign bit ==");
    /*
     *   int8_t  -5 =           1111 1011
     *   int16_t -5 = 1111 1111 1111 1011   <- top byte filled with copies of bit 7
     *   uint8 251 -> uint16 = 0000 0000 1111 1011  <- zero extension
     */
    int8_t s8 = -5;
    int16_t s16 = s8;
    uint16_t z16 = (uint8_t)s8;
    printf("sign-extended 0x%04X, zero-extended 0x%04X\n", (unsigned)(uint16_t)s16, (unsigned)z16);
    assert((uint16_t)s16 == 0xFFFBu);
    assert(z16 == 0x00FBu);

    /* Manual sign extension of an N-bit field (common for 12-bit ADC values,
     * 24-bit audio samples): xor with the sign bit then subtract it. */
    uint32_t raw12 = 0xFFE;                   /* 12-bit two's complement for -2 */
    int32_t val12 = (int32_t)((raw12 ^ 0x800u)) - 0x800;
    printf("12-bit raw 0x%03X -> %" PRId32 "\n", (unsigned)raw12, val12);
    assert(val12 == -2);
    for (int v = -2048; v <= 2047; v++) {
        uint32_t r = (uint32_t)v & 0xFFFu;
        assert((int32_t)(r ^ 0x800u) - 0x800 == v);
    }
    puts("12-bit sign extension trick checked for all 4096 values");

    puts("\nall checks passed");
    return 0;
}
