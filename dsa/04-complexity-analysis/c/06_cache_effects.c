/*
 * 06_cache_effects.c — why O(n) over a linked list is much slower than O(n) over an array
 *
 * Concept : Big-O counts operations, but a memory access costs ~1 ns from L1
 *           cache and ~80-100 ns from DRAM. The CPU loads 64-byte CACHE LINES
 *           and PREFETCHES sequential accesses. An array walk uses every byte
 *           of every line and is perfectly predictable; a linked list whose
 *           nodes are scattered makes every step a dependent cache MISS
 *           (pointer chasing). Same O(n), 5-50x different time.
 *           Also: row-major vs column-major traversal of a 2-D array.
 * Build   : make run P=dsa/04-complexity-analysis/c/06_cache_effects.c
 *           For clean numbers: make OPT=1 SAN=0 run P=dsa/04-complexity-analysis/c/06_cache_effects.c
 * Read    : ../README.md  (section "3.8 Cache effects")
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct node { struct node *next; int64_t value; };   /* 16 bytes: 4 nodes per cache line */

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint64_t rng = 88172645463325252ull;
static uint64_t xorshift(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int64_t sum_array(const int64_t *a, size_t n)
{
    int64_t s = 0;
    for (size_t i = 0; i < n; i++) s += a[i];        /* sequential: prefetcher streams it */
    return s;
}

static int64_t sum_list(const struct node *h)
{
    int64_t s = 0;
    for (; h; h = h->next) s += h->value;             /* next address unknown until this load completes */
    return s;
}

int main(void)
{
    enum { N = 1 << 21 };                             /* 2M elements: 16 MiB array, 32 MiB of nodes */
    int64_t *arr = malloc(N * sizeof *arr);
    struct node *pool = malloc(N * sizeof *pool);     /* all nodes in one block... */
    size_t *order = malloc(N * sizeof *order);
    assert(arr && pool && order);

    for (size_t i = 0; i < N; i++) { arr[i] = (int64_t)i; order[i] = i; }

    /* List A: nodes linked in memory order (best case for a list). */
    for (size_t i = 0; i < N; i++) {
        pool[i].value = (int64_t)i;
        pool[i].next = i + 1 < N ? &pool[i + 1] : NULL;
    }
    struct node *seq_head = &pool[0];

    double t0 = now(); int64_t s1 = sum_array(arr, N); double t_arr = now() - t0;
    t0 = now();        int64_t s2 = sum_list(seq_head); double t_seq = now() - t0;

    /* List B: same nodes, linked in a RANDOM order (what a long-lived heap gives you). */
    for (size_t i = N - 1; i > 0; i--) {             /* Fisher-Yates shuffle */
        size_t j = (size_t)(xorshift() % (i + 1));
        size_t t = order[i]; order[i] = order[j]; order[j] = t;
    }
    for (size_t i = 0; i < N; i++)
        pool[order[i]].next = i + 1 < N ? &pool[order[i + 1]] : NULL;
    struct node *rnd_head = &pool[order[0]];
    t0 = now(); int64_t s3 = sum_list(rnd_head); double t_rnd = now() - t0;

    int64_t expect = (int64_t)N * (N - 1) / 2;
    assert(s1 == expect && s2 == expect && s3 == expect);
    printf("Sum of %d int64 values, all O(n):\n", N);
    printf("  array                     %7.2f ms  (%.2f ns/element)\n", t_arr * 1e3, t_arr * 1e9 / N);
    printf("  list, nodes in order      %7.2f ms  (%.2f ns/element)  x%.1f vs array\n", t_seq * 1e3, t_seq * 1e9 / N, t_seq / t_arr);
    printf("  list, nodes shuffled      %7.2f ms  (%.2f ns/element)  x%.1f vs array\n", t_rnd * 1e3, t_rnd * 1e9 / N, t_rnd / t_arr);

    /* Row-major vs column-major over the same 2-D array (C stores rows contiguously). */
    enum { R = 2048, C = 2048 };
    int32_t *m = malloc((size_t)R * C * sizeof *m);
    assert(m);
    for (size_t i = 0; i < (size_t)R * C; i++) m[i] = 1;
    int64_t a = 0, b = 0;
    t0 = now();
    for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) a += m[(size_t)r * C + c];   /* stride 4 B   */
    double t_row = now() - t0;
    t0 = now();
    for (int c = 0; c < C; c++) for (int r = 0; r < R; r++) b += m[(size_t)r * C + c];   /* stride 8 KiB */
    double t_col = now() - t0;
    assert(a == (int64_t)R * C && b == a);
    printf("\n%dx%d int32 matrix sum:\n  row by row (sequential)   %7.2f ms\n  column by column (stride %d B) %7.2f ms  x%.1f\n",
           R, C, t_row * 1e3, C * 4, t_col * 1e3, t_col / t_row);

    printf("\n(Timings vary run to run and are inflated by ASan/-O0; the ORDER is what matters:\n"
           " array < in-order list << shuffled list, and row-major << column-major.)\n");
    free(m); free(order); free(pool); free(arr);
    printf("All checks passed.\n");
    return 0;
}
