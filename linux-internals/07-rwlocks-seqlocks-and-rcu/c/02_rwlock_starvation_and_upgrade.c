/*
 * 02_rwlock_starvation_and_upgrade.c — the two classic rwlock traps:
 *     (a) writer starvation with the default reader-preferring lock, and
 *         the glibc fix PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
 *     (b) the "upgrade" deadlock: holding a read lock and asking for the
 *         write lock
 *
 * Concept : glibc's default rwlock prefers READERS: a new reader is let in
 *           even while a writer is waiting. With a steady stream of
 *           overlapping readers, the writer may wait forever. A
 *           writer-preferring lock blocks new readers as soon as a writer
 *           queues up. And no POSIX rwlock can be upgraded in place.
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/02_rwlock_starvation_and_upgrade.c
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/02_rwlock_starvation_and_upgrade.c SAN=tsan
 * Read    : ../README.md  (sections "Writer starvation", "The upgrade deadlock")
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

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void nap_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

typedef struct {
    pthread_rwlock_t *rw;
    atomic_bool started, acquired;
} WriterArg;

static void *writer(void *p)
{
    WriterArg *w = p;
    atomic_store(&w->started, true);
    CHECK(pthread_rwlock_wrlock(w->rw) == 0); /* blocks while readers hold it */
    atomic_store(&w->acquired, true);
    CHECK(pthread_rwlock_unlock(w->rw) == 0);
    return NULL;
}

/* A second reader R2 arriving while R1 holds the lock and W is waiting.
 * If it gets in, it HOLDS the read lock until main sets `release`. */
typedef struct {
    pthread_rwlock_t *rw;
    atomic_int rc;       /* -1 = not tried yet, else tryrdlock's result */
    atomic_bool release; /* main -> R2: you may unlock now */
} LateReader;

static void *late_reader(void *p)
{
    LateReader *r = p;
    int rc = pthread_rwlock_tryrdlock(r->rw);
    atomic_store(&r->rc, rc);
    if (rc == 0) {
        double deadline = now_s() + 5.0;
        while (!atomic_load(&r->release) && now_s() < deadline)
            sched_yield();
        CHECK(pthread_rwlock_unlock(r->rw) == 0);
    }
    return NULL;
}

/* Starts R2, waits until it has tried, returns tryrdlock's result. */
static int start_late_reader(LateReader *r, pthread_t *t, pthread_rwlock_t *rw)
{
    r->rw = rw;
    atomic_init(&r->rc, -1);
    atomic_init(&r->release, false);
    CHECK(pthread_create(t, NULL, late_reader, r) == 0);
    double deadline = now_s() + 5.0;
    while (atomic_load(&r->rc) == -1 && now_s() < deadline)
        sched_yield();
    return atomic_load(&r->rc);
}

/*
 * Starvation in miniature (reader-preferring lock):
 *
 *   R1  [=========read=========]
 *   W        wrlock ... waiting ......................... [write]
 *   R2               [=========read=========]       <- let in past W
 *   R3                          [=========read====]  <- and again ...
 *
 *   As long as some reader always holds the lock, W never runs.
 */
static void scenario(const char *name, int kind)
{
    pthread_rwlock_t rw;
    pthread_rwlockattr_t a;
    CHECK(pthread_rwlockattr_init(&a) == 0);
    CHECK(pthread_rwlockattr_setkind_np(&a, kind) == 0);
    CHECK(pthread_rwlock_init(&rw, &a) == 0);
    CHECK(pthread_rwlockattr_destroy(&a) == 0);

    /* R1 = main holds a read lock. */
    CHECK(pthread_rwlock_rdlock(&rw) == 0);

    WriterArg w = {.rw = &rw};
    atomic_init(&w.started, false);
    atomic_init(&w.acquired, false);
    pthread_t tw;
    CHECK(pthread_create(&tw, NULL, writer, &w) == 0);

    /* Wait (bounded) for the writer to be queued. For the writer-preferring
     * lock we can SEE that it has queued: new readers start being refused.
     * For the reader-preferring lock nothing observable changes, so give it
     * a generous moment; correctness of the checks does not depend on it. */
    double deadline = now_s() + 5.0;
    while (!atomic_load(&w.started) && now_s() < deadline)
        sched_yield();
    nap_ms(50);

    LateReader r2;
    pthread_t t2;
    if (kind == PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) {
        /* Retry (bounded) until the writer has visibly queued: from then
         * on, new readers are refused so that the writer goes next. */
        int rc;
        while ((rc = start_late_reader(&r2, &t2, &rw)) == 0) {
            atomic_store(&r2.release, true); /* writer not queued yet */
            CHECK(pthread_join(t2, NULL) == 0);
            CHECK(now_s() < deadline);
            nap_ms(5);
        }
        CHECK(pthread_join(t2, NULL) == 0);
        printf("%-28s: late reader while writer waits -> %s "
               "(writer goes next)\n", name, rc == EBUSY ? "EBUSY" : "?");
        CHECK(rc == EBUSY);
        CHECK(pthread_rwlock_unlock(&rw) == 0); /* R1 leaves: W runs */
    } else {
        int rc = start_late_reader(&r2, &t2, &rw);
        printf("%-28s: late reader while writer waits -> %s "
               "(jumped ahead of the writer)\n", name, rc == 0 ? "ok" : "?");
        CHECK(rc == 0); /* reader-preferring: let in past the waiting W */
        CHECK(pthread_rwlock_unlock(&rw) == 0); /* R1 leaves...           */
        nap_ms(20);
        /* ...but R2 still holds a read lock, so W is STILL waiting.
         * (Deterministic: W cannot hold the write lock while R2 reads.) */
        CHECK(!atomic_load(&w.acquired));
        printf("%-28s: first reader left, writer still starved by the "
               "second\n", name);
        atomic_store(&r2.release, true); /* R2 leaves: W can finally go */
        CHECK(pthread_join(t2, NULL) == 0);
    }
    CHECK(pthread_join(tw, NULL) == 0);
    CHECK(atomic_load(&w.acquired));
    CHECK(pthread_rwlock_destroy(&rw) == 0);
}

int main(void)
{
    scenario("PREFER_READER_NP (default)", PTHREAD_RWLOCK_PREFER_READER_NP);
    scenario("PREFER_WRITER_NONRECURSIVE", PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);

    /*
     * Why "NONRECURSIVE"? With writer preference, a thread that already
     * holds a read lock and takes it AGAIN (recursive read) while a writer
     * waits would block behind the writer, and the writer waits for that
     * very thread: deadlock. You promise not to do that.
     *
     * The upgrade deadlock:
     *
     *   T: rdlock()  ->  (decide to modify)  ->  wrlock()
     *                                              waits for all readers
     *                                              to leave... including T
     *
     * glibc does not detect it (it only detects wrlock after wrlock).
     * Two threads doing this simultaneously deadlock even with a detecting
     * implementation. A timed write lock shows it would block forever:
     */
    pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
    CHECK(pthread_rwlock_rdlock(&rw) == 0);
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_nsec += 50 * 1000000L;
    dl.tv_sec += dl.tv_nsec / 1000000000L;
    dl.tv_nsec %= 1000000000L;
    int rc = pthread_rwlock_clockwrlock(&rw, CLOCK_MONOTONIC, &dl);
    printf("upgrade attempt (rdlock held, then wrlock): %s\n",
           rc == ETIMEDOUT ? "ETIMEDOUT -> a plain wrlock would hang forever"
                           : "unexpected");
    CHECK(rc == ETIMEDOUT);
    /* The fix: drop the read lock, take the write lock, and RE-CHECK the
     * condition (another writer may have changed things in between). */
    CHECK(pthread_rwlock_unlock(&rw) == 0);
    CHECK(pthread_rwlock_wrlock(&rw) == 0);
    /* ... re-validate here ... */
    CHECK(pthread_rwlock_unlock(&rw) == 0);
    CHECK(pthread_rwlock_destroy(&rw) == 0);

    printf("02_rwlock_starvation_and_upgrade: all checks passed\n");
    return 0;
}
