/*
 * 11_multidim_arrays.c — int a[3][4], int (*p)[4], int **, and a flat image buffer
 *
 * Concept : int a[3][4] is ONE contiguous block of 12 ints, row-major:
 *           a[r][c] lives at offset (r*4 + c) * sizeof(int). a decays to
 *           int (*)[4] — "pointer to a row of 4". int** is something else
 *           entirely: an array of pointers to separately allocated rows.
 *           Images (camera frames, video planes) are flat buffers indexed
 *           with y * stride + x.
 * Build   : make run P=dsa/01-pointers/c/11_multidim_arrays.c
 * Read    : ../README.md  (section "3.11 Multidimensional arrays")
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ROWS 3
#define COLS 4

/* Receives a pointer to rows of COLS ints: the column count must be known. */
static int sum_fixed(int (*m)[COLS], int rows)
{
    int s = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < COLS; c++)
            s += m[r][c];   /* m[r] = *(m + r): jumps r * 16 bytes */
    return s;
}

/* Generic: flat pointer + dimensions (what you do for any runtime size). */
static int sum_flat(const int *m, int rows, int cols)
{
    int s = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            s += m[r * cols + c];
    return s;
}

/* int** version: rows allocated separately (jagged allowed, NOT contiguous). */
static int **alloc_rows(int rows, int cols)
{
    int **m = malloc((size_t)rows * sizeof *m);
    if (!m) return NULL;
    for (int r = 0; r < rows; r++) {
        m[r] = calloc((size_t)cols, sizeof **m);
        if (!m[r]) { while (r--) free(m[r]); free(m); return NULL; }
    }
    return m;
}
static void free_rows(int **m, int rows)
{
    for (int r = 0; r < rows; r++) free(m[r]);
    free(m);
}

/* Contiguous dynamic 2-D array with real [r][c] syntax: one malloc. */
static int (*alloc_matrix(int rows))[COLS]
{
    return malloc((size_t)rows * sizeof(int[COLS]));
}

int main(void)
{
    int a[ROWS][COLS];
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            a[r][c] = r * 10 + c;

    /*
     *  a[3][4] in memory (row-major, one block of 48 bytes):
     *
     *  offset: 0   4   8   12 | 16  20  24  28 | 32  36  40  44
     *         [ 0 | 1 | 2 | 3 ][10 |11 |12 |13 ][20 |21 |22 |23 ]
     *          ^a[0]            ^a[1]            ^a[2]
     *   a       : int (*)[4]  a+1 jumps one row (16 bytes)
     *   a[1]    : int *       a[1]+1 jumps one int
     *   &a      : int (*)[3][4]  &a+1 jumps the whole 48 bytes
     */
    printf("sizeof a = %zu, sizeof a[0] = %zu, sizeof a[0][0] = %zu\n", sizeof a, sizeof a[0], sizeof a[0][0]);
    printf("a = %p, a+1 = %p (+%td bytes), &a[1][2] = %p\n", (void *)a, (void *)(a + 1),
           (char *)(a + 1) - (char *)a, (void *)&a[1][2]);
    assert((char *)(a + 1) - (char *)a == COLS * (ptrdiff_t)sizeof(int));
    assert(&a[1][2] == &a[0][0] + 1 * COLS + 2);    /* row-major formula */
    assert(*(*(a + 2) + 3) == a[2][3]);             /* what a[2][3] really means */

    int (*rowp)[COLS] = a;          /* pointer to a row: the decayed type */
    assert(rowp[2][1] == 21);

    assert(sum_fixed(a, ROWS) == sum_flat(&a[0][0], ROWS, COLS));

    /*
     *  int **m : an array of row POINTERS, each row a separate heap block
     *   m ──► [ ● ]──► [0 0 0 0]
     *         [ ● ]──► [0 0 0 0]      rows can be anywhere in the heap,
     *         [ ● ]──► [0 0 0 0]      two loads per access: m[r] then [c]
     */
    int **m = alloc_rows(ROWS, COLS);
    assert(m);
    m[2][3] = 7;
    assert(m[2][3] == 7);
    /* sum_fixed(m, ROWS);  -> type error: int** is not int (*)[4] */
    free_rows(m, ROWS);

    int (*mat)[COLS] = alloc_matrix(5);
    assert(mat);
    mat[4][3] = 99;
    assert(((int *)mat)[4 * COLS + 3] == 99);     /* contiguous: flat view works */
    free(mat);

    /* A grayscale "frame": width 6, but rows padded to stride 8 (hardware
     * often needs stride aligned to 16/64/128 bytes, e.g. TI VPAC/DSS).   */
    enum { W = 6, H = 3, STRIDE = 8 };
    uint8_t *frame = calloc(H * STRIDE, 1);
    assert(frame);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            frame[y * STRIDE + x] = (uint8_t)(y * W + x);   /* pixel (x,y) */
    assert(frame[2 * STRIDE + 5] == 17);
    assert(frame[0 * STRIDE + 7] == 0);                       /* padding untouched */
    printf("frame pixel (5,2) = %u at offset y*stride+x = %d\n", frame[2 * STRIDE + 5], 2 * STRIDE + 5);
    free(frame);

    printf("All checks passed.\n");
    return 0;
}
