/*
 * 01_static_arrays_memory.c — static arrays, memory layout, decay, 2D row-major
 *
 * Concept : An array is N same-sized elements packed back to back, so
 *           a[i] lives at base + i*sizeof(T) and indexing is O(1).
 *           Shows sizeof vs element count, array-to-pointer decay when
 *           passed to a function, row-major 2D layout, and why walking a
 *           2D array row-by-row is cache-friendly and column-by-column is not.
 * Build   : make run P=dsa/05-arrays-and-strings/c/01_static_arrays_memory.c
 * Read    : ../README.md  (sections "Static arrays" and "2D arrays")
 */
#define _POSIX_C_SOURCE 200809L /* for clock_gettime under -std=c11 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/* Element count of a REAL array (not a pointer!). Passing a pointer here
 * gives sizeof(ptr)/sizeof(elem) = garbage — the #1 array bug in C. */
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Inside a function, an "array parameter" is really a pointer.
 * `int a[]`, `int a[5]` and `int *a` all mean exactly the same thing as a
 * parameter, so the function cannot know the length: you MUST pass n
 * separately. (Written as `int a[]`, GCC even warns: "sizeof on array
 * function parameter will return size of int *" — -Wsizeof-array-argument.
 * We spell it `int *a` so the build stays warning-free.) */
static size_t sizeof_param(const int *a)
{
    return sizeof(a); /* sizeof(int *) — 8 on 64-bit, NOT the array size */
}

/* O(n): visit each element once. */
static long sum(const int *a, size_t n)
{
    long s = 0;
    for (size_t i = 0; i < n; i++)
        s += a[i];          /* a[i] is defined as *(a + i) */
    return s;
}

/* Passing a 2D array: the compiler must know the ROW LENGTH (COLS) to
 * compute m[r][c] = base + (r*COLS + c)*sizeof(int). The first dimension
 * may be omitted; the others may not. */
#define ROWS 3
#define COLS 4
static int sum2d(int m[][COLS], int rows)
{
    int s = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < COLS; c++)
            s += m[r][c];
    return s;
}

/* A heap-allocated "2D" matrix done the embedded/graphics way: ONE block,
 * indexed by hand. Same layout as a framebuffer: pixel(x,y) = buf[y*stride+x]. */
static int *matrix_new(size_t rows, size_t cols)
{
    /* Ownership: caller frees with free(). calloc zero-fills and checks
     * rows*cols for overflow for us. */
    return calloc(rows * cols, sizeof(int));
}

static double seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    puts("== 1. Layout of a 1D array ==");
    int a[5] = {10, 20, 30, 40, 50};
    /*
     *  index:      0      1      2      3      4
     *           +------+------+------+------+------+
     *  a  ----> |  10  |  20  |  30  |  40  |  50  |
     *           +------+------+------+------+------+
     *  address: base   +4     +8     +12    +16        (sizeof(int) == 4)
     *
     *  a[i]  ==  *(a + i)  ==  *(int *)((char *)a + i * sizeof(int))
     */
    for (size_t i = 0; i < ARRAY_SIZE(a); i++) {
        printf("  a[%zu] = %d at %p (offset %td bytes)\n", i, a[i],
               (void *)&a[i], (char *)&a[i] - (char *)a);
        CHECK((char *)&a[i] - (char *)a == (ptrdiff_t)(i * sizeof(int)));
        CHECK(a[i] == *(a + i));
    }
    CHECK(sizeof(a) == 5 * sizeof(int));
    CHECK(ARRAY_SIZE(a) == 5);
    printf("  sizeof(a) = %zu bytes, ARRAY_SIZE(a) = %zu elements\n",
           sizeof(a), ARRAY_SIZE(a));

    puts("\n== 2. Decay: arrays become pointers when passed ==");
    printf("  sizeof(a) in main = %zu, sizeof(param) in function = %zu\n",
           sizeof(a), sizeof_param(a));
    CHECK(sizeof_param(a) == sizeof(int *));
    CHECK(sum(a, ARRAY_SIZE(a)) == 150);
    /* &a has type int (*)[5]: a pointer to the WHOLE array. Same address
     * as a, but +1 jumps over all 5 ints (20 bytes). */
    CHECK((void *)&a == (void *)a);
    CHECK((char *)(&a + 1) - (char *)a == (ptrdiff_t)sizeof(a));
    puts("  &a == a (same address) but &a + 1 skips the whole array");

    puts("\n== 3. Initialisation rules ==");
    int partial[6] = {1, 2};          /* rest are zero-filled */
    int designated[6] = {[4] = 7};    /* C99 designated initialiser */
    static int zeroed[4];             /* static storage => all zero (.bss) */
    CHECK(partial[2] == 0 && partial[5] == 0);
    CHECK(designated[4] == 7 && designated[0] == 0);
    CHECK(zeroed[3] == 0);
    puts("  {1,2} zero-fills the rest; {[4]=7} sets one slot; static is zeroed");

    puts("\n== 4. 2D arrays are row-major ==");
    int m[ROWS][COLS] = {
        {0, 1, 2, 3},
        {4, 5, 6, 7},
        {8, 9, 10, 11},
    };
    /*
     *  Logical view (3x4):          Memory (one flat run of 12 ints):
     *     c0 c1 c2 c3
     *  r0  0  1  2  3               | 0 1 2 3 | 4 5 6 7 | 8 9 10 11 |
     *  r1  4  5  6  7                 <-row 0->  <-row 1-> <--row 2-->
     *  r2  8  9 10 11
     *
     *  &m[r][c] == (int *)m + r*COLS + c
     */
    const int *flat = &m[0][0];
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            CHECK(&m[r][c] == flat + r * COLS + c);
    CHECK(sizeof(m[0]) == COLS * sizeof(int)); /* one row */
    CHECK(sum2d(m, ROWS) == 66);
    puts("  &m[r][c] == base + r*COLS + c  verified for every cell");

    puts("\n== 5. Heap matrix as one block (framebuffer style) ==");
    const size_t R = 2048, C = 2048;
    int *big = matrix_new(R, C);
    CHECK(big != NULL);
    for (size_t r = 0; r < R; r++)
        for (size_t c = 0; c < C; c++)
            big[r * C + c] = (int)((r + c) & 0xff);

    /* Row-major walk: consecutive addresses -> every 64-byte cache line
     * fetched is fully used (16 ints). Column walk: each access jumps
     * C*4 = 8 KiB, so nearly every access is a cache miss. */
    long s1 = 0, s2 = 0;
    double t0 = seconds();
    for (size_t r = 0; r < R; r++)
        for (size_t c = 0; c < C; c++)
            s1 += big[r * C + c];
    double t1 = seconds();
    for (size_t c = 0; c < C; c++)
        for (size_t r = 0; r < R; r++)
            s2 += big[r * C + c];
    double t2 = seconds();
    CHECK(s1 == s2);
    printf("  row-by-row: %.3f s   column-by-column: %.3f s  (same sum %ld)\n",
           t1 - t0, t2 - t1, s1);
    puts("  (same O(n) work; the difference is purely cache behaviour)");
    free(big); /* we allocated it, we free it */

    puts("\n== 6. Copying arrays ==");
    int src[4] = {1, 2, 3, 4}, dst[4];
    /* dst = src; is a compile error: arrays are not assignable in C.
     * memcpy copies bytes: O(n). */
    memcpy(dst, src, sizeof src);
    CHECK(memcmp(dst, src, sizeof src) == 0);
    /* ...but a struct containing an array IS assignable (copies all bytes). */
    struct box { int v[4]; } b1 = {{5, 6, 7, 8}}, b2;
    b2 = b1;
    CHECK(b2.v[3] == 8);
    puts("  arrays: memcpy; arrays wrapped in a struct: plain assignment");

    puts("\nall checks passed");
    return 0;
}
