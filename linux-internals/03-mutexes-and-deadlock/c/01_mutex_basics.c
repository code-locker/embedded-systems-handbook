/*
 * 01_mutex_basics.c — pthread mutex: critical section, init, lock/unlock,
 *                     trylock, timedlock (and clocklock), RAII-style unlock
 *
 * Concept : A mutex gives MUTUAL EXCLUSION: at most one thread at a time is
 *           inside the critical section (the code that touches shared data).
 *           The thread that locked it OWNS it and is the only one allowed to
 *           unlock it. Besides the blocking lock() there are two non-blocking
 *           ways in: trylock() (fail at once with EBUSY) and timedlock()
 *           (give up at an absolute deadline with ETIMEDOUT).
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/01_mutex_basics.c
 *           make run P=linux-internals/03-mutexes-and-deadlock/c/01_mutex_basics.c SAN=tsan
 * Read    : ../README.md  (sections 3.1 to 3.3)
 */
#define _GNU_SOURCE /* pthread_mutex_clocklock (glibc >= 2.30) */
#include <errno.h>
#include <pthread.h>
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

#define N_THREADS 4
#define N_ITERS 50000

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Absolute deadline = now(clock) + ms. timedlock wants an ABSOLUTE time, not
 * a duration: if the call is interrupted and restarted, an absolute deadline
 * does not drift. */
static struct timespec deadline_in(clockid_t clk, long ms)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/* ------------------------------------------------------------------------
 * Part 1: the critical section
 *
 *   counter++ is THREE steps (load, add, store). Two threads interleaving
 *   them lose updates (lesson 02). The mutex turns the three steps into one
 *   indivisible block as seen by every other thread that uses the SAME mutex:
 *
 *     thread A                     thread B
 *     lock(m)          ok
 *     load  counter=7              lock(m)   -> blocks (A owns m)
 *     store counter=8                 |
 *     unlock(m)  ---- wakes B ------> | returns, B now owns m
 *                                  load  counter=8   (sees A's store:
 *                                  store counter=9    unlock = release,
 *                                  unlock(m)          lock = acquire)
 *
 *   The mutex also orders MEMORY: everything A wrote before unlock() is
 *   visible to B after its lock(). That is why plain (non-atomic) variables
 *   are fine inside a critical section, even on the weakly ordered A72.
 * ---------------------------------------------------------------------- */

/* Static initialisation: no init call, no destroy needed. Use it for
 * globals. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_counter; /* protected by g_lock */

static void *count_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < N_ITERS; i++) {
        pthread_mutex_lock(&g_lock);
        g_counter++; /* critical section: keep it SHORT */
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void part1_critical_section(void)
{
    pthread_t t[N_THREADS];
    double t0 = now_ms();
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, count_worker, NULL) == 0);
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_join(t[i], NULL) == 0);
    printf("[1] %d threads x %d locked increments -> counter = %ld "
           "(expected %d), %.1f ms\n",
           N_THREADS, N_ITERS, g_counter, N_THREADS * N_ITERS, now_ms() - t0);
    CHECK(g_counter == (long)N_THREADS * N_ITERS);
}

/* ------------------------------------------------------------------------
 * Part 2: trylock and timedlock from a second thread while main owns it
 * ---------------------------------------------------------------------- */
typedef struct {
    pthread_mutex_t *m;
    int trylock_rc;
    int timedlock_rc;
    double timed_waited_ms;
    int clocklock_rc; /* -1 = not available in this libc */
} Probe;

static void *probe_worker(void *arg)
{
    Probe *p = arg;

    /* trylock: never sleeps. EBUSY = somebody owns it. Use it when you have
     * something better to do than wait (e.g. drop a frame, retry later). */
    p->trylock_rc = pthread_mutex_trylock(p->m);

    /* timedlock: sleep, but at most until the deadline.
     * CAVEAT: the deadline is on CLOCK_REALTIME (wall clock). If NTP or the
     * user steps the clock (common on a board that boots in 1970 and then
     * gets network time!), the wait becomes much shorter or much longer. */
    struct timespec dl = deadline_in(CLOCK_REALTIME, 100);
    double t0 = now_ms();
    p->timedlock_rc = pthread_mutex_timedlock(p->m, &dl);
    p->timed_waited_ms = now_ms() - t0;

    p->clocklock_rc = -1;
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
    /* glibc 2.30+: same, but you choose the clock. CLOCK_MONOTONIC never
     * jumps, so this is the one to use for timeouts. (We only use it on the
     * timeout path here: older ThreadSanitizer runtimes do not intercept
     * clocklock, so a SUCCESSFUL clocklock would be invisible to TSan.) */
    struct timespec mdl = deadline_in(CLOCK_MONOTONIC, 50);
    p->clocklock_rc = pthread_mutex_clocklock(p->m, CLOCK_MONOTONIC, &mdl);
#endif
#endif
    return NULL;
}

static void *late_locker(void *arg)
{
    /* Deadline 2 s away, but main unlocks after ~50 ms: we get the lock. */
    pthread_mutex_t *m = arg;
    struct timespec dl = deadline_in(CLOCK_REALTIME, 2000);
    int rc = pthread_mutex_timedlock(m, &dl);
    if (rc == 0)
        pthread_mutex_unlock(m);
    return (void *)(long)rc;
}

static void part2_try_and_timed(void)
{
    /* Dynamic initialisation: needed for mutexes inside malloc'ed structs or
     * when you want non-default attributes (see 02_mutex_types.c). Every
     * pthread_mutex_init must be paired with pthread_mutex_destroy. */
    pthread_mutex_t m;
    CHECK(pthread_mutex_init(&m, NULL) == 0);

    CHECK(pthread_mutex_lock(&m) == 0); /* main owns m */
    Probe p = {.m = &m};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, probe_worker, &p) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(pthread_mutex_unlock(&m) == 0);

    printf("[2] while main holds the lock:\n");
    printf("    trylock   -> %s\n", strerror(p.trylock_rc));
    printf("    timedlock -> %s after %.1f ms (deadline 100 ms, CLOCK_REALTIME)\n",
           strerror(p.timedlock_rc), p.timed_waited_ms);
    if (p.clocklock_rc == -1)
        printf("    clocklock -> not available (glibc < 2.30)\n");
    else
        printf("    clocklock -> %s (deadline 50 ms, CLOCK_MONOTONIC)\n",
               strerror(p.clocklock_rc));
    CHECK(p.trylock_rc == EBUSY);
    CHECK(p.timedlock_rc == ETIMEDOUT);
    CHECK(p.clocklock_rc == -1 || p.clocklock_rc == ETIMEDOUT);

    /* Now the success path of timedlock: main holds m for 50 ms only. */
    CHECK(pthread_mutex_lock(&m) == 0);
    CHECK(pthread_create(&t, NULL, late_locker, &m) == 0);
    struct timespec nap = {0, 50 * 1000000L};
    nanosleep(&nap, NULL);
    CHECK(pthread_mutex_unlock(&m) == 0);
    void *ret;
    CHECK(pthread_join(t, &ret) == 0);
    printf("    timedlock with a 2 s deadline, owner unlocks after 50 ms -> %s\n",
           strerror((int)(long)ret));
    CHECK((long)ret == 0);

    CHECK(pthread_mutex_destroy(&m) == 0);
}

/* ------------------------------------------------------------------------
 * Part 3: "RAII" in C with __attribute__((cleanup))  (GCC and Clang)
 *
 * The most common mutex bug is an early `return` that forgets to unlock.
 * GCC/Clang can call a function automatically when a variable goes out of
 * scope — the same idea as C++ std::lock_guard. The Linux kernel uses this
 * since 6.5: `guard(mutex)(&m);` and `scoped_guard(spinlock, &l) { ... }`
 * in include/linux/cleanup.h. It is a compiler extension, not ISO C.
 * ---------------------------------------------------------------------- */
static void unlock_cleanup(pthread_mutex_t **pm)
{
    pthread_mutex_unlock(*pm);
}

#define LOCK_GUARD(m)                                                        \
    pthread_mutex_t *lock_guard_ __attribute__((cleanup(unlock_cleanup))) =  \
        (pthread_mutex_lock(m), (m))

static pthread_mutex_t g_table_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_table[8]; /* protected by g_table_lock */

/* Several return paths — every one of them unlocks, automatically. */
static int table_find(int value)
{
    LOCK_GUARD(&g_table_lock);
    if (value < 0)
        return -2; /* early return: cleanup runs, mutex unlocked */
    for (int i = 0; i < 8; i++)
        if (g_table[i] == value)
            return i; /* unlocked here too */
    return -1;
}

static void part3_cleanup_guard(void)
{
    {
        LOCK_GUARD(&g_table_lock);
        for (int i = 0; i < 8; i++)
            g_table[i] = i * 10;
    } /* unlocked at this brace */

    CHECK(table_find(-5) == -2);
    CHECK(table_find(30) == 3);
    CHECK(table_find(31) == -1);
    /* If any path above had leaked the lock, trylock would now fail. */
    CHECK(pthread_mutex_trylock(&g_table_lock) == 0);
    CHECK(pthread_mutex_unlock(&g_table_lock) == 0);
    printf("[3] cleanup-attribute guard: 3 return paths, lock released on "
           "every one\n");
}

int main(void)
{
    part1_critical_section();
    part2_try_and_timed();
    part3_cleanup_guard();
    printf("01_mutex_basics: all checks passed\n");
    return 0;
}
