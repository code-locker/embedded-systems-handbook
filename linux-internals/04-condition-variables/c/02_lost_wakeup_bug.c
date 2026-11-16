/*
 * 02_lost_wakeup_bug.c — INTENTIONALLY BROKEN: the lost wake-up
 *
 * Concept : The waiter checks the predicate WITHOUT holding the mutex, and
 *           only then locks and waits. The signaller sets the flag and
 *           signals in the gap between the check and the wait. A condition
 *           variable has no memory: a signal with nobody waiting is simply
 *           dropped. The waiter then sleeps although `ready` is already 1.
 *
 *             waiter                             signaller
 *             if (!ready)       reads 0
 *               ... gap (here: 200 ms) ...       lock; ready = 1;
 *                                                signal -> nobody waits, LOST
 *                                                unlock
 *               lock; wait(cv)  sleeps for good: nobody will signal again
 *
 *           The gap is widened to 200 ms so this happens on EVERY run. In
 *           real code it is a few nanoseconds and the bug shows up once a
 *           week, on the board, under load: a thread that "hangs for no
 *           reason" with its condition already true.
 *
 *           A second classic form of the same bug: the signaller changes the
 *           state WITHOUT the mutex ("ready = 1; signal(cv);"): the waiter
 *           can check ready==0 under the lock, then the signaller writes
 *           and signals before the waiter is queued on cv -> lost again.
 *
 * Find it :
 *   1. Visible misbehaviour: the wait here has a 2 s safety timeout (real
 *      code would hang forever). The program prints "LOST WAKE-UP" and
 *      exits 1:      make run P=linux-internals/04-condition-variables/c/02_lost_wakeup_bug.c
 *   2. ThreadSanitizer: the unlocked read of `ready` is a data race against
 *      the locked write; TSan reports it with both stacks:
 *        TSAN_OPTIONS=halt_on_error=1 make run P=linux-internals/04-condition-variables/c/02_lost_wakeup_bug.c SAN=tsan
 *   3. On a real hang: `gdb -p <pid>`, `thread apply all bt` shows the
 *      thread in pthread_cond_wait, `print ready` shows 1. That combination
 *      (predicate true, still waiting) is the fingerprint of a lost wake-up.
 * Fix     : check the predicate under the lock, in a while loop:
 *             lock(m); while (!ready) wait(cv, m); unlock(m);
 *           and always change the state under the same mutex.
 * Build   : make run P=linux-internals/04-condition-variables/c/02_lost_wakeup_bug.c
 * Read    : ../README.md  (section 3.4 "Lost wake-ups")
 */
#define _GNU_SOURCE
#include <errno.h>
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

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv; /* initialised with CLOCK_MONOTONIC in main */
static int ready;         /* "guarded" by m ... except where the bug is */

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static void *signaller(void *arg)
{
    (void)arg;
    sleep_ms(50); /* lands inside the waiter's gap */
    pthread_mutex_lock(&m);
    ready = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&m);
    printf("signaller: ready = 1, signalled\n");
    fflush(stdout);
    return NULL;
}

int main(void)
{
    pthread_condattr_t ca;
    CHECK(pthread_condattr_init(&ca) == 0);
    CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_cond_init(&cv, &ca) == 0);
    CHECK(pthread_condattr_destroy(&ca) == 0);

    pthread_t t;
    CHECK(pthread_create(&t, NULL, signaller, NULL) == 0);

    int rc = 0;
    if (!ready) {          /* BUG 1: predicate read without the mutex      */
        printf("waiter:    saw ready == 0, about to wait\n");
        fflush(stdout);
        sleep_ms(200);     /* the gap, widened so it happens on every run */
        pthread_mutex_lock(&m);
        struct timespec dl;
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += 2;    /* safety net so the demo terminates           */
        /* BUG 2: `if` + single wait instead of `while (!ready) wait`. */
        rc = pthread_cond_timedwait(&cv, &m, &dl);
        pthread_mutex_unlock(&m);
    }
    CHECK(pthread_join(t, NULL) == 0);

    pthread_mutex_lock(&m);
    int r = ready;
    pthread_mutex_unlock(&m);
    if (rc == ETIMEDOUT) {
        printf("LOST WAKE-UP: waited 2 s and timed out although ready == %d.\n"
               "The signal was sent while nobody was waiting and is gone.\n", r);
        return 1;
    }
    printf("no lost wake-up this time (unexpected; the gap was too small)\n");
    return 0;
}
