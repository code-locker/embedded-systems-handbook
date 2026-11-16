/*
 * 01_racy_counter_bug.c — INTENTIONALLY BROKEN: several threads do
 *                         counter++ on a plain int and on a volatile int
 *
 * Concept : counter++ is three steps — LOAD, ADD, STORE. Two threads can
 *           both load the same old value and both store old+1: one
 *           increment is lost. `volatile` does not help: it only stops the
 *           compiler from caching the value in a register; the three steps
 *           stay three steps and nothing makes them atomic or ordered.
 *
 *     thread A              thread B              counter (memory)
 *     --------              --------              ----------------
 *     LOAD  r=counter                                   5
 *                           LOAD  r=counter             5
 *     ADD   r=r+1 (6)                                   5
 *                           ADD   r=r+1 (6)             5
 *     STORE counter=r                                   6
 *                           STORE counter=r             6   <- should be 7
 *
 * Which tool finds it:
 *   - ThreadSanitizer:
 *       make run P=linux-internals/02-race-conditions-and-atomics/c/01_racy_counter_bug.c SAN=tsan
 *     -> "WARNING: ThreadSanitizer: data race" on `plain_counter++` (and on
 *        `volatile_counter++`), with the stacks of both threads.
 *   - Plain run (default ASan build, or make SAN=0): prints how many
 *     increments were lost. The number changes from run to run and can be 0
 *     on a lucky run — that is exactly why races survive testing. Build with
 *     OPT=1 and the plain counter may even be "right" because the compiler
 *     turned the loop into one big add (legal: a data race is undefined
 *     behaviour, so the compiler may assume no other thread exists).
 *   - The fix is 02_atomic_counter_and_cas.c (atomic_fetch_add).
 * Read    : ../README.md  (sections 3.1 - 3.3)
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define NTHREADS 4
#define ITERS 200000

static int plain_counter;             /* BUG: shared, not atomic, no lock */
static volatile int volatile_counter; /* BUG: volatile is NOT atomic */

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        plain_counter++;    /* data race */
        volatile_counter++; /* data race, just as bad */
    }
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        if (pthread_create(&th[i], NULL, worker, NULL) != 0)
            return 1;
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);

    const int want = NTHREADS * ITERS;
    printf("expected %d\n", want);
    printf("plain    counter = %d  (lost %d increments)\n", plain_counter,
           want - plain_counter);
    printf("volatile counter = %d  (lost %d increments)\n", volatile_counter,
           want - volatile_counter);
    printf("01_racy_counter_bug: finished (run with SAN=tsan to see the report)\n");
    return 0;
}
