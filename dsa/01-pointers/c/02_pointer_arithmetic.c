/*
 * 02_pointer_arithmetic.c — why p + 1 moves by sizeof(*p) bytes
 *
 * Concept : Pointer arithmetic counts in ELEMENTS, not bytes. For T *p,
 *           p + n is the address  (char*)p + n * sizeof(T). Subtracting two
 *           pointers into the same array gives the number of elements between
 *           them (type ptrdiff_t). a[i] is defined as *(a + i).
 * Build   : make run P=dsa/01-pointers/c/02_pointer_arithmetic.c
 * Read    : ../README.md  (section "3.2 Pointer arithmetic")
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    int32_t a[5] = {10, 20, 30, 40, 50};
    int32_t *p = a;                 /* a decays to &a[0] */

    /*
     *  index:      0        1        2        3        4       (5 = one past the end:
     *          +--------+--------+--------+--------+--------+   may be POINTED TO,
     *  a:      |   10   |   20   |   30   |   40   |   50   |   never DEREFERENCED)
     *          +--------+--------+--------+--------+--------+
     *  addr:   base     base+4   base+8   base+12  base+16  base+20
     *          ^p       ^p+1     ^p+2                       ^p+5 == a+5
     */
    printf("element  address           byte offset from a\n");
    for (int i = 0; i <= 5; i++) {
        int32_t *q = p + i;
        ptrdiff_t bytes = (char *)q - (char *)a;
        printf("  p+%d    %p   %td%s\n", i, (void *)q, bytes, i == 5 ? "   (one past end)" : "");
        assert(bytes == i * (ptrdiff_t)sizeof(int32_t));
    }

    /* a[i] == *(a + i) == *(i + a) == i[a]  — subscripting IS pointer arithmetic. */
    assert(a[2] == *(a + 2));
    assert(2[a] == 30);             /* legal and silly; proves the definition */

    /* Same +1, different step, because the TYPE differs. */
    double  dd[2];
    char    cc[2];
    int64_t ll[2];
    printf("\n+1 step: char %td, int32 %td, double %td, int64 %td bytes\n",
           (char *)(cc + 1) - (char *)cc, (char *)(a + 1) - (char *)a,
           (char *)(dd + 1) - (char *)dd, (char *)(ll + 1) - (char *)ll);

    /* Pointer difference = elements between, not bytes. */
    int32_t *first = &a[1], *last = &a[4];
    ptrdiff_t n = last - first;
    printf("&a[4] - &a[1] = %td elements (%td bytes)\n", n, (char *)last - (char *)first);
    assert(n == 3);

    /* Walking an array with a pointer: the idiom of strlen, memcpy, DMA loops. */
    int32_t sum = 0;
    for (const int32_t *it = a, *end = a + 5; it != end; ++it)   /* end = one past */
        sum += *it;
    assert(sum == 150);

    /* *p++ : precedence is *(p++) — read current, then advance the POINTER. */
    p = a;
    int32_t v1 = *p++;     /* v1 = 10, p -> a[1] */
    int32_t v2 = (*p)++;   /* v2 = 20, a[1] becomes 21, p unchanged */
    int32_t v3 = *++p;     /* p -> a[2], v3 = 30 */
    printf("*p++ = %d, (*p)++ = %d (a[1] now %d), *++p = %d\n", v1, v2, a[1], v3);
    assert(v1 == 10 && v2 == 20 && a[1] == 21 && v3 == 30 && p == &a[2]);

    /* Comparisons < > are only defined within the same array (or one past). */
    assert(&a[0] < &a[4]);

    /* Byte-wise walking: cast to unsigned char* (e.g. checksums over a struct). */
    uint32_t word = 0x01020304u;
    unsigned csum = 0;
    for (const unsigned char *b = (const unsigned char *)&word; b < (const unsigned char *)(&word + 1); b++)
        csum += *b;
    assert(csum == 1 + 2 + 3 + 4);
    printf("byte checksum of 0x01020304 = %u (&word + 1 = end of the 4 bytes)\n", csum);

    /* Arithmetic on void* is a GNU extension (step 1); -Wpedantic warns. Use char*. */

    printf("\nAll checks passed.\n");
    return 0;
}
