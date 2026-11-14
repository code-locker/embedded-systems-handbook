/*
 * 03_arrays_vs_pointers.c — an array is NOT a pointer (it just decays into one)
 *
 * Concept : An array is a block of N elements; its name is not a variable
 *           holding an address. In most expressions the name DECAYS to a
 *           pointer to the first element. Three exceptions where it does not:
 *           sizeof arr, &arr, and a string literal initialising a char array.
 *           A function parameter written "int a[]" IS a pointer.
 * Build   : make run P=dsa/01-pointers/c/03_arrays_vs_pointers.c
 * Read    : ../README.md  (section "3.3 Arrays vs pointers")
 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))   /* only valid on REAL arrays */

/* Note: gcc does use the bound for warnings: passing a 16-byte array to
 * "int a[10]" gives -Wstringop-overflow. Still, a is just an int*.
 * "int a[]" in a parameter list is rewritten by the compiler to "int *a".
 * The size is lost. That is why C APIs always pass (pointer, length).      */
static size_t param_size(int a[4])   /* the 4 is documentation only */
{
    /* gcc warns about sizeof on an array parameter (-Wsizeof-array-argument),
     * so we measure the pointer type explicitly to show what a REALLY is.   */
    (void)a;
    return sizeof(int *);
}

static int sum(const int *a, size_t n)
{
    int s = 0;
    for (size_t i = 0; i < n; i++)
        s += a[i];
    return s;
}

/* Pointer to the WHOLE array keeps the size in its type.
 * (Not "const int (*)[4]": before C23, passing an int(*)[4] to it is a
 *  qualifier mismatch that -Wpedantic reports — a known C wart.)          */
static int sum4(int (*arr)[4])
{
    int s = 0;
    for (size_t i = 0; i < ARRAY_LEN(*arr); i++)   /* *arr is a real int[4] */
        s += (*arr)[i];
    return s;
}

int main(void)
{
    int arr[4] = {1, 2, 3, 4};
    int *p = arr;                   /* decay: arr -> &arr[0], type int* */

    /*
     *   arr (16 bytes, no separate pointer stored anywhere)     p (8 bytes)
     *   +-----+-----+-----+-----+                              +-----------+
     *   |  1  |  2  |  3  |  4  |  <──────────────────────────  | &arr[0]   |
     *   +-----+-----+-----+-----+                              +-----------+
     *   ^ arr, &arr[0], &arr — same address, DIFFERENT types:
     *       arr / &arr[0] : int *        (+1 moves 4 bytes)
     *       &arr          : int (*)[4]   (+1 moves 16 bytes — the whole array)
     */
    printf("sizeof arr = %zu (whole array), sizeof p = %zu (just a pointer)\n", sizeof arr, sizeof p);
    assert(sizeof arr == 4 * sizeof(int));
    assert(sizeof p == sizeof(int *));
    assert(ARRAY_LEN(arr) == 4);

    printf("arr      = %p\n&arr[0]  = %p\n&arr     = %p   (same address)\n",
           (void *)arr, (void *)&arr[0], (void *)&arr);
    printf("arr + 1  = %p   (+%zu bytes)\n", (void *)(arr + 1), sizeof arr[0]);
    printf("&arr + 1 = %p   (+%zu bytes: next whole array)\n", (void *)(&arr + 1), sizeof arr);
    assert((void *)arr == (void *)&arr);
    assert((char *)(&arr + 1) - (char *)arr == (ptrdiff_t)sizeof arr);

    /* Decay at a function call: the callee gets a pointer, size is lost. */
    printf("inside f(int a[4]): sizeof a = %zu  (it is a pointer!)\n", param_size(arr));
    assert(param_size(arr) == sizeof(int *));
    assert(sum(arr, ARRAY_LEN(arr)) == 10);
    assert(sum4(&arr) == 10);

    /* You can move a pointer; you cannot assign to an array name. */
    p++;            /* fine */
    /* arr++;          error: lvalue required as increment operand */
    /* arr = p;        error: assignment to expression with array type */
    assert(*p == 2);

    /* Arrays cannot be copied with '=', but structs containing arrays can. */
    struct wrap { int v[4]; } w1 = {{9, 8, 7, 6}}, w2;
    w2 = w1;                         /* copies all 16 bytes */
    assert(memcmp(&w1, &w2, sizeof w1) == 0);

    /* String literal: array vs pointer. */
    char s_arr[] = "hey";            /* 4-byte array COPY on the stack, writable */
    const char *s_ptr = "hey";       /* pointer to read-only literal in .rodata */
    s_arr[0] = 'H';
    printf("sizeof s_arr = %zu, sizeof s_ptr = %zu, strlen = %zu, s_arr=\"%s\"\n",
           sizeof s_arr, sizeof s_ptr, strlen(s_ptr), s_arr);
    assert(sizeof s_arr == 4 && strlen(s_arr) == 3);

    /* extern int x[]; vs extern int *x; are NOT interchangeable across files:
     * declaring an array as a pointer makes the code read the first 8 bytes
     * of the array as an address -> crash. Declarations must match exactly. */

    printf("All checks passed.\n");
    return 0;
}
