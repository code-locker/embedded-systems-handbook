/*
 * 01_condvar_basics.c — condition variables: the wait loop, a one-shot
 *                       event (start gate) and a countdown latch
 *
 * Concept : A condition variable lets a thread SLEEP until a predicate on
 *           shared data becomes true, instead of spinning on it. It holds no
 *           state of its own: the state is your variables, protected by a
 *           mutex. The canonical shape, which never changes:
 *
 *             waiter:                         signaller:
 *               lock(m)                         lock(m)
 *               while (!predicate)              change state (pred = true)
 *                   wait(cv, m)                 signal(cv) / broadcast(cv)
 *               use the state                   unlock(m)
 *               unlock(m)
 * Build   : make run P=linux-internals/04-condition-variables/c/01_condvar_basics.c
 *           make run P=linux-internals/04-condition-variables/c/01_condvar_basics.c SAN=tsan
 * Read    : ../README.md  (sections 3.1 to 3.3, 3.6)
 */
#define _GNU_SOURCE
#include <pthread.h>
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

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

/* ========================================================================
 * Part 1: hand a result from a worker to main
 *
 * Why the MUTEX is passed to wait():  wait() must do
 *     "unlock m  AND  go to sleep on cv"   as ONE atomic step.
 *
 *   If it were two steps, the signal could slip in between:
 *     waiter                         signaller
 *     lock; ready? no
 *     unlock                 <-- gap -->  lock; ready = 1; signal; unlock
 *     sleep on cv   ...forever: the signal went to nobody (LOST WAKEUP)
 *
 *   pthread_cond_wait closes the gap: the waiter is already queued on cv
 *   when m is released, so a signaller who then takes m and signals is
 *   guaranteed to find it. On wake-up, wait() re-locks m BEFORE returning,
 *   so the predicate is re-checked under the lock.
 *
 *     waiter:   lock --- check: false --- [enqueue on cv + unlock] ~~sleep~~
 *     signaller:                               lock -- ready=1 -- signal -- unlock
 *     waiter:                                                ~~wake~~ [relock] -- check: true
 * ====================================================================== */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int ready;  /* the predicate; guarded by m */
    long value; /* the payload;   guarded by m */
} Handoff;

static void *producer(void *arg)
{
    Handoff *h = arg;
    sleep_ms(30); /* "compute" something */
    pthread_mutex_lock(&h->m);
    h->value = 42;
    h->ready = 1;               /* 1. change the state under the lock  */
    pthread_cond_signal(&h->cv); /* 2. then signal (holding m is fine)  */
    pthread_mutex_unlock(&h->m);
    return NULL;
}

static void part1_handoff(void)
{
    Handoff h = {.ready = 0};
    CHECK(pthread_mutex_init(&h.m, NULL) == 0);
    CHECK(pthread_cond_init(&h.cv, NULL) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, producer, &h) == 0);

    int wakeups = 0;
    pthread_mutex_lock(&h.m);
    /* WHILE, not if: (1) spurious wake-ups are allowed by POSIX, (2) with
     * several waiters, another one may have consumed the state first,
     * (3) a broadcast wakes you for a predicate that may not be yours. */
    while (!h.ready) {
        pthread_cond_wait(&h.cv, &h.m);
        wakeups++;
    }
    long v = h.value;
    pthread_mutex_unlock(&h.m);
    CHECK(pthread_join(t, NULL) == 0);
    printf("[1] main slept until ready: value=%ld after %d wake-up(s), "
           "0 CPU spent spinning\n", v, wakeups);
    CHECK(v == 42);
    CHECK(wakeups >= 1);
    CHECK(pthread_cond_destroy(&h.cv) == 0);
    CHECK(pthread_mutex_destroy(&h.m) == 0);
}

/* ========================================================================
 * Part 2: countdown latch = "wait until N things happened". With N = 1 it
 * is a one-shot EVENT (a start gate): once opened it stays open, so a
 * thread arriving late does not wait at all — unlike a bare signal, which
 * is forgotten if nobody is waiting at that moment.
 *
 *   count: 3 --count_down--> 2 --count_down--> 1 --count_down--> 0
 *                                                                 |
 *                                                  broadcast: all waiters go
 * ====================================================================== */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    int count; /* guarded by m */
} Latch;

static void latch_init(Latch *l, int n)
{
    CHECK(pthread_mutex_init(&l->m, NULL) == 0);
    CHECK(pthread_cond_init(&l->cv, NULL) == 0);
    l->count = n;
}

static void latch_count_down(Latch *l)
{
    pthread_mutex_lock(&l->m);
    if (l->count > 0 && --l->count == 0)
        pthread_cond_broadcast(&l->cv); /* EVERY waiter's predicate is true */
    pthread_mutex_unlock(&l->m);
}

static void latch_wait(Latch *l)
{
    pthread_mutex_lock(&l->m);
    while (l->count > 0)
        pthread_cond_wait(&l->cv, &l->m);
    pthread_mutex_unlock(&l->m);
}

static void latch_destroy(Latch *l)
{
    CHECK(pthread_cond_destroy(&l->cv) == 0);
    CHECK(pthread_mutex_destroy(&l->m) == 0);
}

#define N_WORKERS 4
#define N_WAITERS 3

typedef struct {
    Latch *start_gate; /* count 1: one-shot event */
    Latch *done;       /* count N_WORKERS */
    int *results;
    int id;
    int saw_all; /* for waiters */
} Ctx;

static void *worker(void *arg)
{
    Ctx *c = arg;
    latch_wait(c->start_gate); /* nobody starts before main says "go" */
    c->results[c->id] = (c->id + 1) * 100;
    latch_count_down(c->done); /* unlock inside = release: result visible */
    return NULL;
}

static void *result_waiter(void *arg)
{
    Ctx *c = arg;
    latch_wait(c->done);
    c->saw_all = 1;
    for (int i = 0; i < N_WORKERS; i++)
        if (c->results[i] != (i + 1) * 100)
            c->saw_all = 0;
    return NULL;
}

static void part2_latch(void)
{
    Latch gate, done;
    latch_init(&gate, 1);
    latch_init(&done, N_WORKERS);
    int results[N_WORKERS] = {0};
    pthread_t tw[N_WORKERS], tr[N_WAITERS];
    Ctx cw[N_WORKERS], cr[N_WAITERS];

    for (int i = 0; i < N_WAITERS; i++) {
        cr[i] = (Ctx){&gate, &done, results, i, 0};
        CHECK(pthread_create(&tr[i], NULL, result_waiter, &cr[i]) == 0);
    }
    for (int i = 0; i < N_WORKERS; i++) {
        cw[i] = (Ctx){&gate, &done, results, i, 0};
        CHECK(pthread_create(&tw[i], NULL, worker, &cw[i]) == 0);
    }
    sleep_ms(20); /* give workers time to block on the gate (not required) */
    pthread_mutex_lock(&done.m);
    int still = done.count;
    pthread_mutex_unlock(&done.m);
    CHECK(still == N_WORKERS); /* gate closed: no worker can have finished */

    latch_count_down(&gate); /* open the gate: one broadcast starts all */

    for (int i = 0; i < N_WORKERS; i++)
        CHECK(pthread_join(tw[i], NULL) == 0);
    for (int i = 0; i < N_WAITERS; i++) {
        CHECK(pthread_join(tr[i], NULL) == 0);
        CHECK(cr[i].saw_all);
    }
    /* A late arrival on an open gate does not block at all. */
    latch_wait(&gate);
    printf("[2] start gate (latch of 1) released %d workers at once; "
           "latch of %d released %d waiters, each saw all results; a late "
           "latch_wait() returned at once\n", N_WORKERS, N_WORKERS, N_WAITERS);
    latch_destroy(&gate);
    latch_destroy(&done);
}

int main(void)
{
    part1_handoff();
    part2_latch();
    printf("01_condvar_basics: all checks passed\n");
    return 0;
}
