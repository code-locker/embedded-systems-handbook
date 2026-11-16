/*
 * 06_futex_mutex.c — a real sleeping mutex from one int and the futex
 *                    syscall: Ulrich Drepper's 3-state mutex from the paper
 *                    "Futexes Are Tricky" (2004-2011)
 *
 * Concept : The lock word has three states:
 *             0 = unlocked
 *             1 = locked, nobody waiting        (unlock: no syscall)
 *             2 = locked, maybe someone waiting (unlock: FUTEX_WAKE)
 *           Uncontended lock+unlock is two atomic instructions and ZERO
 *           syscalls. Only when a thread really has to wait does anyone
 *           enter the kernel. glibc's pthread_mutex_lock (lowlevellock.h,
 *           nptl/pthread_mutex_lock.c) is exactly this algorithm.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/06_futex_mutex.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/06_futex_mutex.c SAN=tsan
 * Read    : ../README.md  (section "A futex mutex from scratch")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define ITERS 40000

/* Syscall counters, only for teaching (a real mutex would not have them). */
static atomic_long g_wait_calls, g_wake_calls;

static void futex_wait(atomic_int *addr, int expected)
{
    atomic_fetch_add_explicit(&g_wait_calls, 1, memory_order_relaxed);
    syscall(SYS_futex, addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, expected, NULL,
            NULL, 0);
}

static void futex_wake(atomic_int *addr, int n)
{
    atomic_fetch_add_explicit(&g_wake_calls, 1, memory_order_relaxed);
    syscall(SYS_futex, addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, NULL, NULL, 0);
}

typedef struct { atomic_int state; } FutexMutex;

/*
 *  lock():
 *    c = cmpxchg(state, 0 -> 1)                 FAST PATH: was 0, now 1. Done.
 *    if c != 0:                                 contended
 *       if c != 2: c = xchg(state, 2)           announce "there are waiters"
 *       while c != 0:                           (if xchg returned 0 we got it,
 *           futex_wait(state, 2)                 marked 2: harmless, one extra
 *           c = xchg(state, 2)                   wake later)
 *
 *  unlock():
 *    if fetch_sub(state, 1) != 1:               was 2: someone may sleep
 *        state = 0
 *        futex_wake(state, 1)                   wake exactly one
 *
 *  Timeline with two threads:
 *
 *   A: cmpxchg 0->1  ok          (state 1)  .....critical section.....
 *   B:   cmpxchg fails (c=1); xchg -> 2  (state 2); futex_wait(2): sleeps
 *   A: unlock: fetch_sub 2->1 returned 2 != 1 -> state=0, futex_wake(1)
 *   B:   wakes; xchg(state,2) returns 0 -> owns lock (state 2: pessimistic)
 *   B: unlock: fetch_sub returned 2 -> state=0, futex_wake (nobody: wasted,
 *      but correct. Pessimism costs a syscall, never correctness.)
 *
 *  Why not a 2-state lock (0/1) that always calls futex_wake in unlock?
 *  Correct, but every unlock is a syscall, ~100x slower than an atomic.
 *  Why does a waiter set 2 (not 1) after waking? It cannot know whether
 *  OTHER sleepers remain, so it must assume yes, or they would never be
 *  woken (lost wakeup -> deadlock). See "Futexes Are Tricky", mutex 2/3.
 *
 *  Memory ordering: acquire on every RMW that may take the lock, release on
 *  the unlock RMW / store. Because the hand-off is visible through these
 *  atomics, ThreadSanitizer (which cannot see inside the futex syscall)
 *  still sees a proper happens-before edge and stays quiet.
 */
static void fmutex_lock(FutexMutex *m)
{
    int c = 0;
    if (atomic_compare_exchange_strong_explicit(&m->state, &c, 1,
                                                memory_order_acquire,
                                                memory_order_relaxed))
        return; /* fast path, no syscall */
    if (c != 2)
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    while (c != 0) {
        futex_wait(&m->state, 2); /* returns at once if state is no longer 2 */
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    }
}

static bool fmutex_trylock(FutexMutex *m)
{
    int c = 0;
    return atomic_compare_exchange_strong_explicit(
        &m->state, &c, 1, memory_order_acquire, memory_order_relaxed);
}

static void fmutex_unlock(FutexMutex *m)
{
    if (atomic_fetch_sub_explicit(&m->state, 1, memory_order_release) != 1) {
        atomic_store_explicit(&m->state, 0, memory_order_release);
        futex_wake(&m->state, 1);
    }
}

/* ------------------------------------------------------------------------- */
static FutexMutex g_m;
static long g_counter; /* protected by g_m */

/* A little work inside the critical section, so waiters really do have to
 * sleep sometimes and the slow path gets exercised. */
static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        fmutex_lock(&g_m);
        long v = g_counter;
        for (volatile int k = 0; k < 20; k++) {
        }
        g_counter = v + 1; /* read-modify-write spread out: a race would show */
        fmutex_unlock(&g_m);
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void)
{
    atomic_init(&g_m.state, 0);

    /* 1. Uncontended: 100k lock/unlock pairs and NOT ONE syscall. */
    for (int i = 0; i < 100000; i++) {
        fmutex_lock(&g_m);
        g_counter++;
        fmutex_unlock(&g_m);
    }
    printf("uncontended: 100000 lock/unlock, futex_wait calls %ld, "
           "futex_wake calls %ld\n", atomic_load(&g_wait_calls),
           atomic_load(&g_wake_calls));
    CHECK(g_counter == 100000);
    CHECK(atomic_load(&g_wait_calls) == 0 && atomic_load(&g_wake_calls) == 0);

    /* 2. States: 0 -> 1 on lock, trylock fails while held, back to 0. */
    fmutex_lock(&g_m);
    CHECK(atomic_load(&g_m.state) == 1);
    CHECK(!fmutex_trylock(&g_m));
    fmutex_unlock(&g_m);
    CHECK(atomic_load(&g_m.state) == 0);

    /* 3. Contended: 4 threads. */
    g_counter = 0;
    pthread_t th[4];
    double t0 = now_ms();
    for (int i = 0; i < 4; i++)
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < 4; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    double ms = now_ms() - t0;
    printf("contended: counter %ld (expected %d) in %.1f ms; futex_wait %ld, "
           "futex_wake %ld (timing-dependent)\n", g_counter, 4 * ITERS, ms,
           atomic_load(&g_wait_calls), atomic_load(&g_wake_calls));
    CHECK(g_counter == 4L * ITERS);
    CHECK(atomic_load(&g_m.state) == 0); /* nobody left holding/waiting */
    printf("(syscalls happen only on contention; compare with %d lock "
           "operations, each of which would be a syscall in a naive lock)\n",
           4 * ITERS);

    printf("06_futex_mutex: all checks passed\n");
    return 0;
}
