/*
 * 01_pthread_spinlock.c — POSIX spinlocks: init, lock, trylock, unlock, and
 *                         when spinning is (and is not) a good idea
 *
 * Concept : A spinlock never sleeps. A thread that finds it taken loops
 *           ("spins") on the CPU until the owner releases it. That is the
 *           fastest possible hand-over when the critical section is a few
 *           dozen instructions AND the owner is running on another core at
 *           the same moment. pthread_spinlock_t is glibc's ready-made one.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/01_pthread_spinlock.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/01_pthread_spinlock.c SAN=tsan
 * Read    : ../README.md  (sections "Spinlock vs sleeping lock", "pthread_spinlock_t")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define ITERS 50000

/*
 * Spin vs sleep, in one picture (two cores, thread B wants the lock A has):
 *
 *   SPINLOCK                              MUTEX (sleeping lock)
 *   core0  A: [lock]--cs--[unlock]        core0  A: [lock]--cs--[unlock]+wake(B)
 *   core1  B:   spin spin spin [lock]     core1  B:  futex_wait -> sleep ... ctx
 *                              ^ ~50 ns                 switch back in, ~2-10 us
 *
 * When the critical section is shorter than two context switches, spinning
 * wins. When it is longer, or when the owner is NOT running (preempted,
 * blocked in I/O, or both threads share ONE core), B burns its whole time
 * slice for nothing, and that is why user-space code nearly always uses a
 * mutex instead.
 *
 * The user-space preemption problem: in the kernel, spin_lock() disables
 * preemption, so the owner cannot be scheduled out while holding the lock.
 * In user space you CANNOT disable preemption: the scheduler may stop the
 * owner in the middle of its critical section, and every waiter then spins
 * until the owner gets the CPU back (a whole time slice, milliseconds).
 * On a single core this is pure loss: the waiter spinning is exactly what
 * prevents the owner from running.
 */

static pthread_spinlock_t g_lock;
static long g_counter; /* protected by g_lock */

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        /* Keep the critical section tiny: that is the whole contract of a
         * spinlock. No printf, no malloc, no syscalls inside. */
        CHECK(pthread_spin_lock(&g_lock) == 0);
        g_counter++;
        CHECK(pthread_spin_unlock(&g_lock) == 0);
    }
    return NULL;
}

/* trylock: the non-blocking variant. Returns EBUSY instead of spinning, so
 * the caller can go and do something else (classic "opportunistic" work). */
static void *try_worker(void *arg)
{
    long *busy = arg; /* how many times trylock found it taken (this thread) */
    int done = 0;
    while (done < ITERS) {
        int rc = pthread_spin_trylock(&g_lock);
        if (rc == 0) {
            g_counter++;
            CHECK(pthread_spin_unlock(&g_lock) == 0);
            done++;
        } else {
            CHECK(rc == EBUSY);
            (*busy)++; /* in real code: do other useful work here */
        }
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
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    /* Rule: never run more spinning threads than cores. Here at most 4. */
    int nthreads = (int)(ncpu < 4 ? (ncpu < 2 ? 2 : ncpu) : 4);
    printf("online CPUs: %ld, using %d threads\n", ncpu, nthreads);
    if (ncpu < 2)
        printf("NOTE: single core: spinning only wastes the owner's CPU time\n");

    /* PTHREAD_PROCESS_PRIVATE: only threads of this process use it.
     * PTHREAD_PROCESS_SHARED would allow placing it in shared memory
     * (lesson 11) and using it from several processes. */
    CHECK(pthread_spin_init(&g_lock, PTHREAD_PROCESS_PRIVATE) == 0);

    /* 1. Single-thread semantics: trylock on a held lock is EBUSY.
     *    (A spinlock is NOT recursive: pthread_spin_lock() here would spin
     *    forever, a self-deadlock. trylock is the safe way to show it.) */
    CHECK(pthread_spin_lock(&g_lock) == 0);
    CHECK(pthread_spin_trylock(&g_lock) == EBUSY);
    CHECK(pthread_spin_unlock(&g_lock) == 0);
    CHECK(pthread_spin_trylock(&g_lock) == 0);
    CHECK(pthread_spin_unlock(&g_lock) == 0);
    printf("trylock on held lock -> EBUSY, on free lock -> 0\n");

    /* 2. Many threads, blocking lock. */
    pthread_t th[4];
    double t0 = now_ms();
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    double t1 = now_ms();
    printf("pthread_spin_lock   : counter %ld (expected %ld) in %.1f ms\n",
           g_counter, (long)nthreads * ITERS, t1 - t0);
    CHECK(g_counter == (long)nthreads * ITERS);

    /* 3. Many threads, trylock loop. */
    g_counter = 0; /* safe: all threads joined (join = synchronisation) */
    long busy[4] = {0};
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_create(&th[i], NULL, try_worker, &busy[i]) == 0);
    long busy_total = 0;
    for (int i = 0; i < nthreads; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        busy_total += busy[i];
    }
    printf("pthread_spin_trylock: counter %ld, EBUSY seen %ld times "
           "(timing-dependent)\n", g_counter, busy_total);
    CHECK(g_counter == (long)nthreads * ITERS);

    CHECK(pthread_spin_destroy(&g_lock) == 0);
    printf("01_pthread_spinlock: all checks passed\n");
    return 0;
}
