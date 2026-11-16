/*
 * 04_sem_signal_handler.c — sem_post() is async-signal-safe: wake a thread
 *                           from a signal handler (the user-space "ISR")
 *
 * Concept : Inside a signal handler you may only call async-signal-safe
 *           functions (man 7 signal-safety). pthread_mutex_lock,
 *           pthread_cond_signal, printf and malloc are NOT on that list —
 *           the handler may have interrupted the very thread that holds the
 *           lock. sem_post IS on the list: it is one atomic increment plus,
 *           if needed, a futex wake. So "handler posts, a normal thread
 *           waits and does the real work" is the classic pattern — exactly
 *           like an RTOS ISR giving a semaphore to a task
 *           (xSemaphoreGiveFromISR).
 *           Bonus: sem_wait returns -1/EINTR when a handler runs without
 *           SA_RESTART; the wait loop must retry.
 * Build   : make run P=linux-internals/05-semaphores/c/04_sem_signal_handler.c
 *           make run P=linux-internals/05-semaphores/c/04_sem_signal_handler.c SAN=tsan
 * Read    : ../README.md  (section 3.6 "Posting from a signal handler")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
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

#define N_EVENTS 5

/*
 *   "interrupt" source             signal handler (async!)       worker thread
 *   (helper thread, every 10ms)    -------------------------     -------------
 *   pthread_kill(main, SIGUSR1) -> events_raised++ (atomic)      sem_wait ~~~~
 *                                  sem_post(&ev) ---------------> wakes, handles
 *                                  return                        event in normal
 *                                                                context: printf,
 *                                                                malloc, locks OK
 *   This is the "top half / bottom half" split of every interrupt handler.
 */
static sem_t g_ev;
static sem_t g_ack;         /* worker -> source: "handled, send the next" */
static atomic_int g_raised; /* lock-free atomics are signal-safe too */

static void on_sigusr1(int sig)
{
    (void)sig;
    int saved = errno; /* handlers must not clobber errno */
    atomic_fetch_add(&g_raised, 1);
    sem_post(&g_ev); /* async-signal-safe */
    errno = saved;
}

static void *irq_source(void *arg)
{
    pthread_t target = *(pthread_t *)arg;
    for (int i = 0; i < N_EVENTS; i++) {
        struct timespec t = {0, 10 * 1000000L};
        nanosleep(&t, NULL);
        /* Direct the signal at the main thread, so it is the one whose
         * sem_wait gets interrupted (EINTR) and the handler runs there. */
        CHECK(pthread_kill(target, SIGUSR1) == 0);
        /* Standard signals do not queue: two SIGUSR1 pending at once merge
         * into one. Waiting for the ack keeps the demo deterministic. (Real
         * code that must count events uses real-time signals, sigqueue, or
         * eventfd/signalfd — lesson 13.) */
        while (sem_wait(&g_ack) == -1)
            CHECK(errno == EINTR);
    }
    return NULL;
}

int main(void)
{
    CHECK(sem_init(&g_ev, 0, 0) == 0);
    CHECK(sem_init(&g_ack, 0, 0) == 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigusr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: sem_wait will see EINTR */
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    /* Block SIGUSR1 in the helper (it inherits the mask at create time),
     * so only main ever runs the handler. */
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    CHECK(pthread_sigmask(SIG_BLOCK, &block, &old) == 0);
    pthread_t self = pthread_self(), helper;
    CHECK(pthread_create(&helper, NULL, irq_source, &self) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, &old, NULL) == 0);

    int handled = 0, eintr = 0;
    while (handled < N_EVENTS) {
        if (sem_wait(&g_ev) == -1) {
            CHECK(errno == EINTR);
            eintr++; /* the handler ran while we slept: just retry */
            continue;
        }
        handled++;
        printf("    worker: handled event %d (printf is fine here, not in "
               "the handler)\n", handled);
        CHECK(sem_post(&g_ack) == 0);
    }
    CHECK(pthread_join(helper, NULL) == 0);

    printf("[1] %d signals raised, %d events handled, sem_wait interrupted "
           "with EINTR %d time(s) (timing-dependent)\n",
           atomic_load(&g_raised), handled, eintr);
    CHECK(atomic_load(&g_raised) == N_EVENTS);
    CHECK(handled == N_EVENTS);
    int v = -1;
    CHECK(sem_getvalue(&g_ev, &v) == 0);
    CHECK(v == 0);

    signal(SIGUSR1, SIG_DFL);
    CHECK(sem_destroy(&g_ev) == 0);
    CHECK(sem_destroy(&g_ack) == 0);
    printf("04_sem_signal_handler: all checks passed\n");
    return 0;
}
