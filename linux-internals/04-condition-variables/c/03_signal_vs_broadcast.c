/*
 * 03_signal_vs_broadcast.c — pthread_cond_signal vs pthread_cond_broadcast,
 *                            the thundering herd, and futile wake-ups
 *
 * Concept : signal() wakes AT LEAST ONE waiter, broadcast() wakes ALL.
 *           With N identical consumers and ONE new item, broadcast wakes N
 *           threads, one gets the item and N-1 wake up for nothing, re-take
 *           the mutex one after another and go back to sleep: the THUNDERING
 *           HERD. The program counts those futile wake-ups exactly.
 *           Use signal when any one waiter can handle the change; broadcast
 *           when the change may concern every waiter (shutdown, a latch
 *           opening, waiters waiting for DIFFERENT predicates on one cv).
 * Build   : make run P=linux-internals/04-condition-variables/c/03_signal_vs_broadcast.c
 *           make run P=linux-internals/04-condition-variables/c/03_signal_vs_broadcast.c SAN=tsan
 * Read    : ../README.md  (section 3.5 "signal vs broadcast")
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

#define N_CONSUMERS 4
#define N_SIGNAL_ROUNDS 8

/*
 *   broadcast with 1 token, 4 waiters:
 *
 *   W0  ~~sleep~~ wake -> lock -> token! take it      -> unlock
 *   W1  ~~sleep~~ wake -> .....wait for m..... -> lock -> nothing -> wait
 *   W2  ~~sleep~~ wake -> ..........wait for m.......... -> lock -> nothing -> wait
 *   W3  ~~sleep~~ wake -> ...............wait for m............... -> nothing
 *                 ^ all 4 runnable at once, they serialise on the mutex
 *   futile wake-ups: 3 (= N - 1). With 100 threads, 99 context switches for
 *   one item, and the mutex's cache line bouncing between all the cores.
 */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t item_cv;  /* consumers wait here for tokens        */
    pthread_cond_t state_cv; /* main waits here for consumer progress */
    int tokens;              /* the predicate: work available          */
    int waiting;             /* consumers currently inside cond_wait   */
    long wakeups;            /* returns from cond_wait                 */
    long futile;             /* ... that found no token (and no close) */
    long taken;              /* tokens consumed                        */
    int closed;
} Pool;

static void *consumer(void *arg)
{
    Pool *p = arg;
    pthread_mutex_lock(&p->m);
    for (;;) {
        while (p->tokens == 0 && !p->closed) {
            p->waiting++;
            pthread_cond_broadcast(&p->state_cv); /* "I am waiting" */
            pthread_cond_wait(&p->item_cv, &p->m);
            p->waiting--;
            p->wakeups++;
            if (p->tokens == 0 && !p->closed)
                p->futile++; /* woke up for nothing */
            pthread_cond_broadcast(&p->state_cv);
        }
        if (p->tokens > 0) {
            p->tokens--;
            p->taken++;
            pthread_cond_broadcast(&p->state_cv);
            continue;
        }
        break; /* closed and nothing left */
    }
    pthread_mutex_unlock(&p->m);
    return NULL;
}

/* main: wait (with a 5 s safety deadline) until fn(p) is true. Caller
 * holds p->m. The deadline uses CLOCK_MONOTONIC (see 05_timed_wait). */
static void wait_state(Pool *p, int (*fn)(const Pool *, long), long arg)
{
    struct timespec dl;
    clock_gettime(CLOCK_MONOTONIC, &dl);
    dl.tv_sec += 5;
    while (!fn(p, arg)) {
        int rc = pthread_cond_timedwait(&p->state_cv, &p->m, &dl);
        CHECK(rc == 0 || (rc == ETIMEDOUT && fn(p, arg)));
    }
}
static int all_waiting(const Pool *p, long n) { return p->waiting == n; }
static int token_gone(const Pool *p, long n) { (void)n; return p->tokens == 0; }
static int woke_at_least(const Pool *p, long n) { return p->wakeups >= n; }

int main(void)
{
    Pool p = {.tokens = 0};
    pthread_condattr_t ca;
    CHECK(pthread_condattr_init(&ca) == 0);
    CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_mutex_init(&p.m, NULL) == 0);
    CHECK(pthread_cond_init(&p.item_cv, NULL) == 0);
    CHECK(pthread_cond_init(&p.state_cv, &ca) == 0);
    CHECK(pthread_condattr_destroy(&ca) == 0);

    pthread_t th[N_CONSUMERS];
    for (int i = 0; i < N_CONSUMERS; i++)
        CHECK(pthread_create(&th[i], NULL, consumer, &p) == 0);

    /* ---- Phase 1: one token + signal, 8 times ------------------------- */
    pthread_mutex_lock(&p.m);
    long w0 = p.wakeups, f0 = p.futile;
    for (int r = 0; r < N_SIGNAL_ROUNDS; r++) {
        wait_state(&p, all_waiting, N_CONSUMERS);
        p.tokens = 1;
        pthread_cond_signal(&p.item_cv);
        wait_state(&p, token_gone, 0);
    }
    wait_state(&p, all_waiting, N_CONSUMERS);
    long sig_wakeups = p.wakeups - w0, sig_futile = p.futile - f0;
    printf("[1] signal   : %d tokens, %ld wake-ups, %ld futile "
           "(>= 1 wake-up per signal; extra ones would be spurious)\n",
           N_SIGNAL_ROUNDS, sig_wakeups, sig_futile);
    CHECK(p.taken == N_SIGNAL_ROUNDS);
    CHECK(sig_wakeups >= N_SIGNAL_ROUNDS);

    /* ---- Phase 2: one token + broadcast -------------------------------- */
    long w1 = p.wakeups, f1 = p.futile;
    p.tokens = 1;
    pthread_cond_broadcast(&p.item_cv);
    /* Every one of the N waiters was blocked (waiting == N), so every one
     * of them wakes and re-checks: exactly one finds the token. */
    wait_state(&p, woke_at_least, w1 + N_CONSUMERS);
    wait_state(&p, token_gone, 0);
    long bc_wakeups = p.wakeups - w1, bc_futile = p.futile - f1;
    printf("[2] broadcast: 1 token, %ld wake-ups, %ld futile "
           "(the thundering herd: N-1 = %d)\n",
           bc_wakeups, bc_futile, N_CONSUMERS - 1);
    CHECK(p.taken == N_SIGNAL_ROUNDS + 1);
    CHECK(bc_wakeups >= N_CONSUMERS);
    CHECK(bc_futile >= N_CONSUMERS - 1);

    /* ---- Phase 3: shutdown is a broadcast job ------------------------- */
    /* Every consumer must see `closed`; signal would wake only one and the
     * others would sleep forever (join would hang). */
    p.closed = 1;
    pthread_cond_broadcast(&p.item_cv);
    pthread_mutex_unlock(&p.m);
    for (int i = 0; i < N_CONSUMERS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    printf("[3] close + broadcast: all %d consumers exited\n", N_CONSUMERS);

    CHECK(pthread_cond_destroy(&p.item_cv) == 0);
    CHECK(pthread_cond_destroy(&p.state_cv) == 0);
    CHECK(pthread_mutex_destroy(&p.m) == 0);
    printf("03_signal_vs_broadcast: all checks passed\n");
    return 0;
}
