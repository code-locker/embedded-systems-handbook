/*
 * 08_barrier_no_generation_bug.c — INTENTIONALLY BROKEN: a reusable barrier
 *                                  WITHOUT a generation counter
 *
 * Concept : Waiters sleep "while (waiting != 0)", i.e. until the last
 *           arriver resets the count. When the barrier is reused, the
 *           released last arriver can race into the NEXT round and bump
 *           `waiting` to 1 before the slow waiters of the previous round
 *           wake up. They re-check, see waiting != 0, and go back to
 *           sleep. Nobody will ever reset it: deadlock.
 *
 *     round k: T0, T1 sleep; T2 arrives last: waiting = 0, broadcast
 *     T2 runs ahead into round k+1: waiting = 1, sleeps
 *     T0 wakes: waiting (1) != 0 -> sleeps again   <- stuck in round k
 *     T1 wakes: waiting (1) != 0 -> sleeps again   <- stuck in round k
 *     all three asleep forever.
 *
 * Which tool finds it and how:
 *   It is a lost-wakeup/deadlock, not a data race (everything is under the
 *   mutex), so TSan stays quiet. The program finds it itself: main joins
 *   the workers with pthread_timedjoin_np() and a 5 s watchdog; on timeout
 *   it prints "DEADLOCK" plus the round each thread is stuck in, and exits
 *   1. Without the watchdog, run it as `timeout 10 ./prog` and expect exit
 *   status 124, then attach gdb (`gdb -p <pid>`, `thread apply all bt`) to
 *   see every thread inside pthread_cond_wait. It usually deadlocks in the
 *   first few rounds; a run that survives is just lucky timing.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/08_barrier_no_generation_bug.c
 * Read    : ../README.md  (section "A reusable barrier: why a generation counter")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NTHREADS 3
#define ROUNDS 100000

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned waiting; /* under m */
static atomic_int round_of[NTHREADS];

static void broken_barrier_wait(void)
{
    pthread_mutex_lock(&m);
    if (++waiting == NTHREADS) {
        waiting = 0;
        pthread_cond_broadcast(&cv);
    } else {
        while (waiting != 0) /* BUG: "count reset" is not a stable signal */
            pthread_cond_wait(&cv, &m);
    }
    pthread_mutex_unlock(&m);
}

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    for (int r = 0; r < ROUNDS; r++) {
        atomic_store(&round_of[id], r);
        broken_barrier_wait();
    }
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; i++)
        pthread_create(&th[i], NULL, worker, (void *)i);

    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 5;
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_timedjoin_np(th[i], NULL, &dl) == ETIMEDOUT) {
            printf("DEADLOCK: barrier did not complete within 5 s; rounds:");
            for (int k = 0; k < NTHREADS; k++)
                printf(" T%d=%d", k, atomic_load(&round_of[k]));
            printf("\n08_barrier_no_generation_bug: BUG CAUGHT\n");
            exit(1); /* threads are stuck: exit without joining them */
        }
    }
    printf("08_barrier_no_generation_bug: survived %d rounds (lucky timing; "
           "the bug is still there)\n", ROUNDS);
    return 0;
}
