/*
 * 05_false_sharing.c — two threads, two DIFFERENT counters, same cache line:
 *                      measure the slowdown, fix it with padding
 *
 * Concept : Caches move memory in lines (64 bytes on Cortex-A72 and most
 *           x86). Two cores writing two different variables that happen to
 *           sit in the same line keep stealing the line from each other
 *           (MESI "invalidate" ping-pong) even though they never share any
 *           data. Correct, race-free — and several times slower. Fix: give
 *           each hot per-thread variable its own line (_Alignas(64)), or
 *           accumulate in a local and write once.
 *           Timings are PRINTED, never asserted (they depend on the machine
 *           and load); only the counts are checked.
 * Build   : make run P=linux-internals/02-race-conditions-and-atomics/c/05_false_sharing.c
 *           for meaningful numbers build optimised without sanitizers:
 *           make run P=linux-internals/02-race-conditions-and-atomics/c/05_false_sharing.c SAN=0 OPT=1
 * Read    : ../README.md  (section 3.11)
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

#define ITERS 2000000L
#define CACHE_LINE 64

/*
 *  PACKED: both counters in one 64-byte line
 *
 *     line 0: [ a | b | ........................... ]
 *              ^core 0 writes   ^core 1 writes
 *     every write invalidates the other core's copy -> line ping-pongs
 *
 *  PADDED: each counter owns a line
 *
 *     line 0: [ a | ............ pad ............. ]   stays in core 0
 *     line 1: [ b | ............ pad ............. ]   stays in core 1
 */
struct packed {
    atomic_long a;
    atomic_long b;
};

struct padded {
    _Alignas(CACHE_LINE) atomic_long a;
    _Alignas(CACHE_LINE) atomic_long b;
};

/* static storage, so _Alignas is honoured (malloc only guarantees 16) */
static struct packed g_packed;
static struct padded g_padded;
_Static_assert(sizeof(struct padded) == 2 * CACHE_LINE, "one line each");

typedef struct {
    atomic_long *counter;
} Arg;

static void *hammer(void *p)
{
    atomic_long *c = ((Arg *)p)->counter;
    for (long i = 0; i < ITERS; i++)
        /* Each counter has ONE writer, so a relaxed load + store would be
         * enough; fetch_add makes the cost of owning the line obvious. */
        atomic_fetch_add_explicit(c, 1, memory_order_relaxed);
    return NULL;
}

static long local_result[2];

static void *accumulate_locally(void *p)
{
    int slot = (int)(intptr_t)p;
    /* The real-world fix: work in a register/stack variable, publish once.
     * Nothing shared is touched in the loop at all. */
    long sum = 0;
    for (long i = 0; i < ITERS; i++)
        sum += 1;
    local_result[slot] = sum; /* read by main after join */
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static double run_pair(atomic_long *x, atomic_long *y)
{
    Arg ax = {x}, ay = {y};
    pthread_t t1, t2;
    double t0 = now_ms();
    CHECK(pthread_create(&t1, NULL, hammer, &ax) == 0);
    CHECK(pthread_create(&t2, NULL, hammer, &ay) == 0);
    CHECK(pthread_join(t1, NULL) == 0);
    CHECK(pthread_join(t2, NULL) == 0);
    return now_ms() - t0;
}

static long sysfs_line_size(void)
{
    FILE *f = fopen("/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size", "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

int main(void)
{
    printf("cache line: sysfs says %ld bytes, sysconf L1D says %ld (0 = unknown);"
           " this program assumes %d\n",
           sysfs_line_size(), sysconf(_SC_LEVEL1_DCACHE_LINESIZE), CACHE_LINE);
    printf("&packed.a = %p, &packed.b = %p (distance %zu bytes)\n",
           (void *)&g_packed.a, (void *)&g_packed.b,
           (size_t)((char *)&g_packed.b - (char *)&g_packed.a));
    printf("&padded.a = %p, &padded.b = %p (distance %zu bytes)\n\n",
           (void *)&g_padded.a, (void *)&g_padded.b,
           (size_t)((char *)&g_padded.b - (char *)&g_padded.a));

    double packed_ms = run_pair(&g_packed.a, &g_packed.b);
    double padded_ms = run_pair(&g_padded.a, &g_padded.b);

    pthread_t t1, t2;
    double t0 = now_ms();
    CHECK(pthread_create(&t1, NULL, accumulate_locally, (void *)0) == 0);
    CHECK(pthread_create(&t2, NULL, accumulate_locally, (void *)1) == 0);
    CHECK(pthread_join(t1, NULL) == 0);
    CHECK(pthread_join(t2, NULL) == 0);
    double local_ms = now_ms() - t0;

    printf("2 threads x %ld increments each (timing-dependent):\n", ITERS);
    printf("  same cache line (false sharing) : %8.1f ms\n", packed_ms);
    printf("  padded to separate lines        : %8.1f ms   (%.1fx faster)\n",
           padded_ms, padded_ms > 0 ? packed_ms / padded_ms : 0.0);
    printf("  local sum, publish once         : %8.1f ms\n", local_ms);
    printf("(sanitizer builds blur the gap; try SAN=0 OPT=1, and on the board)\n");

    CHECK(atomic_load(&g_packed.a) == ITERS && atomic_load(&g_packed.b) == ITERS);
    CHECK(atomic_load(&g_padded.a) == ITERS && atomic_load(&g_padded.b) == ITERS);
    CHECK(local_result[0] == ITERS && local_result[1] == ITERS);
    printf("\n05_false_sharing: all checks passed\n");
    return 0;
}
