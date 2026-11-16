/*
 * 05_loop_var_arg_bug.c — INTENTIONALLY BROKEN: passing &i (the loop
 *                         variable) as the thread argument
 *
 * Concept : pthread_create() does not copy what the argument points to. All
 *           threads receive the SAME address &i, and by the time a thread
 *           reads *arg the loop has moved on (or finished and the variable's
 *           scope has ended). Two bugs in one line:
 *             1. data race: main writes i (i++) while a thread reads it
 *             2. use after scope: `i` is dead after the loop, threads still
 *                read through the pointer
 *
 *     main stack                      threads
 *     +-------+
 *     |  i    | <──── &i ─────────── thread 0 (expected 0)
 *     +-------+ <──── &i ─────────── thread 1 (expected 1)
 *                <─── &i ─────────── thread 2 (expected 2)   all the same!
 *
 * Which tool finds it:
 *   - AddressSanitizer (the default build):
 *       make run P=linux-internals/01-processes-and-threads/c/05_loop_var_arg_bug.c
 *     -> "ERROR: AddressSanitizer: stack-use-after-scope" in worker()
 *        (the threads wait a few ms, so they read after the loop ended)
 *   - ThreadSanitizer:
 *       make run P=linux-internals/01-processes-and-threads/c/05_loop_var_arg_bug.c SAN=tsan
 *     -> "WARNING: ThreadSanitizer: data race" between the write `i++` in
 *        main and the read `*(int *)arg` in worker
 *   - Without sanitizers (make SAN=0) it "works" and prints wrong ids,
 *     or the right ones by luck: the worst kind of bug.
 *
 * Fix     : give each thread its own storage that outlives it
 *           (int ids[N]; pass &ids[i]), or pass the value itself:
 *           (void *)(intptr_t)i  — see 02_pthread_create_join.c and
 *           03_tls_and_cancellation.c.
 * Read    : ../README.md  (section 3.5 and "Common mistakes")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define NTHREADS 4

static int seen[NTHREADS];

static void *worker(void *arg)
{
    struct timespec ts = {0, 5 * 1000000L}; /* 5 ms: let the loop finish */
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
    int my_id = *(int *)arg; /* BUG: reads main's loop variable */
    printf("thread got id %d\n", my_id);
    if (my_id >= 0 && my_id < NTHREADS)
        seen[my_id]++;
    return NULL;
}

int main(void)
{
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        if (pthread_create(&th[i], NULL, worker, &i) != 0) /* BUG: &i */
            return 1;
    }
    /* `i` no longer exists here, but the threads still hold &i */
    for (int k = 0; k < NTHREADS; k++)
        pthread_join(th[k], NULL);

    int ok = 1;
    for (int k = 0; k < NTHREADS; k++)
        if (seen[k] != 1)
            ok = 0;
    printf("each id seen exactly once: %s\n", ok ? "yes (by luck)" : "NO");
    printf("05_loop_var_arg_bug: finished (this program is meant to be caught by"
           " ASan/TSan)\n");
    return ok ? 0 : 1;
}
