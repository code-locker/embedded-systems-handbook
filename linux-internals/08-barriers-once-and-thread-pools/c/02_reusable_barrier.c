/*
 * 02_reusable_barrier.c — a barrier built from a mutex and a condition
 *                         variable, reusable round after round thanks to a
 *                         GENERATION counter
 *
 * Concept : The obvious design ("count arrivals; the last one broadcasts;
 *           the others wait until the count is reset") breaks when the
 *           barrier is REUSED: a fast thread released from round k can
 *           re-enter for round k+1 and bump the count before a slow thread
 *           from round k has even woken up. The slow thread then sees a
 *           non-zero count, thinks round k is not over, and waits forever.
 *           The fix: waiters wait for the GENERATION number to change, a
 *           value that only ever moves forward. (See 08_barrier_no_generation_bug.c
 *           for the broken version, caught by a watchdog.)
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/02_reusable_barrier.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/02_reusable_barrier.c SAN=tsan
 * Read    : ../README.md  (section "A reusable barrier: why a generation counter")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define NTHREADS 4
#define ROUNDS 2000

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv;
    unsigned count;      /* threads per round (fixed)                  */
    unsigned waiting;    /* arrived in the current round   (under m)   */
    unsigned long gen;   /* round number, only increases   (under m)   */
} Barrier;

static void barrier_init(Barrier *b, unsigned count)
{
    CHECK(pthread_mutex_init(&b->m, NULL) == 0);
    CHECK(pthread_cond_init(&b->cv, NULL) == 0);
    b->count = count;
    b->waiting = 0;
    b->gen = 0;
}

static void barrier_destroy(Barrier *b)
{
    CHECK(pthread_cond_destroy(&b->cv) == 0);
    CHECK(pthread_mutex_destroy(&b->m) == 0);
}

/*
 *   round k (gen = k)                          round k+1 (gen = k+1)
 *   T0 arrives: waiting=1, sleeps until gen != k
 *   T1 arrives: waiting=2, sleeps until gen != k
 *   T2 arrives: waiting=3 == count  -> gen = k+1, waiting = 0, broadcast
 *   T2 returns, races ahead:                   T2 arrives: waiting=1, sleeps
 *   T0, T1 wake: gen (k+1) != k  -> return     (with the broken "wait until
 *                                               waiting == 0" they would see
 *                                               1 and sleep forever)
 *
 * Returns true in exactly one thread per round (like
 * PTHREAD_BARRIER_SERIAL_THREAD): the last one to arrive.
 */
static bool barrier_wait(Barrier *b)
{
    CHECK(pthread_mutex_lock(&b->m) == 0);
    unsigned long my_gen = b->gen;
    if (++b->waiting == b->count) {
        b->gen++;        /* open the gate for this round...           */
        b->waiting = 0;  /* ...and reset for the next one immediately */
        CHECK(pthread_cond_broadcast(&b->cv) == 0);
        CHECK(pthread_mutex_unlock(&b->m) == 0);
        return true;
    }
    while (b->gen == my_gen) /* loop: spurious wakeups (lesson 04) */
        CHECK(pthread_cond_wait(&b->cv, &b->m) == 0);
    CHECK(pthread_mutex_unlock(&b->m) == 0);
    return false;
}

/* ---- test: every thread marks each round; after the barrier, everyone
 *      must see all NTHREADS marks of that round. ---------------------- */
static Barrier g_bar;
static atomic_int g_marks[ROUNDS];
static atomic_int g_serial[ROUNDS];
static atomic_long g_violations;

static void *worker(void *arg)
{
    (void)arg;
    for (int r = 0; r < ROUNDS; r++) {
        atomic_fetch_add(&g_marks[r], 1);
        if (barrier_wait(&g_bar))
            atomic_fetch_add(&g_serial[r], 1);
        /* After the barrier, round r is complete for everybody... */
        if (atomic_load(&g_marks[r]) != NTHREADS)
            atomic_fetch_add(&g_violations, 1);
        /* ...and nobody can be more than one round ahead of us. */
        if (r + 2 < ROUNDS && atomic_load(&g_marks[r + 2]) != 0)
            atomic_fetch_add(&g_violations, 1);
    }
    return NULL;
}

int main(void)
{
    barrier_init(&g_bar, NTHREADS);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    int serial_ok = 1;
    for (int r = 0; r < ROUNDS; r++)
        if (atomic_load(&g_serial[r]) != 1)
            serial_ok = 0;
    printf("%d threads x %d rounds: generation %lu, violations %ld, exactly "
           "one 'serial' thread per round: %s\n", NTHREADS, ROUNDS, g_bar.gen,
           atomic_load(&g_violations), serial_ok ? "yes" : "NO");
    CHECK(g_bar.gen == ROUNDS);
    CHECK(atomic_load(&g_violations) == 0);
    CHECK(serial_ok);
    CHECK(g_bar.waiting == 0);

    barrier_destroy(&g_bar);
    printf("02_reusable_barrier: all checks passed\n");
    return 0;
}
