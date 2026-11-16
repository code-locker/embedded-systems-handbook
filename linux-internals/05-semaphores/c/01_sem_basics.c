/*
 * 01_sem_basics.c — unnamed POSIX semaphores: init, wait (P), post (V),
 *                   trywait, timedwait, getvalue, destroy; binary semaphore;
 *                   "B waits for A" ordering
 *
 * Concept : A semaphore is a counter that never goes below zero.
 *             sem_wait (P, "proberen", try / take) : if value > 0, value--
 *                                                    else SLEEP until it is
 *             sem_post (V, "verhogen", raise / give): value++, wake a sleeper
 *           Dijkstra (1965). It has NO owner: any thread may post, which is
 *           what makes it a SIGNALLING tool ("A finished, B may go") and a
 *           RESOURCE COUNTER ("3 buffers free") — and what makes it a poor
 *           mutex (see the README).
 * Build   : make run P=linux-internals/05-semaphores/c/01_sem_basics.c
 *           make run P=linux-internals/05-semaphores/c/01_sem_basics.c SAN=tsan
 * Read    : ../README.md  (sections 3.1 to 3.3)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
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

static int value_of(sem_t *s)
{
    int v = -1;
    CHECK(sem_getvalue(s, &v) == 0); /* a snapshot: may be stale at once */
    return v;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* sem_wait can return -1/EINTR if a signal handler runs (without
 * SA_RESTART). Robust code retries. Note: semaphores use errno, unlike
 * pthread_* functions which RETURN the error number. */
static void sem_wait_retry(sem_t *s)
{
    while (sem_wait(s) == -1)
        CHECK(errno == EINTR);
}

/* ------------------------------------------------------------------------
 * Part 1: the counter itself
 *
 *   value:   2 --wait--> 1 --wait--> 0 --wait--> (sleeps) ...
 *                                     ^--post-- wakes it, value stays 0
 *                                               (the post is "consumed")
 * ---------------------------------------------------------------------- */
static void part1_counter(void)
{
    sem_t s;
    /* sem_init(sem, pshared, value): pshared = 0 -> threads of this process
     * only; != 0 -> the sem_t must live in shared memory (see 05/08). */
    CHECK(sem_init(&s, 0, 2) == 0);
    CHECK(value_of(&s) == 2);
    sem_wait_retry(&s);
    sem_wait_retry(&s);
    CHECK(value_of(&s) == 0);

    /* trywait: never sleeps; -1 with errno EAGAIN when the value is 0 */
    errno = 0;
    int rc = sem_trywait(&s);
    printf("[1] init 2, wait, wait -> value 0; trywait -> %d (%s)\n", rc,
           strerror(errno));
    CHECK(rc == -1 && errno == EAGAIN);

    /* timedwait: absolute deadline on CLOCK_REALTIME (same caveat as
     * mutex timedlock: wall-clock jumps change the wait). */
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_nsec += 80 * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    double t0 = now_ms();
    errno = 0;
    rc = sem_timedwait(&s, &dl);
    int e = errno;
    printf("    timedwait(80 ms, REALTIME) -> %d (%s) after %.1f ms\n", rc,
           strerror(e), now_ms() - t0);
    CHECK(rc == -1 && e == ETIMEDOUT);

#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
    /* glibc 2.30+: sem_clockwait lets you use CLOCK_MONOTONIC. (Used on
     * the timeout path only: GCC 11's TSan does not intercept it.) */
    struct timespec mdl;
    clock_gettime(CLOCK_MONOTONIC, &mdl);
    mdl.tv_nsec += 30 * 1000000L;
    if (mdl.tv_nsec >= 1000000000L) {
        mdl.tv_sec++;
        mdl.tv_nsec -= 1000000000L;
    }
    errno = 0;
    rc = sem_clockwait(&s, CLOCK_MONOTONIC, &mdl);
    e = errno;
    printf("    sem_clockwait(30 ms, MONOTONIC) -> %d (%s)\n", rc, strerror(e));
    CHECK(rc == -1 && e == ETIMEDOUT);
#endif
#endif

    CHECK(sem_post(&s) == 0);
    CHECK(value_of(&s) == 1);
    CHECK(sem_trywait(&s) == 0);
    CHECK(sem_destroy(&s) == 0); /* no thread may be waiting on it */
}

/* ------------------------------------------------------------------------
 * Part 2: signalling / ordering — "B must not start step 2 before A has
 * finished step 1". Init value 0 = "not happened yet".
 *
 *    A:  step1 ............ post(done1)
 *    B:  wait(done1) ~~~~~~~~~~~~~~~~~~~^ step2
 *
 * The post may come BEFORE the wait: the semaphore REMEMBERS it (value 1),
 * unlike a condition-variable signal. That is the big practical
 * difference.
 * ---------------------------------------------------------------------- */
typedef struct {
    sem_t done1;
    int log[2]; /* order of events; each slot written by one thread */
    int data;   /* written by A, read by B: ordered by the semaphore */
} Order;

static void *thread_a(void *arg)
{
    Order *o = arg;
    struct timespec t = {0, 20 * 1000000L};
    nanosleep(&t, NULL); /* make A slow, so B really has to wait */
    o->data = 1234;
    o->log[0] = 'A';
    CHECK(sem_post(&o->done1) == 0); /* post = release: data is published */
    return NULL;
}

static void *thread_b(void *arg)
{
    Order *o = arg;
    sem_wait_retry(&o->done1); /* wait = acquire */
    o->log[1] = o->data == 1234 ? 'B' : '?';
    return NULL;
}

static void part2_ordering(void)
{
    Order o = {.data = 0};
    CHECK(sem_init(&o.done1, 0, 0) == 0);
    pthread_t ta, tb;
    CHECK(pthread_create(&tb, NULL, thread_b, &o) == 0); /* B first! */
    CHECK(pthread_create(&ta, NULL, thread_a, &o) == 0);
    CHECK(pthread_join(ta, NULL) == 0);
    CHECK(pthread_join(tb, NULL) == 0);
    printf("[2] ordering with a 0-initialised semaphore: %c then %c "
           "(B saw A's data)\n", o.log[0], o.log[1]);
    CHECK(o.log[0] == 'A' && o.log[1] == 'B');

    /* Post before wait: remembered. */
    CHECK(sem_post(&o.done1) == 0);
    CHECK(sem_trywait(&o.done1) == 0);
    printf("    post BEFORE wait is remembered (value 1): trywait succeeds\n");
    CHECK(sem_destroy(&o.done1) == 0);
}

/* ------------------------------------------------------------------------
 * Part 3: a binary semaphore (0/1) used as a lock. Works, but note: there
 * is no owner, so ANOTHER thread could post it, and there is no priority
 * inheritance. Use a mutex for mutual exclusion.
 * ---------------------------------------------------------------------- */
static sem_t g_bin;
static long g_counter; /* "protected" by g_bin */

static void *bin_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 20000; i++) {
        sem_wait_retry(&g_bin);
        g_counter++;
        CHECK(sem_post(&g_bin) == 0);
    }
    return NULL;
}

static void part3_binary(void)
{
    CHECK(sem_init(&g_bin, 0, 1) == 0); /* 1 = "unlocked" */
    pthread_t t[3];
    for (int i = 0; i < 3; i++)
        CHECK(pthread_create(&t[i], NULL, bin_worker, NULL) == 0);
    for (int i = 0; i < 3; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    printf("[3] binary semaphore as a lock: 3 x 20000 increments = %ld\n",
           g_counter);
    CHECK(g_counter == 60000);
    /* Nothing stops a counting semaphore from going above 1: one extra post
     * by mistake and TWO threads get into the "critical section". */
    CHECK(sem_post(&g_bin) == 0);
    printf("    one stray post -> value %d: the 'lock' now admits 2 threads "
           "(a mutex cannot do that)\n", value_of(&g_bin));
    CHECK(value_of(&g_bin) == 2);
    CHECK(sem_destroy(&g_bin) == 0);
}

int main(void)
{
    part1_counter();
    part2_ordering();
    part3_binary();
    printf("01_sem_basics: all checks passed\n");
    return 0;
}
