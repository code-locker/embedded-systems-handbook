/*
 * 08_cyclictest_lite.c — measure wakeup latency the way cyclictest does:
 *                        sleep until an absolute time, read the clock,
 *                        record (actual - expected), print a histogram
 *
 * Concept : The number that decides whether a Linux system is "real-time"
 *           is the WORST-CASE wakeup latency: how late a thread runs after
 *           the timer it waited for expired. It is the sum of timer
 *           interrupt latency + time with preemption/IRQs disabled +
 *           scheduler + context switch. A thread does
 *           clock_nanosleep(ABSTIME, next) and then subtracts `next` from
 *           the clock: always >= 0. On a stock desktop kernel with
 *           SCHED_OTHER the max is often 100 us .. several ms; on the
 *           BeagleBone's PREEMPT_RT kernel with SCHED_FIFO 90 + mlockall +
 *           an isolated CPU it is typically tens of us. The program tries
 *           SCHED_FIFO 80 and falls back to SCHED_OTHER, and NEVER asserts
 *           latency values — only sanity (no early wakeups, count right).
 *           It measures twice: with the default 50 us TIMER SLACK that
 *           SCHED_OTHER threads get, and with prctl(PR_SET_TIMERSLACK, 1).
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/08_cyclictest_lite.c
 * Read    : ../README.md  (section 3.11 "Measuring latency: cyclictest")
 */
#define _GNU_SOURCE /* pthread_attr_setinheritsched, CPU_SET */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define UNDER_SANITIZER 1
#else
#define UNDER_SANITIZER 0
#endif

#define NSEC_PER_SEC 1000000000LL
#define INTERVAL_NS  (1000 * 1000LL) /* 1 ms, like cyclictest -i 1000 */
#define LOOPS        500             /* 0.5 s */
#define RT_PRIO      80

/* Histogram bucket upper bounds in microseconds; the last is "everything
 * above". cyclictest -h prints 1-us buckets; coarse ones fit a terminal. */
static const int bucket_us[] = {5, 10, 20, 50, 100, 200, 500, 1000, 5000};
#define NB (sizeof bucket_us / sizeof bucket_us[0])

struct measurement {
    int64_t lat_ns[LOOPS]; /* preallocated: no malloc inside the loop */
    int policy;            /* what the thread really ran with          */
    int tight_slack;       /* 1: set timer slack to 1 ns first         */
    long slack_ns;         /* the timer slack the thread ran with      */
};

static void ts_add_ns(struct timespec *t, int64_t add)
{
    t->tv_nsec += (long)add;
    while (t->tv_nsec >= NSEC_PER_SEC) {
        t->tv_nsec -= NSEC_PER_SEC;
        t->tv_sec++;
    }
}

static int64_t ts_ns(const struct timespec *t) { return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec; }

/*
 *   expected      actual wakeup (thread running again)
 *      |<-- latency -->|
 *  ----+---------------+-----------------------> CLOCK_MONOTONIC
 *      ^ hrtimer fires: IRQ -> wake thread -> scheduler -> context switch
 */
static void *measure(void *arg)
{
    struct measurement *m = arg;
    struct sched_param sp;
    CHECK(pthread_getschedparam(pthread_self(), &m->policy, &sp) == 0);
    /* TIMER SLACK: the kernel may delay a SCHED_OTHER thread's timer by up
     * to 50 us (default) to batch wakeups and save power. RT threads get 0.
     * Per-thread, no privilege needed. Without this line a SCHED_OTHER
     * measurement shows a ~50 us floor that is policy, not latency. */
    if (m->tight_slack)
        CHECK(prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0) == 0);
    m->slack_ns = (long)prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);

    struct timespec next, now;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &next) == 0);
    for (int i = 0; i < LOOPS; i++) {
        ts_add_ns(&next, INTERVAL_NS);
        int rc;
        while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL)) == EINTR) {
        }
        CHECK(rc == 0);
        clock_gettime(CLOCK_MONOTONIC, &now); /* vDSO: no syscall */
        m->lat_ns[i] = ts_ns(&now) - ts_ns(&next);
    }
    return NULL;
}

static int start_thread(pthread_t *t, struct measurement *m)
{
    /* Try SCHED_FIFO RT_PRIO. PTHREAD_EXPLICIT_SCHED is essential: without
     * it the new thread INHERITS the creator's policy and the attr policy
     * is silently ignored — the #1 "my thread isn't RT" bug. */
    pthread_attr_t a;
    CHECK(pthread_attr_init(&a) == 0);
    CHECK(pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED) == 0);
    CHECK(pthread_attr_setschedpolicy(&a, SCHED_FIFO) == 0);
    struct sched_param sp = {.sched_priority = RT_PRIO};
    CHECK(pthread_attr_setschedparam(&a, &sp) == 0);
    int e = pthread_create(t, &a, measure, m);
    pthread_attr_destroy(&a);
    if (e == 0)
        return 1;
    CHECK(e == EPERM);
    printf("   SCHED_FIFO %d: EPERM -> measuring as SCHED_OTHER (expect a worse max)\n", RT_PRIO);
    CHECK(pthread_create(t, NULL, measure, m) == 0);
    return 0;
}

static int cmp64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x > y) - (x < y);
}

static void run_once(int tight_slack)
{
    static struct measurement m; /* static: 4 KiB array off the stack */
    memset(&m, 0, sizeof m);
    m.tight_slack = tight_slack;
    pthread_t t;
    int rt = start_thread(&t, &m);
    CHECK(pthread_join(t, NULL) == 0); /* join: happens-before for m */
    if (rt)
        CHECK(m.policy == SCHED_FIFO);

    /* Statistics (outside the measured loop: printing and sorting are slow) */
    int64_t min = m.lat_ns[0], max = m.lat_ns[0], sum = 0;
    unsigned hist[NB + 1] = {0};
    for (int i = 0; i < LOOPS; i++) {
        int64_t l = m.lat_ns[i];
        CHECK(l >= 0); /* an absolute sleep never returns before `next` */
        if (l < min) min = l;
        if (l > max) max = l;
        sum += l;
        size_t b = 0;
        while (b < NB && l >= (int64_t)bucket_us[b] * 1000)
            b++;
        hist[b]++;
    }
    static int64_t sorted[LOOPS];
    memcpy(sorted, m.lat_ns, sizeof sorted);
    qsort(sorted, LOOPS, sizeof sorted[0], cmp64);

    printf("   run %d: policy %s, timer slack %ld ns\n", tight_slack + 1,
           m.policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER", m.slack_ns);
    printf("   min %.1f us | avg %.1f us | p99 %.1f us | max %.1f us\n", min / 1e3,
           (double)sum / LOOPS / 1e3, sorted[LOOPS * 99 / 100] / 1e3, max / 1e3);
    printf("   histogram (wakeup latency):\n");
    unsigned total = 0;
    for (size_t b = 0; b <= NB; b++) {
        char label[32];
        if (b == 0)
            snprintf(label, sizeof label, "      < %4d us", bucket_us[0]);
        else if (b < NB)
            snprintf(label, sizeof label, "%4d .. %4d us", bucket_us[b - 1], bucket_us[b]);
        else
            snprintf(label, sizeof label, "     >= %4d us", bucket_us[NB - 1]);
        int bar = (int)(hist[b] * 50 / LOOPS);
        printf("   %s %4u |%.*s\n", label, hist[b], bar,
               "##################################################");
        total += hist[b];
    }
    CHECK(total == LOOPS);
}

int main(void)
{
    alarm(20);
    printf("cyclictest-lite: %d wakeups, interval %lld us\n", LOOPS, INTERVAL_NS / 1000);

    /* mlockall keeps page faults out of the measurement. Under ASan/TSan
     * the process maps terabytes of NORESERVE shadow memory: mlockall
     * fails with ENOMEM (or, with CAP_IPC_LOCK, tries to populate all of
     * it!), so it is skipped there. */
    if (UNDER_SANITIZER)
        printf("   mlockall: skipped under a sanitizer (huge shadow mappings)\n");
    else if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
        printf("   mlockall(MCL_CURRENT|MCL_FUTURE): ok\n");
    else
        printf("   mlockall: %s (RLIMIT_MEMLOCK too small; see c/09)\n", strerror(errno));

    run_once(0);
    run_once(1);
    printf("   The real tool:  sudo cyclictest -m -S -p 90 -i 1000 -h 400 -D 1m\n"
           "   and under load: stress-ng --cpu 2 --io 2 --vm 1 & (latency = the max!)\n");
    printf("08_cyclictest_lite: all checks passed\n");
    return 0;
}
