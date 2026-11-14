/*
 * 06_header_guards.c — why every header starts with #ifndef X / #define X
 *
 * Concept : #include is copy-paste. If two headers both include "point.h",
 *           the struct is pasted twice and the compiler says
 *           "error: redefinition of 'struct point'". A header guard makes the
 *           second paste empty. This file INCLUDES ITSELF to prove it: the top
 *           part plays "point.h", the bottom part plays "main.c".
 * Build   : make run P=dsa/00-foundations/c/06_header_guards.c
 * Read    : ../README.md  (section "Header guards")
 *
 * Experiment: comment out the #ifndef/#define/#endif of POINT_H below and
 * rebuild — you get exactly the redefinition error described above.
 * See the double paste with:  gcc -E dsa/00-foundations/c/06_header_guards.c | grep -n 'struct point {'
 */

/* =================== "point.h" ==================================== */
#ifndef POINT_H              /* first time: not defined -> keep going   */
#define POINT_H              /* now defined: next paste skips to #endif */

struct point {
    int x;
    int y;
};

/* A header should contain DECLARATIONS. A non-static function DEFINITION in
 * a header, included from two .c files, gives "multiple definition" at LINK
 * time (the guard only protects one translation unit!). static inline is OK. */
static inline int point_manhattan(struct point p) { return (p.x < 0 ? -p.x : p.x) + (p.y < 0 ? -p.y : p.y); }

#endif /* POINT_H */

/* =================== "main.c" ===================================== */
#ifndef MAIN_PART_DONE
#define MAIN_PART_DONE

/* Simulate a second header that also includes "point.h":
 * this pastes the WHOLE file again; the guard makes the point.h part empty
 * and MAIN_PART_DONE makes this part empty.                              */
#include "06_header_guards.c"   /* quotes: searched next to THIS file first */

#include <assert.h>
#include <stdio.h>

int main(void)
{
    struct point p = {3, -4};
    int d = point_manhattan(p);
    printf("struct point defined once despite two includes; |3| + |-4| = %d\n", d);
    assert(d == 7);

    /* #pragma once does the same job; not in the standard but supported by
     * gcc/clang/MSVC. The kernel and most C projects use #ifndef guards.   */
    printf("All checks passed.\n");
    return 0;
}

#endif /* MAIN_PART_DONE */
