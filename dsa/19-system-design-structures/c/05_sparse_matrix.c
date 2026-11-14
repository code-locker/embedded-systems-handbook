/*
 * 05_sparse_matrix.c — sparse matrices: COO (triplets) -> CSR, mat-vec, transpose
 *
 * Concept : A matrix that is mostly zeros wastes memory and time when stored
 *           densely. COO stores (row, col, value) triplets — easy to build.
 *           CSR (compressed sparse row) sorts by row and replaces the row
 *           array with row_ptr offsets — compact and fast to multiply.
 *           Memory: dense = R*C values; CSR = nnz values + nnz cols + R+1 ptrs.
 * Build   : make run P=dsa/19-system-design-structures/c/05_sparse_matrix.c
 * Read    : ../README.md  (section "Sparse matrix")
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *      dense 4x5                     CSR
 *  [ 5 0 0 0 0 ]       values  = [ 5  8  3  6  2  7 ]
 *  [ 0 8 0 3 0 ]       col_idx = [ 0  1  3  1  2  4 ]
 *  [ 0 0 0 0 0 ]       row_ptr = [ 0  1  3  3  6 ]
 *  [ 0 6 2 0 7 ]                   ^  ^  ^  ^  ^
 *                       row r owns values[row_ptr[r] .. row_ptr[r+1])
 *                       row 2 is empty: row_ptr[2] == row_ptr[3]
 */
typedef struct { int row, col; double val; } Triplet;

typedef struct {
    int rows, cols, nnz;
    double *values;
    int *col_idx;
    int *row_ptr;          /* rows + 1 entries */
} CSR;

/* COO -> CSR in O(nnz + rows) with a counting sort by row (no comparisons).
   Duplicates (same row,col) are summed — the convention of most libraries. */
static CSR csr_from_coo(int rows, int cols, const Triplet *t, int n)
{
    CSR m = { rows, cols, 0, NULL, NULL, NULL };
    m.row_ptr = calloc((size_t)rows + 1, sizeof *m.row_ptr);
    int *ccol = malloc((size_t)(n ? n : 1) * sizeof *ccol);
    double *cval = malloc((size_t)(n ? n : 1) * sizeof *cval);
    assert(m.row_ptr && ccol && cval);

    for (int i = 0; i < n; i++) m.row_ptr[t[i].row + 1]++;       /* 1. count per row  */
    for (int r = 0; r < rows; r++) m.row_ptr[r + 1] += m.row_ptr[r]; /* 2. prefix sums */
    int *next = malloc((size_t)rows * sizeof *next + 1);
    assert(next);
    memcpy(next, m.row_ptr, (size_t)rows * sizeof *next);
    for (int i = 0; i < n; i++) {                                 /* 3. scatter        */
        int dst = next[t[i].row]++;
        ccol[dst] = t[i].col;
        cval[dst] = t[i].val;
    }
    free(next);

    /* 4. inside each row: sort by column (insertion sort: rows are short),
          then merge duplicates and drop explicit zeros. */
    m.values = malloc((size_t)(n ? n : 1) * sizeof *m.values);
    m.col_idx = malloc((size_t)(n ? n : 1) * sizeof *m.col_idx);
    assert(m.values && m.col_idx);
    int out = 0;
    for (int r = 0; r < rows; r++) {
        int lo = m.row_ptr[r], hi = m.row_ptr[r + 1];
        for (int i = lo + 1; i < hi; i++) {
            int c = ccol[i]; double v = cval[i]; int j = i - 1;
            while (j >= lo && ccol[j] > c) { ccol[j + 1] = ccol[j]; cval[j + 1] = cval[j]; j--; }
            ccol[j + 1] = c; cval[j + 1] = v;
        }
        m.row_ptr[r] = out;
        for (int i = lo; i < hi; ) {
            int c = ccol[i]; double s = 0;
            while (i < hi && ccol[i] == c) s += cval[i++];
            if (s != 0.0) { m.col_idx[out] = c; m.values[out] = s; out++; }
        }
    }
    m.row_ptr[rows] = out;
    m.nnz = out;
    free(ccol);
    free(cval);
    return m;
}

static void csr_free(CSR *m) { free(m->values); free(m->col_idx); free(m->row_ptr); }

/* y = A x : O(nnz) instead of O(rows*cols). Reads values/col_idx sequentially
   — the reason CSR is the default format for SpMV in scientific codes. */
static void csr_matvec(const CSR *m, const double *x, double *y)
{
    for (int r = 0; r < m->rows; r++) {
        double s = 0;
        for (int k = m->row_ptr[r]; k < m->row_ptr[r + 1]; k++)
            s += m->values[k] * x[m->col_idx[k]];
        y[r] = s;
    }
}

/* A(r,c): O(log(nnz in row)) with binary search on the sorted columns. */
static double csr_get(const CSR *m, int r, int c)
{
    int lo = m->row_ptr[r], hi = m->row_ptr[r + 1] - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (m->col_idx[mid] == c) return m->values[mid];
        if (m->col_idx[mid] < c) lo = mid + 1; else hi = mid - 1;
    }
    return 0.0;
}

/* Transpose CSR -> CSR (equivalently: CSR -> CSC) in O(nnz + cols),
   again a counting sort, this time by column. */
static CSR csr_transpose(const CSR *a)
{
    CSR t = { a->cols, a->rows, a->nnz, NULL, NULL, NULL };
    t.row_ptr = calloc((size_t)t.rows + 1, sizeof *t.row_ptr);
    t.values = malloc((size_t)(a->nnz ? a->nnz : 1) * sizeof *t.values);
    t.col_idx = malloc((size_t)(a->nnz ? a->nnz : 1) * sizeof *t.col_idx);
    assert(t.row_ptr && t.values && t.col_idx);
    for (int k = 0; k < a->nnz; k++) t.row_ptr[a->col_idx[k] + 1]++;
    for (int r = 0; r < t.rows; r++) t.row_ptr[r + 1] += t.row_ptr[r];
    int *next = malloc((size_t)t.rows * sizeof *next + 1);
    assert(next);
    memcpy(next, t.row_ptr, (size_t)t.rows * sizeof *next);
    for (int r = 0; r < a->rows; r++)            /* rows visited in order => output */
        for (int k = a->row_ptr[r]; k < a->row_ptr[r + 1]; k++) { /* cols come sorted */
            int dst = next[a->col_idx[k]]++;
            t.col_idx[dst] = r;
            t.values[dst] = a->values[k];
        }
    free(next);
    return t;
}

int main(void)
{
    printf("== the 4x5 example ==\n");
    Triplet ex[] = { {3,4,7}, {0,0,5}, {1,3,3}, {3,1,6}, {1,1,8}, {3,2,2},
                     {1,3,0.5}, {1,3,-0.5} };          /* duplicate that cancels in sum */
    CSR m = csr_from_coo(4, 5, ex, (int)(sizeof ex / sizeof ex[0]));
    printf("values :"); for (int k = 0; k < m.nnz; k++) printf(" %g", m.values[k]);
    printf("\ncol_idx:"); for (int k = 0; k < m.nnz; k++) printf(" %d", m.col_idx[k]);
    printf("\nrow_ptr:"); for (int r = 0; r <= m.rows; r++) printf(" %d", m.row_ptr[r]);
    printf("\n");
    int want_ptr[] = { 0, 1, 3, 3, 6 }, want_col[] = { 0, 1, 3, 1, 2, 4 };
    double want_val[] = { 5, 8, 3, 6, 2, 7 };
    assert(m.nnz == 6);
    for (int r = 0; r <= 4; r++) assert(m.row_ptr[r] == want_ptr[r]);
    for (int k = 0; k < 6; k++) assert(m.col_idx[k] == want_col[k] && m.values[k] == want_val[k]);
    csr_free(&m);

    printf("\n== random matrices vs dense reference ==\n");
    srand(5);
    for (int trial = 0; trial < 30; trial++) {
        int R = 1 + rand() % 60, C = 1 + rand() % 60;
        int n = rand() % (R * C / 10 + 2);              /* ~<=10% density + duplicates */
        Triplet *t = malloc((size_t)(n ? n : 1) * sizeof *t);
        double *dense = calloc((size_t)R * C, sizeof *dense);
        assert(t && dense);
        for (int i = 0; i < n; i++) {
            t[i] = (Triplet){ rand() % R, rand() % C, (double)(rand() % 19 - 9) };
            dense[t[i].row * C + t[i].col] += t[i].val;
        }
        CSR a = csr_from_coo(R, C, t, n);
        for (int r = 0; r < R; r++)
            for (int c = 0; c < C; c++)
                assert(csr_get(&a, r, c) == dense[r * C + c]);

        double *x = malloc((size_t)C * sizeof *x), *y = malloc((size_t)R * sizeof *y);
        assert(x && y);
        for (int c = 0; c < C; c++) x[c] = rand() % 7 - 3;
        csr_matvec(&a, x, y);
        for (int r = 0; r < R; r++) {
            double s = 0;
            for (int c = 0; c < C; c++) s += dense[r * C + c] * x[c];
            assert(fabs(s - y[r]) < 1e-9);
        }
        CSR at = csr_transpose(&a);
        for (int r = 0; r < R; r++)
            for (int c = 0; c < C; c++)
                assert(csr_get(&at, c, r) == dense[r * C + c]);
        csr_free(&at);
        csr_free(&a);
        free(x); free(y); free(t); free(dense);
    }
    printf("30 random matrices: get, matvec, transpose all match dense\n");

    printf("\n== memory: 10000x10000 with 5 non-zeros per row ==\n");
    double R = 10000, C = 10000, nnz = 5 * R;
    double dense_b = R * C * sizeof(double);
    double csr_b = nnz * (sizeof(double) + sizeof(int)) + (R + 1) * sizeof(int);
    printf("dense: %.0f MB   CSR: %.2f MB   (%.0fx smaller)\n",
           dense_b / 1e6, csr_b / 1e6, dense_b / csr_b);
    assert(csr_b * 1000 < dense_b);
    printf("OK\n");
    return 0;
}
