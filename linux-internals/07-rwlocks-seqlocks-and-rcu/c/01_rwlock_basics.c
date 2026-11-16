/*
 * 01_rwlock_basics.c — pthread_rwlock_t: many readers OR one writer;
 *                      rdlock/wrlock, try and timed variants, and a proof
 *                      that readers really run concurrently
 *
 * Concept : Most shared data is read far more often than written (config,
 *           routing tables, caches). A mutex serialises the readers for no
 *           reason. A read-write lock lets any number of readers in at the
 *           same time and gives a writer exclusive access.
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/01_rwlock_basics.c
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/01_rwlock_basics.c SAN=tsan
 * Read    : ../README.md  (section "pthread_rwlock_t")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define NREADERS 4

/*
 *   time ->
 *   R1  [==== read ====]
 *   R2     [==== read ====]          readers overlap freely
 *   R3        [==== read ====]
 *   W                       ...wait...[== write ==]   writer alone
 *   R4                                  ...wait...  [== read ==]
 */
static pthread_rwlock_t g_rw = PTHREAD_RWLOCK_INITIALIZER;

/* ---- 1. prove concurrency: every reader waits INSIDE the read lock until
 *         all NREADERS are inside. With a mutex this would deadlock (the
 *         bounded wait would time out); with an rwlock it completes. ---- */
static atomic_int g_inside, g_max_inside;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *concurrent_reader(void *arg)
{
    (void)arg;
    CHECK(pthread_rwlock_rdlock(&g_rw) == 0);
    int in = atomic_fetch_add(&g_inside, 1) + 1;
    int m = atomic_load(&g_max_inside);
    while (in > m && !atomic_compare_exchange_weak(&g_max_inside, &m, in))
        ;
    /* Wait on g_max_inside, not g_inside: g_inside drops again as soon as
     * the first reader leaves, and the others would then wait in vain. */
    double deadline = now_s() + 5.0; /* never hang, even if broken */
    while (atomic_load(&g_max_inside) < NREADERS && now_s() < deadline)
        sched_yield();
    atomic_fetch_sub(&g_inside, 1);
    CHECK(pthread_rwlock_unlock(&g_rw) == 0);
    return NULL;
}

/* ---- 2. try / timed variants, tested from a second thread ---------------- */
typedef struct { int try_rd, try_wr, timed_rd, timed_wr; } Probe;

static struct timespec abs_realtime_in_ms(long ms)
{
    /* pthread_rwlock_timed*lock take an ABSOLUTE CLOCK_REALTIME deadline
     * (so a wall-clock jump changes the wait). glibc >= 2.30 also has
     * pthread_rwlock_clockrdlock/clockwrlock with CLOCK_MONOTONIC. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return ts;
}

static void *prober(void *arg)
{
    Probe *p = arg;
    p->try_rd = pthread_rwlock_tryrdlock(&g_rw);
    if (p->try_rd == 0)
        pthread_rwlock_unlock(&g_rw);
    p->try_wr = pthread_rwlock_trywrlock(&g_rw);
    if (p->try_wr == 0)
        pthread_rwlock_unlock(&g_rw);
    struct timespec dl = abs_realtime_in_ms(30);
    p->timed_rd = pthread_rwlock_timedrdlock(&g_rw, &dl);
    if (p->timed_rd == 0)
        pthread_rwlock_unlock(&g_rw);
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    now.tv_nsec += 30 * 1000000L;
    now.tv_sec += now.tv_nsec / 1000000000L;
    now.tv_nsec %= 1000000000L;
    p->timed_wr = pthread_rwlock_clockwrlock(&g_rw, CLOCK_MONOTONIC, &now);
    if (p->timed_wr == 0)
        pthread_rwlock_unlock(&g_rw);
    return NULL;
}

static Probe probe(void)
{
    Probe p;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, prober, &p) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    return p;
}

static const char *err(int e)
{
    return e == 0 ? "ok" : e == EBUSY ? "EBUSY" : e == ETIMEDOUT ? "ETIMEDOUT" : "?";
}

/* ---- 3. stress: invariant a + b == 1000 must hold for every reader ------- */
static int g_a = 1000, g_b = 0; /* protected by g_rw */
static atomic_long g_reads, g_bad;

static void *stress_reader(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        CHECK(pthread_rwlock_rdlock(&g_rw) == 0);
        if (g_a + g_b != 1000)
            atomic_fetch_add(&g_bad, 1);
        CHECK(pthread_rwlock_unlock(&g_rw) == 0);
        atomic_fetch_add_explicit(&g_reads, 1, memory_order_relaxed);
    }
    return NULL;
}

static void *stress_writer(void *arg)
{
    (void)arg;
    for (int i = 0; i < 2000; i++) {
        CHECK(pthread_rwlock_wrlock(&g_rw) == 0);
        g_a--; /* between these two lines the invariant is broken: */
        g_b++; /* no reader may see it, the write lock guarantees that */
        CHECK(pthread_rwlock_unlock(&g_rw) == 0);
    }
    return NULL;
}

int main(void)
{
    /* 1 */
    pthread_t th[NREADERS];
    for (int i = 0; i < NREADERS; i++)
        CHECK(pthread_create(&th[i], NULL, concurrent_reader, NULL) == 0);
    for (int i = 0; i < NREADERS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    printf("max readers inside the read lock at the same time: %d of %d\n",
           atomic_load(&g_max_inside), NREADERS);
    CHECK(atomic_load(&g_max_inside) == NREADERS);

    /* 2a. A reader holds it: other readers get in, writers do not. */
    CHECK(pthread_rwlock_rdlock(&g_rw) == 0);
    Probe p = probe();
    printf("while READ-locked : tryrd %s, trywr %s, timedrd %s, clockwr %s\n",
           err(p.try_rd), err(p.try_wr), err(p.timed_rd), err(p.timed_wr));
    CHECK(p.try_rd == 0 && p.try_wr == EBUSY);
    CHECK(p.timed_rd == 0 && p.timed_wr == ETIMEDOUT);
    CHECK(pthread_rwlock_unlock(&g_rw) == 0);

    /* 2b. A writer holds it: nobody else gets in. */
    CHECK(pthread_rwlock_wrlock(&g_rw) == 0);
    p = probe();
    printf("while WRITE-locked: tryrd %s, trywr %s, timedrd %s, clockwr %s\n",
           err(p.try_rd), err(p.try_wr), err(p.timed_rd), err(p.timed_wr));
    CHECK(p.try_rd == EBUSY && p.try_wr == EBUSY);
    CHECK(p.timed_rd == ETIMEDOUT && p.timed_wr == ETIMEDOUT);
    /* glibc detects "write-lock it again from the owner thread": */
    CHECK(pthread_rwlock_wrlock(&g_rw) == EDEADLK);
    CHECK(pthread_rwlock_unlock(&g_rw) == 0);

    /* 2c. Free: everything succeeds. */
    p = probe();
    CHECK(p.try_rd == 0 && p.try_wr == 0 && p.timed_rd == 0 && p.timed_wr == 0);

    /* 3 */
    pthread_t r[3], w[2];
    for (int i = 0; i < 3; i++)
        CHECK(pthread_create(&r[i], NULL, stress_reader, NULL) == 0);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_create(&w[i], NULL, stress_writer, NULL) == 0);
    for (int i = 0; i < 3; i++)
        CHECK(pthread_join(r[i], NULL) == 0);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(w[i], NULL) == 0);
    printf("stress: %ld reads, %ld saw a broken invariant; a=%d b=%d\n",
           atomic_load(&g_reads), atomic_load(&g_bad), g_a, g_b);
    CHECK(atomic_load(&g_bad) == 0);
    CHECK(g_a == 1000 - 4000 && g_b == 4000);

    CHECK(pthread_rwlock_destroy(&g_rw) == 0);
    printf("01_rwlock_basics: all checks passed\n");
    return 0;
}
