/*
 * 07_sem_from_mutex_condvar.c — build a counting semaphore from a mutex and
 *                               a condition variable
 *
 * Concept : A semaphore is "a counter + wait until counter > 0". That is a
 *           predicate on shared state, so lesson 04 gives it to us at once:
 *             wait: lock; while (count == 0) cond_wait; count--; unlock
 *             post: lock; count++; cond_signal; unlock
 *           This is how you get a semaphore where none exists (C++17,
 *           some RTOS ports), and the clearest way to see that a semaphore
 *           REMEMBERS posts (the count) while a bare condvar does not.
 *           Includes trywait and a monotonic timedwait, then reruns the
 *           resource-pool and ordering tests against it.
 * Build   : make run P=linux-internals/05-semaphores/c/07_sem_from_mutex_condvar.c
 *           make run P=linux-internals/05-semaphores/c/07_sem_from_mutex_condvar.c SAN=tsan
 * Read    : ../README.md  (section 3.9 "Building one yourself")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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

/*
 *   struct Csem { mutex m; cond cv; unsigned count; }
 *
 *   csem_wait:  lock(m)                         csem_post:  lock(m)
 *               while (count == 0)                          count++
 *                   wait(cv, m)   <-------- signal -------- signal(cv)
 *               count--                                     unlock(m)
 *               unlock(m)
 *
 *   signal (not broadcast) is right: one post = one unit = one waiter can
 *   use it. A post with nobody waiting is NOT lost: count remembers it.
 */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv; /* CLOCK_MONOTONIC for timedwait */
    unsigned count;    /* guarded by m */
} Csem;

static void csem_init(Csem *s, unsigned value)
{
    pthread_condattr_t a;
    CHECK(pthread_condattr_init(&a) == 0);
    CHECK(pthread_condattr_setclock(&a, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_mutex_init(&s->m, NULL) == 0);
    CHECK(pthread_cond_init(&s->cv, &a) == 0);
    CHECK(pthread_condattr_destroy(&a) == 0);
    s->count = value;
}

static void csem_destroy(Csem *s)
{
    CHECK(pthread_cond_destroy(&s->cv) == 0);
    CHECK(pthread_mutex_destroy(&s->m) == 0);
}

static void csem_wait(Csem *s)
{
    pthread_mutex_lock(&s->m);
    while (s->count == 0)
        pthread_cond_wait(&s->cv, &s->m);
    s->count--;
    pthread_mutex_unlock(&s->m);
}

static bool csem_trywait(Csem *s)
{
    pthread_mutex_lock(&s->m);
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    pthread_mutex_unlock(&s->m);
    return ok;
}

static bool csem_timedwait_ms(Csem *s, long ms)
{
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->m);
    int rc = 0;
    while (s->count == 0 && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&s->cv, &s->m, &dl);
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    pthread_mutex_unlock(&s->m);
    return ok;
}

static void csem_post(Csem *s)
{
    pthread_mutex_lock(&s->m);
    s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->m);
}

/* ---------------------------------------------------------------------- */
#define POOL 2
#define N_THREADS 6
#define ROUNDS 20

typedef struct {
    Csem *sem;
    atomic_int inside, max_inside;
} PoolTest;

static void *pool_worker(void *arg)
{
    PoolTest *p = arg;
    for (int i = 0; i < ROUNDS; i++) {
        csem_wait(p->sem);
        int now = atomic_fetch_add(&p->inside, 1) + 1;
        int prev = atomic_load(&p->max_inside);
        while (now > prev && !atomic_compare_exchange_weak(&p->max_inside, &prev, now))
            ;
        struct timespec t = {0, 200000L}; /* 0.2 ms of "work" */
        nanosleep(&t, NULL);
        atomic_fetch_sub(&p->inside, 1);
        csem_post(p->sem);
    }
    return NULL;
}

typedef struct {
    Csem *ready;
    int data;
} Handoff;

static void *late_poster(void *arg)
{
    Handoff *h = arg;
    struct timespec t = {0, 20 * 1000000L};
    nanosleep(&t, NULL);
    h->data = 99;
    csem_post(h->ready); /* unlock inside post = release: data published */
    return NULL;
}

int main(void)
{
    /* 1. basic semantics */
    Csem s;
    csem_init(&s, 1);
    CHECK(csem_trywait(&s));
    CHECK(!csem_trywait(&s));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(!csem_timedwait_ms(&s, 50));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
    csem_post(&s);
    csem_post(&s); /* two posts, nobody waiting: both remembered */
    CHECK(csem_trywait(&s) && csem_trywait(&s) && !csem_trywait(&s));
    printf("[1] trywait/timedwait (timed out after %.1f ms)/post: two posts "
           "with no waiter were both remembered\n", ms);
    CHECK(ms >= 49.0);
    csem_destroy(&s);

    /* 2. resource pool: never more than POOL threads inside */
    Csem pool;
    csem_init(&pool, POOL);
    PoolTest pt = {.sem = &pool};
    atomic_init(&pt.inside, 0);
    atomic_init(&pt.max_inside, 0);
    pthread_t th[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_create(&th[i], NULL, pool_worker, &pt) == 0);
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    printf("[2] pool of %d, %d threads x %d rounds: max inside = %d\n", POOL,
           N_THREADS, ROUNDS, atomic_load(&pt.max_inside));
    CHECK(atomic_load(&pt.max_inside) <= POOL);
    CHECK(pool.count == POOL); /* all joined: no lock needed to read */
    csem_destroy(&pool);

    /* 3. ordering: wait before the post happens */
    Csem ready;
    csem_init(&ready, 0);
    Handoff h = {&ready, 0};
    pthread_t tp;
    CHECK(pthread_create(&tp, NULL, late_poster, &h) == 0);
    csem_wait(&ready);
    printf("[3] waited for the poster: data = %d\n", h.data);
    CHECK(h.data == 99);
    CHECK(pthread_join(tp, NULL) == 0);
    csem_destroy(&ready);

    printf("07_sem_from_mutex_condvar: all checks passed\n");
    return 0;
}
