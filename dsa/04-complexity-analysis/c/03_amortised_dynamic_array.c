/*
 * 03_amortised_dynamic_array.c — why push_back is O(1) "amortised" (and when it is not)
 *
 * Concept : A dynamic array that is full must allocate a bigger buffer and
 *           COPY everything: that one push costs O(n). If the capacity grows
 *           GEOMETRICALLY (x2, or x1.5), copies total 1 + 2 + 4 + ... + n < 2n,
 *           so the AVERAGE cost per push is O(1) — amortised. If it grows by a
 *           CONSTANT (+16), copies total ~ n^2 / 32 -> O(n) per push.
 *           This program counts the copies exactly and also times both.
 * Build   : make run P=dsa/04-complexity-analysis/c/03_amortised_dynamic_array.c
 * Read    : ../README.md  (section "3.4 Amortised analysis")
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct vec {
    int *data;
    size_t len, cap;
    size_t copies;         /* element copies done by growth: the hidden cost */
    size_t reallocs;
    size_t worst_push;     /* most copies done by a single push */
};

enum growth { DOUBLE, ONE_AND_HALF, PLUS_16 };

static size_t next_cap(size_t cap, enum growth g)
{
    switch (g) {
    case DOUBLE:       return cap ? cap * 2 : 1;
    case ONE_AND_HALF: return cap < 2 ? cap + 1 : cap + cap / 2;
    case PLUS_16:      return cap + 16;
    }
    return cap + 1;
}

static void push(struct vec *v, int x, enum growth g)
{
    if (v->len == v->cap) {
        size_t nc = next_cap(v->cap, g);
        int *nd = malloc(nc * sizeof *nd);          /* new buffer (realloc could hide the copy) */
        assert(nd);
        if (v->len) memcpy(nd, v->data, v->len * sizeof *nd);   /* O(len) */
        free(v->data);
        v->data = nd;
        v->cap = nc;
        v->copies += v->len;
        if (v->len > v->worst_push) v->worst_push = v->len;
        v->reallocs++;
    }
    v->data[v->len++] = x;                          /* O(1) */
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static struct vec run(size_t n, enum growth g, double *secs)
{
    struct vec v = {0};
    double t0 = now();
    for (size_t i = 0; i < n; i++) push(&v, (int)i, g);
    *secs = now() - t0;
    assert(v.len == n && v.data[n - 1] == (int)(n - 1));
    free(v.data);
    v.data = NULL;
    return v;
}

int main(void)
{
    const char *names[] = {"x2", "x1.5", "+16"};
    printf("%-5s %9s %9s %12s %14s %12s %10s\n", "grow", "n", "reallocs", "copies", "copies/push", "worst push", "time");
    for (int g = 0; g < 3; g++) {
        for (size_t n = 1000; n <= 64000; n *= 4) {
            double t;
            struct vec v = run(n, (enum growth)g, &t);
            printf("%-5s %9zu %9zu %12zu %14.2f %12zu %8.2f ms\n", names[g], n, v.reallocs, v.copies,
                   (double)v.copies / n, v.worst_push, t * 1e3);
            if (g == DOUBLE)
                assert(v.copies < 2 * n);            /* geometric: total copies < 2n */
            if (g == ONE_AND_HALF)
                assert(v.copies < 3 * n);            /* factor 1.5: < n / (1.5-1) = 2n, + rounding */
            if (g == PLUS_16)
                assert(v.copies > n * (n / 16) / 2 - n);   /* arithmetic: ~ n^2 / 32 */
        }
    }
    /*
     *  Accounting ("banker's") view of doubling:
     *  charge every push 3 coins: 1 pays for the write, 2 are saved.
     *  When capacity c fills, the c/2 elements added since the last growth
     *  saved c coins: exactly enough to copy all c elements. The bank never
     *  goes negative -> each push is O(1) amortised, even though ONE push
     *  (the "worst push" column) costs O(n).
     */
    printf("\n=> x2 and x1.5: copies/push stays below 2-3 for any n (O(1) amortised).\n"
           "=> +16: copies/push grows with n (O(n) per push, O(n^2) total).\n"
           "=> 'worst push' is O(n) for all of them: amortised != worst case (real-time code cares!).\n");
    printf("All checks passed.\n");
    return 0;
}
