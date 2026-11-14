/*
 * 01_address_and_deref.c — & gives an address, * follows one
 *
 * Concept : Every object lives in a numbered byte of memory. &x is that
 *           number (the address). A pointer is a variable that STORES an
 *           address. *p means "go to the address in p and use what is there".
 *           & and * are inverses: *&x is x, and &*p is p.
 * Build   : make run P=dsa/01-pointers/c/01_address_and_deref.c
 * Read    : ../README.md  (section "3.1 Addresses, & and *")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Print the raw bytes of any object, lowest address first. Shows endianness. */
static void dump_bytes(const char *label, const void *addr, size_t n)
{
    const unsigned char *b = addr;     /* unsigned char* may alias anything */
    printf("  %-8s @ %p :", label, addr);
    for (size_t i = 0; i < n; i++)
        printf(" %02x", b[i]);
    printf("\n");
}

/* The only way for a C function to change the caller's variable: receive its address. */
static void set_to_42(int *out)
{
    *out = 42;            /* write into the caller's box */
}

int main(void)
{
    int x = 0x11223344;
    int *p = &x;           /* p holds the address of x */

    /*
     *   name:   x                           p
     *         +------------+              +----------------+
     *  value: | 0x11223344 |   <───────── | 0x7ffc...a4    |
     *         +------------+              +----------------+
     *  addr:   0x7ffc...a4                 0x7ffc...a8
     *          (4 bytes)                   (8 bytes on LP64)
     */
    printf("x  = 0x%x, &x = %p, sizeof x = %zu\n", (unsigned)x, (void *)&x, sizeof x);
    printf("p  = %p, &p = %p, sizeof p = %zu\n", (void *)p, (void *)&p, sizeof p);
    printf("*p = 0x%x  (follow the arrow)\n", (unsigned)*p);
    assert(p == &x);
    assert(*p == x);
    assert(*&x == x && &*p == p);  /* & and * undo each other */

    /* Writing through the pointer writes x: there is only ONE box. */
    *p = 7;
    assert(x == 7);

    /* Little-endian (x86_64 and the TDA4VM A72): lowest byte at lowest address. */
    x = 0x11223344;
    printf("\nBytes of x in memory:\n");
    dump_bytes("x", &x, sizeof x);
    const unsigned char *first = (const unsigned char *)&x;
    printf("  first byte = 0x%02x -> %s-endian\n", *first, *first == 0x44 ? "little" : "big");

    /* A pointer is just a number: you can see it as an integer. */
    uintptr_t raw = (uintptr_t)p;
    printf("\n(uintptr_t)p = 0x%jx  (the address as an integer)\n", (uintmax_t)raw);
    assert((int *)raw == p);
    assert(raw % _Alignof(int) == 0);  /* ints are 4-byte aligned */

    /* Output parameter pattern. */
    int result = 0;
    set_to_42(&result);
    assert(result == 42);
    printf("set_to_42(&result) -> result = %d\n", result);

    /* Each type has its own pointer type; the type says how many bytes *p reads. */
    double d = 3.5;
    double *pd = &d;
    char c = 'Z';
    char *pc = &c;
    printf("\nsizeof(*pd) = %zu, sizeof(*pc) = %zu, but sizeof(pd) = sizeof(pc) = %zu\n",
           sizeof *pd, sizeof *pc, sizeof pd);
    assert(sizeof pd == sizeof pc);   /* all object pointers are 8 bytes here */
    assert(*pd == 3.5 && *pc == 'Z');

    printf("\nAll checks passed.\n");
    return 0;
}
