/*
 * 04_priority_inversion.c — priority inversion, priority inheritance and
 *                           priority ceiling: simulated, then the real
 *                           pthread attributes
 *
 * Concept : A high-priority task H waits for a mutex held by a low-priority
 *           task L. A medium task M, which needs no lock at all, preempts L
 *           and so delays H for as long as M likes: "priority inversion".
 *           Fix 1, PRIORITY INHERITANCE: while H waits, L runs at H's
 *           priority. Fix 2, PRIORITY CEILING (protect): whoever holds the
 *           mutex runs at a fixed, high "ceiling" priority.
 *           Part 1 is a deterministic single-CPU scheduler simulation that
 *           prints the three timelines. Part 2 sets PTHREAD_PRIO_INHERIT /
 *           PTHREAD_PRIO_PROTECT on real mutexes and reads them back (no root
 *           needed). Part 3 tries SCHED_FIFO and skips politely on EPERM.
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/04_priority_inversion.c
 *           make run P=linux-internals/03-mutexes-and-deadlock/c/04_priority_inversion.c SAN=tsan
 * Read    : ../README.md  (section 3.6 "Priority inversion")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif

/* ========================================================================
 * Part 1: simulation of one CPU, a fixed-priority preemptive scheduler
 *         (like SCHED_FIFO or any RTOS), one mutex, three tasks.
 *
 *   L (prio 1): arrives t=0, 1 tick of work, then lock, 4 ticks inside
 *   H (prio 3): arrives t=2, 1 tick of work, then lock, 1 tick inside
 *   M (prio 2): arrives t=3, 6 ticks of pure computation, no lock
 *
 * This is the Mars Pathfinder bug (1997): H = the information bus
 * manager, L = the meteorological data task, both sharing a VxWorks mutex
 * around the bus; M = the communications task. H starved, a watchdog saw
 * the bus task miss its deadline and reset the whole lander, again and
 * again. The fix, uploaded from Earth, was to switch on priority
 * inheritance for that one mutex.
 * ====================================================================== */
enum Mode { NO_PROTOCOL, INHERIT, CEILING };
enum Phase { PRE, WANT_LOCK, IN_LOCK, DONE };

typedef struct {
    char name;
    int prio, arrive;
    int pre, crit;  /* ticks left before the lock / inside the lock */
    enum Phase phase;
    int finish;     /* tick at which the task completed */
} Task;

#define CEILING_PRIO 3 /* = highest priority of any task using the mutex */

static int effective_prio(const Task *t, const Task *ts, int n,
                          const Task *owner, enum Mode mode)
{
    int p = t->prio;
    if (t != owner)
        return p;
    if (mode == CEILING)
        return CEILING_PRIO; /* raised the moment the lock was taken */
    if (mode == INHERIT) /* raised only while somebody better waits */
        for (int i = 0; i < n; i++)
            if (ts[i].phase == WANT_LOCK && ts[i].prio > p)
                p = ts[i].prio;
    return p;
}

/* Returns H's finish tick; writes the timeline, one letter per tick. */
static int simulate(enum Mode mode, char *timeline, size_t cap)
{
    Task ts[3] = {
        {'L', 1, 0, 1, 4, PRE, -1},
        {'H', 3, 2, 1, 1, PRE, -1},
        {'M', 2, 3, 6, 0, PRE, -1},
    };
    const int n = 3;
    Task *owner = NULL;  /* who holds the mutex */
    Task *running = NULL;
    size_t len = 0;

    for (int tick = 0; tick < 64; tick++) {
        /* Pick the highest effective priority among READY tasks. A task in
         * WANT_LOCK is ready only if the mutex is free (else it sleeps on
         * the mutex). Ties keep the running task (FIFO within a level). */
        Task *best = NULL;
        int best_p = -1;
        for (int i = 0; i < n; i++) {
            Task *t = &ts[i];
            if (t->arrive > tick || t->phase == DONE)
                continue;
            if (t->phase == WANT_LOCK && owner != NULL)
                continue; /* blocked */
            int p = effective_prio(t, ts, n, owner, mode);
            if (p > best_p || (p == best_p && t == running)) {
                best = t;
                best_p = p;
            }
        }
        if (best == NULL)
            break; /* everybody done */
        running = best;

        if (best->phase == WANT_LOCK) { /* take the free mutex, no time */
            owner = best;
            best->phase = IN_LOCK;
        }
        /* Run one tick. */
        if (best->phase == PRE) {
            if (--best->pre == 0)
                best->phase = best->crit > 0 ? WANT_LOCK : DONE;
        } else if (best->phase == IN_LOCK) {
            if (--best->crit == 0) {
                owner = NULL; /* unlock */
                best->phase = DONE;
            }
        }
        if (best->phase == DONE)
            best->finish = tick + 1;
        if (len + 1 < cap)
            timeline[len++] = best->name;
    }
    timeline[len] = '\0';
    return ts[1].finish;
}

static void part1_simulation(void)
{
    static const char *names[] = {"no protocol (inversion!)",
                                  "priority inheritance    ",
                                  "priority ceiling = 3    "};
    int h_finish[3];
    char tl[3][64];
    printf("[1] simulated single CPU, tick by tick (L=low, M=medium, H=high)\n");
    printf("    time:                       0123456789012\n");
    for (int m = 0; m < 3; m++) {
        h_finish[m] = simulate((enum Mode)m, tl[m], sizeof tl[m]);
        printf("    %s  : %s   H waits %2d ticks (arrived t=2, done t=%d)\n",
               names[m], tl[m], h_finish[m] - 2, h_finish[m]);
    }
    /*
     *  no protocol : L L H M M M M M M L L L H
     *                    ^ H locks? L owns it -> H sleeps
     *                      ^^^^^^^^^^^ M runs: it outranks L, nothing stops it
     *  inheritance : L L H L L L H M M M M M M
     *                      ^^^^^ L runs at prio 3 while H waits, M must wait
     */
    CHECK(strcmp(tl[NO_PROTOCOL], "LLHMMMMMMLLLH") == 0);
    CHECK(strcmp(tl[INHERIT], "LLHLLLHMMMMMM") == 0);
    CHECK(strcmp(tl[CEILING], "LLLLLHHMMMMMM") == 0);
    CHECK(h_finish[INHERIT] < h_finish[NO_PROTOCOL]);
    CHECK(h_finish[CEILING] < h_finish[NO_PROTOCOL]);
    printf("    -> without a protocol H's wait is bounded by M's run time, "
           "i.e. NOT bounded at all\n");
}

/* ========================================================================
 * Part 2: the real attributes. Setting them needs no privileges; they only
 * MATTER for threads with real-time policies (SCHED_FIFO / SCHED_RR).
 * PTHREAD_PRIO_INHERIT on Linux = a "PI futex": the kernel's rt_mutex code
 * boosts the owner (FUTEX_LOCK_PI / FUTEX_UNLOCK_PI syscalls).
 * ====================================================================== */
static const char *proto_name(int p)
{
    return p == PTHREAD_PRIO_NONE      ? "PTHREAD_PRIO_NONE"
           : p == PTHREAD_PRIO_INHERIT ? "PTHREAD_PRIO_INHERIT"
           : p == PTHREAD_PRIO_PROTECT ? "PTHREAD_PRIO_PROTECT"
                                       : "?";
}

static void part2_attributes(void)
{
    pthread_mutexattr_t a;
    int proto = -1;
    printf("[2] mutex protocol attributes\n");

    /* default */
    CHECK(pthread_mutexattr_init(&a) == 0);
    CHECK(pthread_mutexattr_getprotocol(&a, &proto) == 0);
    printf("    default protocol: %s\n", proto_name(proto));
    CHECK(proto == PTHREAD_PRIO_NONE);

    /* inheritance: lock/unlock works for an ordinary user */
    CHECK(pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT) == 0);
    CHECK(pthread_mutexattr_getprotocol(&a, &proto) == 0);
    CHECK(proto == PTHREAD_PRIO_INHERIT);
    pthread_mutex_t pi;
    CHECK(pthread_mutex_init(&pi, &a) == 0);
    CHECK(pthread_mutex_lock(&pi) == 0);
    CHECK(pthread_mutex_unlock(&pi) == 0);
    CHECK(pthread_mutex_destroy(&pi) == 0);
    printf("    %s: set, read back, lock/unlock ok\n", proto_name(proto));

    /* ceiling: the ceiling is a SCHED_FIFO priority (1..99 on Linux) */
    int lo = sched_get_priority_min(SCHED_FIFO);
    int hi = sched_get_priority_max(SCHED_FIFO);
    CHECK(pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_PROTECT) == 0);
    CHECK(pthread_mutexattr_setprioceiling(&a, 50) == 0);
    int ceil = -1;
    CHECK(pthread_mutexattr_getprotocol(&a, &proto) == 0);
    CHECK(pthread_mutexattr_getprioceiling(&a, &ceil) == 0);
    CHECK(proto == PTHREAD_PRIO_PROTECT && ceil == 50);
    printf("    %s: ceiling %d (SCHED_FIFO range %d..%d), read back ok\n",
           proto_name(proto), ceil, lo, hi);

    pthread_mutex_t pp;
    CHECK(pthread_mutex_init(&pp, &a) == 0);
    CHECK(pthread_mutex_getprioceiling(&pp, &ceil) == 0);
    CHECK(ceil == 50);
    /* Locking a PROTECT mutex raises the caller to the ceiling: on Linux
     * that is a real-time priority, so it needs CAP_SYS_NICE (or an
     * RLIMIT_RTPRIO that allows it). As a normal user glibc refuses with
     * EINVAL/EPERM. (ThreadSanitizer treats a failed lock as "use of an
     * invalid mutex", so the attempt is skipped under TSan.) */
    int rc = UNDER_TSAN ? EPERM : pthread_mutex_lock(&pp);
    if (UNDER_TSAN) {
        printf("    locking it: skipped under TSan\n");
    } else if (rc == 0) {
        printf("    locking it: ok (this process may raise its priority)\n");
        CHECK(pthread_mutex_unlock(&pp) == 0);
    } else {
        printf("    locking it: %s -> raising to the ceiling needs real-time "
               "privileges; skipped\n", strerror(rc));
    }
    CHECK(pthread_mutex_destroy(&pp) == 0);
    CHECK(pthread_mutexattr_destroy(&a) == 0);
}

/* ========================================================================
 * Part 3: can we even get SCHED_FIFO? (root, CAP_SYS_NICE, or rtprio limit
 * in /etc/security/limits.conf). A real priority-inversion experiment on
 * the board would pin L, M, H to one core with sched_setaffinity.
 * ====================================================================== */
static void *rt_thread(void *arg)
{
    int *policy_out = arg;
    struct sched_param sp;
    CHECK(pthread_getschedparam(pthread_self(), policy_out, &sp) == 0);
    return NULL;
}

static void part3_try_sched_fifo(void)
{
    pthread_attr_t at;
    struct sched_param sp = {.sched_priority = sched_get_priority_min(SCHED_FIFO)};
    CHECK(pthread_attr_init(&at) == 0);
    /* Without EXPLICIT_SCHED the new thread just inherits the creator's
     * policy and the two calls below are silently ignored. */
    CHECK(pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED) == 0);
    CHECK(pthread_attr_setschedpolicy(&at, SCHED_FIFO) == 0);
    CHECK(pthread_attr_setschedparam(&at, &sp) == 0);
    pthread_t t;
    int policy = -1;
    int rc = pthread_create(&t, &at, rt_thread, &policy);
    if (rc == EPERM) {
        printf("[3] SCHED_FIFO thread: EPERM (not root / no CAP_SYS_NICE) -> "
               "skipped. On the board: sudo, or `ulimit -r 99` via limits.conf\n");
    } else {
        CHECK(rc == 0);
        CHECK(pthread_join(t, NULL) == 0);
        printf("[3] SCHED_FIFO thread created, policy read back = %s\n",
               policy == SCHED_FIFO ? "SCHED_FIFO" : "other");
        CHECK(policy == SCHED_FIFO);
    }
    CHECK(pthread_attr_destroy(&at) == 0);
}

int main(void)
{
    part1_simulation();
    part2_attributes();
    part3_try_sched_fifo();
    printf("04_priority_inversion: all checks passed\n");
    return 0;
}
