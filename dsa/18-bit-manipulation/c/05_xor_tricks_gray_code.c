/*
 * 05_xor_tricks_gray_code.c — XOR swap, find the single number, missing number,
 *                             parity, Gray code encode/decode
 *
 * Concept : XOR is "add without carry" (addition mod 2 per bit). Its laws:
 *              a ^ a = 0,  a ^ 0 = a,  commutative, associative.
 *           So XOR-ing a list cancels every value that appears an even number
 *           of times. Gray code n ^ (n >> 1) makes consecutive numbers differ
 *           in exactly ONE bit — used by rotary encoders and async FIFO
 *           pointers crossing clock domains.
 * Build   : make run P=dsa/18-bit-manipulation/c/05_xor_tricks_gray_code.c
 * Read    : ../README.md  (section "3.6 XOR tricks and Gray code")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* XOR swap. Trace with a=0b1100, b=0b1010:
 *   a ^= b  -> a = 0110 (a^b),   b = 1010
 *   b ^= a  -> b = 1100 (b^a^b = a)
 *   a ^= b  -> a = 1010 (a^b^a = b)
 * PITFALL: if x and y point to the SAME object, step 1 makes it 0 and the
 * value is lost. Hence the guard. In real code use a temporary: it is just as
 * fast (register rename) and clearer. This is a puzzle, not an optimisation. */
static void xor_swap(unsigned *x, unsigned *y)
{
    if (x == y)
        return;
    *x ^= *y;
    *y ^= *x;
    *x ^= *y;
}

/* Every value appears twice except one: XOR them all. O(n) time, O(1) space. */
static int single_number(const int *a, int n)
{
    int r = 0;
    for (int i = 0; i < n; i++)
        r ^= a[i];
    return r;
}

/* Two values appear once, all others twice. XOR gives s = p ^ q (non-zero).
 * Any set bit of s is a bit where p and q differ: split the array by that bit
 * and each half contains exactly one of them. */
static void two_single_numbers(const int *a, int n, int *p, int *q)
{
    unsigned s = 0;
    for (int i = 0; i < n; i++)
        s ^= (unsigned)a[i];
    unsigned diff = s & (0u - s);          /* lowest bit where p and q differ */
    unsigned x = 0, y = 0;
    for (int i = 0; i < n; i++) {
        if ((unsigned)a[i] & diff)
            x ^= (unsigned)a[i];
        else
            y ^= (unsigned)a[i];
    }
    *p = (int)(x < y ? x : y);
    *q = (int)(x < y ? y : x);
}

/* 0..n with one number missing (array length n): XOR of 0..n ^ XOR of array. */
static int missing_number(const int *a, int n)
{
    int r = n;
    for (int i = 0; i < n; i++)
        r ^= i ^ a[i];
    return r;
}

/* Parity (1 if odd number of set bits): fold the word onto itself.
 * Used for UART parity bits, ECC, RAID-5 (data ^ data ^ ... = parity disk). */
static unsigned parity32(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return x & 1u;
}

static uint32_t to_gray(uint32_t n)
{
    return n ^ (n >> 1);
}

/* Decode: each binary bit = XOR of all gray bits at and above it (prefix XOR
 * from the MSB). Done in log2(32) = 5 steps, like parity32. */
static uint32_t from_gray(uint32_t g)
{
    g ^= g >> 16;
    g ^= g >> 8;
    g ^= g >> 4;
    g ^= g >> 2;
    g ^= g >> 1;
    return g;
}

static void print_bits(uint32_t v, int bits)
{
    for (int i = bits - 1; i >= 0; i--)
        putchar((v >> i) & 1u ? '1' : '0');
}

int main(void)
{
    puts("== 1. XOR swap (and why it needs an alias guard) ==");
    unsigned a = 12, b = 10;
    xor_swap(&a, &b);
    printf("after swap: a=%u b=%u\n", a, b);
    assert(a == 10 && b == 12);
    xor_swap(&a, &a);                       /* same object: guarded */
    assert(a == 10);

    puts("\n== 2. Single number (all others appear twice) ==");
    int arr[] = { 4, 1, 2, 1, 2, 9, 4 };
    int n = (int)(sizeof arr / sizeof arr[0]);
    printf("single_number({4,1,2,1,2,9,4}) = %d\n", single_number(arr, n));
    assert(single_number(arr, n) == 9);

    int arr2[] = { 3, 5, 7, 3, 8, 5 };
    int p, q;
    two_single_numbers(arr2, 6, &p, &q);
    printf("two singles in {3,5,7,3,8,5} = %d, %d\n", p, q);
    assert(p == 7 && q == 8);

    int arr3[] = { 3, 0, 1, 5, 2 };          /* 0..5 missing 4 */
    printf("missing from {3,0,1,5,2} = %d\n", missing_number(arr3, 5));
    assert(missing_number(arr3, 5) == 4);

    puts("\n== 3. Parity ==");
    for (uint32_t x = 0; x < 300000; x++)
        assert(parity32(x * 2654435761u) == ((unsigned)__builtin_popcount(x * 2654435761u) & 1u));
    printf("parity(0b1011) = %u\n", parity32(0xB));
    puts("parity32 == popcount & 1 on 300000 values: OK");

    puts("\n== 4. Gray code: neighbours differ in exactly one bit ==");
    /*
     *   n  binary  gray     n  binary  gray
     *   0  000     000      4  100     110
     *   1  001     001      5  101     111
     *   2  010     011      6  110     101
     *   3  011     010      7  111     100
     * A 3-bit binary counter going 3->4 flips ALL three bits; if a reader
     * samples mid-change it may see any of 8 values. Gray flips one, so a
     * sample is always either the old or the new value.
     */
    for (uint32_t i = 0; i < 8; i++) {
        printf("  %u  ", (unsigned)i);
        print_bits(i, 3);
        printf("  ");
        print_bits(to_gray(i), 3);
        putchar('\n');
    }
    for (uint32_t i = 0; i < (1u << 20); i++) {
        assert(from_gray(to_gray(i)) == i);
        assert(__builtin_popcount(to_gray(i) ^ to_gray(i + 1)) == 1);
    }
    assert(__builtin_popcount(to_gray(0xFFFFFFFFu) ^ to_gray(0)) == 1); /* wraps too */
    puts("decode(encode(i)) == i and one-bit steps for 2^20 values: OK");

    puts("\nall checks passed");
    return 0;
}
