/*
 * 08_alignment_padding.c — why sizeof(struct) is bigger than the sum of its fields
 *
 * Concept : Each type has an ALIGNMENT: its address must be a multiple of it
 *           (int: 4, double/pointer: 8 on LP64). The compiler inserts PADDING
 *           between fields so every field is aligned, and at the end so that
 *           arrays of the struct stay aligned. Order fields from largest to
 *           smallest alignment to minimise padding. __attribute__((packed))
 *           removes padding (for wire formats/registers) at a cost.
 * Build   : make run P=dsa/02-memory-management/c/08_alignment_padding.c
 * Read    : ../README.md  (section "3.8 Alignment and padding")
 * Tool    : pahole (from 'dwarves') shows holes:  pahole build/dsa/02-memory-management/c/08_alignment_padding
 */
#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Badly ordered: 1 + (7 pad) + 8 + 1 + (3 pad) + 4 + 1 + (7 pad) = 32 */
struct bad {
    char   a;      /* offset 0            */
    double b;      /* offset 8  (pad 7)   */
    char   c;      /* offset 16           */
    int    d;      /* offset 20 (pad 3)   */
    char   e;      /* offset 24, then 7 bytes tail padding -> 32 */
};

/* Same fields, largest first: 8 + 4 + 1 + 1 + 1 + (1 pad) = 16 */
struct good {
    double b;
    int    d;
    char   a, c, e;
};

/* Wire format / hardware register block: exact layout required. */
struct __attribute__((packed)) wire_hdr {
    uint8_t  version;
    uint32_t length;      /* offset 1: MISALIGNED on purpose */
    uint16_t crc;
};

/* Hardware-style register map: explicit reserved fields, no hidden padding. */
struct regs {
    uint32_t ctrl;        /* 0x00 */
    uint32_t status;      /* 0x04 */
    uint32_t reserved[2]; /* 0x08, 0x0C */
    uint64_t dma_addr;    /* 0x10 */
};
_Static_assert(offsetof(struct regs, dma_addr) == 0x10, "register map layout is ABI");

/* Avoid false sharing between CPU cores: put hot counters on separate 64-byte lines. */
struct per_cpu_counter { alignas(64) uint64_t value; };

#define SHOW(T, f) printf("    %-8s offset %2zu size %zu align %zu\n", #f, offsetof(T, f), sizeof(((T *)0)->f), alignof(__typeof__(((T *)0)->f)))

int main(void)
{
    printf("alignof: char %zu, short %zu, int %zu, long %zu, double %zu, void* %zu, max_align_t %zu\n",
           alignof(char), alignof(short), alignof(int), alignof(long), alignof(double),
           alignof(void *), alignof(max_align_t));

    printf("\nstruct bad  (sizeof %zu):\n", sizeof(struct bad));
    SHOW(struct bad, a); SHOW(struct bad, b); SHOW(struct bad, c); SHOW(struct bad, d); SHOW(struct bad, e);
    printf("struct good (sizeof %zu):\n", sizeof(struct good));
    SHOW(struct good, b); SHOW(struct good, d); SHOW(struct good, a); SHOW(struct good, c); SHOW(struct good, e);
    assert(sizeof(struct bad) == 32 && sizeof(struct good) == 16);
    assert(offsetof(struct bad, b) == 8 && offsetof(struct bad, d) == 20);

    /* An array of 1000 of each: the cost of bad ordering is real RAM/cache. */
    printf("1000 elements: bad = %zu bytes, good = %zu bytes\n", 1000 * sizeof(struct bad), 1000 * sizeof(struct good));

    /* Tail padding keeps arr[1] aligned: */
    struct bad arr[2];
    assert((uintptr_t)&arr[1].b % alignof(double) == 0);

    printf("\npacked wire_hdr: sizeof %zu (no padding), length at offset %zu\n",
           sizeof(struct wire_hdr), offsetof(struct wire_hdr, length));
    assert(sizeof(struct wire_hdr) == 7);

    /* Read a misaligned field portably: memcpy (the compiler emits the right
     * loads; on ARM, unaligned access to device memory faults).             */
    unsigned char raw[7] = {1, 0x10, 0, 0, 0, 0xCD, 0xAB};   /* little-endian length=16 */
    struct wire_hdr h;
    memcpy(&h, raw, sizeof h);
    uint32_t len;
    memcpy(&len, raw + offsetof(struct wire_hdr, length), sizeof len);
    assert(h.version == 1 && len == 16 && h.crc == 0xABCD);
    /* Taking &h.length gives a misaligned uint32_t*: gcc warns
     * -Waddress-of-packed-member. Dereferencing it is UB on strict CPUs. */

    printf("per_cpu_counter: sizeof %zu, align %zu (one cache line each)\n",
           sizeof(struct per_cpu_counter), alignof(struct per_cpu_counter));
    assert(sizeof(struct per_cpu_counter) == 64);

    /* Padding bytes are UNSPECIFIED: memcmp of structs can differ even if every
     * field is equal. memset first if you will memcmp/hash/send the struct.   */
    struct bad s1, s2;
    memset(&s1, 0, sizeof s1);
    memset(&s2, 0, sizeof s2);
    s1.a = s2.a = 'x'; s1.b = s2.b = 1.0; s1.c = s2.c = 'y'; s1.d = s2.d = 2; s1.e = s2.e = 'z';
    assert(memcmp(&s1, &s2, sizeof s1) == 0);

    printf("All checks passed.\n");
    return 0;
}
