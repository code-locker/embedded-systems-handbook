/*
 * 05_const_pointers.c — const int *p vs int *const p vs const int *const p
 *
 * Concept : Read the declaration RIGHT TO LEFT, starting at the name:
 *             const int *p        "p is a pointer to an int that is const"
 *             int *const p        "p is a const pointer to an int"
 *             const int *const p  "p is a const pointer to a const int"
 *           const to the LEFT of * protects the DATA; to the RIGHT protects the POINTER.
 * Build   : make run P=dsa/01-pointers/c/05_const_pointers.c
 * Read    : ../README.md  (section "3.5 const and pointers")
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* API design: const in the parameter is a PROMISE to the caller.
 * memcpy(void *dst, const void *src, size_t n) tells you which side is written. */
static size_t count_char(const char *s, char c)
{
    size_t n = 0;
    for (; *s; s++)        /* moving s is fine: the POINTER is not const */
        n += (*s == c);    /* *s = 'x'; would be a compile error */
    return n;
}

int main(void)
{
    int a = 1, b = 2;

    /* 1. pointer to const: data read-only THROUGH p, p can move. */
    const int *pc = &a;
    /* *pc = 5;  error: assignment of read-only location '*pc' */
    pc = &b;            /* OK */
    assert(*pc == 2);
    a = 10;             /* a itself is not const: other names may change it */

    /* 2. const pointer: p fixed, data writable. Like an MMIO base address. */
    int *const cp = &a;
    *cp = 11;           /* OK */
    /* cp = &b;  error: assignment of read-only variable 'cp' */
    assert(a == 11);

    /* 3. const pointer to const: nothing changes through it. */
    const int *const ccp = &b;
    /* *ccp = 0; ccp = &a;  both errors */
    assert(*ccp == 2);

    /*
     *              can change *p ?   can change p ?
     *  int *p            yes              yes
     *  const int *p      no               yes      ("int const *p" is the same)
     *  int *const p      yes              no
     *  const int *const  no               no
     */

    /* Conversion rules: adding const is implicit; removing it needs a cast. */
    int *plain = &a;
    const int *view = plain;          /* OK: promise more */
    /* int *again = view;  warning: discards 'const' qualifier */
    (void)view;

    /* The char** trap: char** does NOT convert to const char** implicitly.
     * If it did, you could sneak a pointer to read-only data into a char*.
     * const char *const * is safe and allowed.                           */

    /* Writing to a string literal through a cast is UB (it lives in .rodata). */
    const char *msg = "pointer";
    assert(count_char(msg, 'o') == 1);
    char buf[16];
    strcpy(buf, msg);      /* writable copy */
    buf[0] = 'P';
    assert(strcmp(buf, "Pointer") == 0);

    printf("const int *p, int *const p, const int *const p: all checks passed.\n");
    return 0;
}
