/*
 * 05_parallel_for.c — work distribution: summing an array with N threads,
 *                     static split vs dynamic chunks, and why partial
 *                     results must not share a cache line
 *
 * Concept : "Parallel for" = split an index range across threads, let each
 *           compute a PARTIAL result with no locking at all, then combine
 *           the partials after join. Two ways to split:
 *             static : thread t gets [t*n/T, (t+1)*n/T). Zero overhead,
 *                      but if some items are slower, one thread finishes
 *                      last and the others idle (load imbalance).
 *             dynamic: threads grab the next chunk from a shared atomic
 *                      counter. Balances itself; costs one atomic per chunk.
 *           This is what OpenMP's schedule(static) / schedule(dynamic) do.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/05_parallel_for.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/05_parallel_for.c SAN=tsan
 * Read    : ../README.md  (section "Parallel for")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define N (1 << 20) /* 1M elements */
#define MAX_THREADS 8
#define CHUNK 4096

static uint32_t *g_data;

/* Each partial sum on its OWN cache line. With a plain `uint64_t
 * partial[8]` all eight share one or two 64-byte lines, and every += from
 * one core invalidates the line in the others ("false sharing"): correct
 * results, several times slower. */
typedef struct {
    _Alignas(64) uint64_t sum;
} Partial;
static Partial g_partial[MAX_THREADS];

/* ---- static schedule ----------------------------------------------------- */
typedef struct { int id, nthreads; } StaticArg;

static void *static_worker(void *p)
{
    StaticArg *a = p;
    size_t lo = (size_t)a->id * N / a->nthreads;
    size_t hi = (size_t)(a->id + 1) * N / a->nthreads; /* covers remainder */
    uint64_t s = 0; /* accumulate in a local (register), store once */
    for (size_t i = lo; i < hi; i++)
        s += g_data[i];
    g_partial[a->id].sum = s;
    return NULL;
}

/* ---- dynamic schedule ---------------------------------------------------- */
static atomic_size_t g_next_chunk;
static atomic_int g_chunks_by[MAX_THREADS];

static void *dynamic_worker(void *p)
{
    int id = *(int *)p;
    uint64_t s = 0;
    for (;;) {
        size_t start = atomic_fetch_add_explicit(&g_next_chunk, CHUNK,
                                                 memory_order_relaxed);
        if (start >= N)
            break;
        size_t end = start + CHUNK < N ? start + CHUNK : N;
        for (size_t i = start; i < end; i++)
            s += g_data[i];
        atomic_fetch_add_explicit(&g_chunks_by[id], 1, memory_order_relaxed);
    }
    g_partial[id].sum = s;
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint64_t combine(int nthreads)
{
    /* Read after pthread_join: join is the synchronisation that makes the
     * workers' writes visible. No atomics needed for the partials. */
    uint64_t total = 0;
    for (int t = 0; t < nthreads; t++)
        total += g_partial[t].sum;
    return total;
}

int main(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = ncpu < 2 ? 2 : (ncpu > 4 ? 4 : (int)ncpu);

    g_data = malloc(sizeof(uint32_t) * N);
    CHECK(g_data != NULL);
    uint64_t expect = 0;
    for (size_t i = 0; i < N; i++) {
        g_data[i] = (uint32_t)(i * 2654435761u >> 20); /* pseudo-random */
        expect += g_data[i];
    }

    pthread_t th[MAX_THREADS];

    /* static */
    double t0 = now_ms();
    StaticArg sa[MAX_THREADS];
    for (int t = 0; t < nthreads; t++) {
        sa[t] = (StaticArg){t, nthreads};
        CHECK(pthread_create(&th[t], NULL, static_worker, &sa[t]) == 0);
    }
    for (int t = 0; t < nthreads; t++)
        CHECK(pthread_join(th[t], NULL) == 0);
    uint64_t s1 = combine(nthreads);
    double t1 = now_ms();
    printf("static : %d threads, sum %llu (expected %llu) in %.2f ms\n",
           nthreads, (unsigned long long)s1, (unsigned long long)expect, t1 - t0);
    CHECK(s1 == expect);

    /* dynamic */
    atomic_store(&g_next_chunk, 0);
    int ids[MAX_THREADS];
    for (int t = 0; t < nthreads; t++) {
        ids[t] = t;
        atomic_store(&g_chunks_by[t], 0);
        CHECK(pthread_create(&th[t], NULL, dynamic_worker, &ids[t]) == 0);
    }
    for (int t = 0; t < nthreads; t++)
        CHECK(pthread_join(th[t], NULL) == 0);
    uint64_t s2 = combine(nthreads);
    double t2 = now_ms();
    int chunks = 0;
    printf("dynamic: sum %llu in %.2f ms; chunks per thread:",
           (unsigned long long)s2, t2 - t1);
    for (int t = 0; t < nthreads; t++) {
        printf(" %d", atomic_load(&g_chunks_by[t]));
        chunks += atomic_load(&g_chunks_by[t]);
    }
    printf(" (timing-dependent)\n");
    CHECK(s2 == expect);
    CHECK(chunks == (N + CHUNK - 1) / CHUNK); /* every chunk exactly once */

    free(g_data);
    printf("05_parallel_for: all checks passed\n");
    return 0;
}
