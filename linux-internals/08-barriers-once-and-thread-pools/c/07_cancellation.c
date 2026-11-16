/*
 * 07_cancellation.c — pthread_cancel and why it is dangerous with locks:
 *                     cleanup handlers, cancellation points, and the
 *                     cooperative alternative (a stop flag)
 *
 * Concept : pthread_cancel(t) asks thread t to terminate. With the default
 *           (deferred) type, t only dies at a CANCELLATION POINT: a call
 *           like pthread_cond_wait, read, sleep, nanosleep... If t dies
 *           while holding a mutex, the mutex stays locked FOREVER and the
 *           next lock() deadlocks; malloc'ed memory leaks. The defence is
 *           pthread_cleanup_push(): handlers that run during cancellation,
 *           in reverse order, to unlock and free. pthread_cond_wait has a
 *           special rule: when cancelled inside it, the mutex is RE-ACQUIRED
 *           before the cleanup handlers run, so the handler must unlock it.
 *           Most real code avoids cancel entirely and uses a stop flag.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/07_cancellation.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/07_cancellation.c SAN=tsan
 * Read    : ../README.md  (section "Cancellation")
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
 * Sanitizer note: in C (no -fexceptions), glibc implements cancellation of
 * a thread inside pthread_cleanup_push() by unwinding with an internal
 * longjmp. AddressSanitizer does not see that jump, so the red zones it
 * put around the cancelled function's locals stay "poisoned" and ASan
 * aborts later with "AddressSanitizer CHECK failed ... kCurrentStackFrameMagic"
 * when the thread exits. A sanitizer limitation, not a bug in this code:
 * the functions that get cancelled are simply not ASan-instrumented.
 * (ThreadSanitizer handles cancellation fine and still checks them.)
 */
#if defined(__SANITIZE_ADDRESS__)
#define CANCELLABLE __attribute__((no_sanitize_address))
#else
#define CANCELLABLE
#endif

static pthread_mutex_t g_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static bool g_ready;                /* under g_m: never set in part 1 */
static atomic_int g_cleanups;       /* how many cleanup handlers ran */
static atomic_bool g_waiting;       /* worker reached the wait */

static void unlock_cleanup(void *m)
{
    atomic_fetch_add(&g_cleanups, 1);
    pthread_mutex_unlock(m);
}

static void free_cleanup(void *p)
{
    atomic_fetch_add(&g_cleanups, 1);
    free(p);
}

/*
 *   push(free buf) -> push(unlock m) -> lock m -> cond_wait ... CANCEL
 *                                                  (m re-acquired)
 *   handlers run in REVERSE order: unlock m, then free buf -> thread ends
 */
CANCELLABLE static void *waiter(void *arg)
{
    (void)arg;
    char *buf = malloc(4096); /* something that must not leak */
    CHECK(buf != NULL);
    pthread_cleanup_push(free_cleanup, buf);

    CHECK(pthread_mutex_lock(&g_m) == 0);
    pthread_cleanup_push(unlock_cleanup, &g_m);
    atomic_store(&g_waiting, true);
    while (!g_ready)
        pthread_cond_wait(&g_cv, &g_m); /* cancellation point */
    pthread_cleanup_pop(1); /* normal path: run it = unlock */

    pthread_cleanup_pop(1); /* normal path: free buf */
    return NULL;
    /* push/pop are MACROS that open and close a { } block: they must be
     * paired in the same function and the same nesting level. */
}

/* A thread that must not be cancelled in the middle of a multi-step update
 * disables cancellation around it. */
static int g_a, g_b; /* invariant a == b, under g_m */
CANCELLABLE static void *careful(void *arg)
{
    (void)arg;
    for (int i = 0; i < 200; i++) {
        int old;
        pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
        pthread_mutex_lock(&g_m);
        g_a++;
        struct timespec ts = {0, 100000}; /* nanosleep IS a cancellation */
        nanosleep(&ts, NULL);             /* point, but disabled here    */
        g_b++;
        pthread_mutex_unlock(&g_m);
        pthread_setcancelstate(old, NULL);
        pthread_testcancel(); /* explicit, safe place to die */
    }
    return NULL;
}

/* The cooperative alternative: no cancel at all. */
static atomic_bool g_stop;
static void *cooperative(void *arg)
{
    long *iterations = arg;
    while (!atomic_load(&g_stop)) {
        pthread_mutex_lock(&g_m);
        (*iterations)++;
        pthread_mutex_unlock(&g_m);
        struct timespec ts = {0, 50000};
        nanosleep(&ts, NULL);
    }
    return NULL; /* leaves through the front door: nothing to clean up */
}

static void nap_ms(long ms)
{
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

int main(void)
{
    /* 1. Cancel a thread blocked in pthread_cond_wait. */
    pthread_t t;
    CHECK(pthread_create(&t, NULL, waiter, NULL) == 0);
    for (int i = 0; i < 5000 && !atomic_load(&g_waiting); i++)
        nap_ms(1);
    CHECK(atomic_load(&g_waiting));
    CHECK(pthread_cancel(t) == 0);
    void *res;
    CHECK(pthread_join(t, &res) == 0);
    printf("cancelled waiter: result %s, cleanup handlers run: %d\n",
           res == PTHREAD_CANCELED ? "PTHREAD_CANCELED" : "?",
           atomic_load(&g_cleanups));
    CHECK(res == PTHREAD_CANCELED);
    CHECK(atomic_load(&g_cleanups) == 2);
    /* The mutex was released by the handler: we can take it. Without the
     * handler this trylock returns EBUSY and lock() would hang forever. */
    CHECK(pthread_mutex_trylock(&g_m) == 0);
    CHECK(pthread_mutex_unlock(&g_m) == 0);
    printf("mutex is free again after cancellation (cleanup handler "
           "unlocked it)\n");

    /* 2. Cancellation disabled around a critical update. */
    CHECK(pthread_create(&t, NULL, careful, NULL) == 0);
    nap_ms(5);
    CHECK(pthread_cancel(t) == 0);
    CHECK(pthread_join(t, &res) == 0);
    CHECK(pthread_mutex_lock(&g_m) == 0);
    printf("careful thread %s after %d updates; invariant a == b: %s\n",
           res == PTHREAD_CANCELED ? "was cancelled" : "finished", g_a,
           g_a == g_b ? "holds" : "BROKEN");
    CHECK(g_a == g_b); /* never cancelled between a++ and b++ */
    CHECK(pthread_mutex_unlock(&g_m) == 0);

    /* 3. The cooperative way. */
    long iters = 0;
    CHECK(pthread_create(&t, NULL, cooperative, &iters) == 0);
    nap_ms(5);
    atomic_store(&g_stop, true);
    CHECK(pthread_join(t, &res) == 0);
    CHECK(res == NULL);
    printf("cooperative thread stopped by flag after %ld iterations "
           "(timing-dependent)\n", iters);

    printf("07_cancellation: all checks passed\n");
    return 0;
}
