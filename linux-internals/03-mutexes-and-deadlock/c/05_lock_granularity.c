/*
 * 05_lock_granularity.c — coarse lock vs lock striping vs one lock per bucket
 *
 * Concept : The same hash-table counter protected three ways:
 *             COARSE  : one mutex for the whole table (simple, serialises all)
 *             STRIPED : S mutexes, bucket b uses lock b % S (good trade-off)
 *             FINE    : one mutex per bucket (most parallel, most memory)
 *           All three must give identical, exact results. Timings are only
 *           printed: they depend on cores, contention and sanitizers.
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/05_lock_granularity.c
 *           make run P=linux-internals/03-mutexes-and-deadlock/c/05_lock_granularity.c SAN=tsan
 *           (try OPT=1 SAN=0 for meaningful timings)
 * Read    : ../README.md  (section 3.7 "Lock granularity")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define N_BUCKETS 64
#define N_STRIPES 8
#define N_THREADS 4
#define N_OPS 20000

/*
 *  COARSE                 STRIPED (S = 8)                 FINE
 *  +------+               lock0  lock1 ...  lock7         lock per bucket
 *  | lock |               |  \    |  \       |  \         [L|b0][L|b1][L|b2]...
 *  +------+               b0  b8  b1  b9 ... b7  b15 ...
 *  b0 b1 b2 ... b63       bucket b -> locks[b % 8]
 *
 *  Two threads touching b3 and b40 : COARSE blocks, STRIPED/FINE do not
 *  Two threads touching b3 and b11 : COARSE and STRIPED block, FINE does not
 *
 * Java's old ConcurrentHashMap used 16 stripes; the Linux dcache and many
 * kernel hash tables use a lock (or bit-spinlock) per bucket.
 */
enum Mode { COARSE, STRIPED, FINE };

typedef struct {
    long count[N_BUCKETS];
    pthread_mutex_t global;
    pthread_mutex_t stripe[N_STRIPES];
    pthread_mutex_t bucket[N_BUCKETS];
    enum Mode mode;
} Table;

static pthread_mutex_t *lock_for(Table *t, unsigned b)
{
    switch (t->mode) {
    case COARSE: return &t->global;
    case STRIPED: return &t->stripe[b % N_STRIPES];
    default: return &t->bucket[b];
    }
}

/* Deterministic per-thread key stream (xorshift32), so the expected counts
 * can be computed single-threaded and compared exactly. */
static uint32_t next_key(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x % N_BUCKETS;
}

typedef struct {
    Table *t;
    uint32_t seed;
} Arg;

static void *worker(void *p)
{
    Arg *a = p;
    uint32_t s = a->seed;
    for (int i = 0; i < N_OPS; i++) {
        unsigned b = next_key(&s);
        pthread_mutex_t *m = lock_for(a->t, b);
        pthread_mutex_lock(m);
        a->t->count[b]++; /* the only shared write; covered by m */
        pthread_mutex_unlock(m);
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void run(Table *t, enum Mode mode, const long *expect)
{
    static const char *names[] = {"coarse (1 lock)  ", "striped (8 locks)",
                                  "fine (64 locks)  "};
    memset(t->count, 0, sizeof t->count);
    t->mode = mode;
    pthread_t th[N_THREADS];
    Arg args[N_THREADS];
    double t0 = now_ms();
    for (int i = 0; i < N_THREADS; i++) {
        args[i] = (Arg){t, 0x9E3779B9u * (uint32_t)(i + 1)};
        CHECK(pthread_create(&th[i], NULL, worker, &args[i]) == 0);
    }
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    double ms = now_ms() - t0;
    long total = 0;
    for (int b = 0; b < N_BUCKETS; b++) {
        CHECK(t->count[b] == expect[b]); /* exact, bucket by bucket */
        total += t->count[b];
    }
    printf("    %s: %ld increments, all %d buckets exact, %.1f ms\n", names[mode],
           total, N_BUCKETS, ms);
    CHECK(total == (long)N_THREADS * N_OPS);
}

int main(void)
{
    /* Ownership: the table lives on the heap (it is ~5 KB of mutexes),
     * main allocates and frees it after all threads are joined. */
    Table *t = calloc(1, sizeof *t);
    CHECK(t != NULL);
    CHECK(pthread_mutex_init(&t->global, NULL) == 0);
    for (int i = 0; i < N_STRIPES; i++)
        CHECK(pthread_mutex_init(&t->stripe[i], NULL) == 0);
    for (int i = 0; i < N_BUCKETS; i++)
        CHECK(pthread_mutex_init(&t->bucket[i], NULL) == 0);

    /* Expected result, computed with no threads at all. */
    long expect[N_BUCKETS] = {0};
    for (int i = 0; i < N_THREADS; i++) {
        uint32_t s = 0x9E3779B9u * (uint32_t)(i + 1);
        for (int k = 0; k < N_OPS; k++)
            expect[next_key(&s)]++;
    }

    printf("[1] %d threads x %d increments on a %d-bucket table\n", N_THREADS,
           N_OPS, N_BUCKETS);
    run(t, COARSE, expect);
    run(t, STRIPED, expect);
    run(t, FINE, expect);
    printf("    (timings are illustrative only; with 1 lock all threads "
           "queue on one cache line)\n");
    printf("    memory for locks: coarse %zu B, striped %zu B, fine %zu B\n",
           sizeof(pthread_mutex_t), N_STRIPES * sizeof(pthread_mutex_t),
           N_BUCKETS * sizeof(pthread_mutex_t));

    /* A whole-table operation (resize, snapshot) needs EVERY lock. With
     * stripes you take them all in index order (0..S-1) — a fixed order,
     * so it cannot deadlock against another whole-table operation. */
    long snapshot = 0;
    for (int i = 0; i < N_STRIPES; i++)
        pthread_mutex_lock(&t->stripe[i]);
    for (int b = 0; b < N_BUCKETS; b++)
        snapshot += t->count[b];
    for (int i = N_STRIPES - 1; i >= 0; i--)
        pthread_mutex_unlock(&t->stripe[i]);
    printf("[2] consistent snapshot under all %d stripe locks: total %ld\n",
           N_STRIPES, snapshot);
    CHECK(snapshot == (long)N_THREADS * N_OPS);

    for (int i = 0; i < N_BUCKETS; i++)
        CHECK(pthread_mutex_destroy(&t->bucket[i]) == 0);
    for (int i = 0; i < N_STRIPES; i++)
        CHECK(pthread_mutex_destroy(&t->stripe[i]) == 0);
    CHECK(pthread_mutex_destroy(&t->global) == 0);
    free(t);
    printf("05_lock_granularity: all checks passed\n");
    return 0;
}
