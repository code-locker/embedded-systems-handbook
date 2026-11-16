/*
 * 05_periodic_drift.c — a periodic loop done wrong (relative nanosleep) and
 *                       right (clock_nanosleep TIMER_ABSTIME on MONOTONIC)
 *
 * Concept : "do work, then sleep(period)" makes the real period
 *           period + work + wakeup latency, and the error ACCUMULATES:
 *           after N cycles you are N*(work+latency) late. The fix is to
 *           keep an absolute "next release time", add the period to it each
 *           cycle and sleep UNTIL it (TIMER_ABSTIME). Each wakeup is still a
 *           little late (latency), but the lateness no longer adds up: the
 *           error at cycle N is just that one wakeup's latency. Both loops
 *           here run 100 cycles of 2 ms with 0.3 ms of simulated work.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/05_periodic_drift.c
 * Read    : ../README.md  (section 3.9 "Sleeping accurately: relative vs absolute")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

#define NSEC_PER_SEC 1000000000LL
#define PERIOD_NS    (2 * 1000 * 1000LL) /* 2 ms  */
#define WORK_NS      (300 * 1000LL)      /* 0.3 ms busy work per cycle */
#define CYCLES       100

static int64_t now_ns(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (int64_t)t.tv_sec * NSEC_PER_SEC + t.tv_nsec;
}

/* Simulated work: spin until WORK_NS have passed. Spinning (not sleeping)
 * makes the work time exact and independent of the scheduler. */
static void work(void)
{
    int64_t end = now_ns() + WORK_NS;
    while (now_ns() < end) {
    }
}

/* Normalise after adding: tv_nsec must stay in [0, 1e9) or
 * clock_nanosleep returns EINVAL. The classic "timespec_add" helper. */
static void ts_add_ns(struct timespec *t, int64_t add)
{
    t->tv_nsec += (long)(add % NSEC_PER_SEC);
    t->tv_sec += (time_t)(add / NSEC_PER_SEC);
    if (t->tv_nsec >= NSEC_PER_SEC) {
        t->tv_nsec -= NSEC_PER_SEC;
        t->tv_sec++;
    }
}

struct stats {
    int64_t total_ns;  /* first release -> after last cycle            */
    int64_t drift_ns;  /* total - CYCLES*PERIOD: how late we finished  */
    int64_t max_late;  /* worst single wakeup lateness (absolute loop) */
    int missed;        /* cycles whose release time had already passed */
};

/*  WRONG:   |work|---sleep 2ms---|work|---sleep 2ms---|work| ...
 *           <------ 2.3+ ms ----->   every cycle is longer than 2 ms,
 *                                     and the extra adds up forever.   */
static struct stats relative_loop(void)
{
    struct stats s = {0};
    int64_t start = now_ns();
    for (int i = 0; i < CYCLES; i++) {
        work();
        struct timespec d = {0, PERIOD_NS};
        while (nanosleep(&d, &d) == -1 && errno == EINTR) {
        } /* EINTR: continue with the REMAINING time */
    }
    s.total_ns = now_ns() - start;
    s.drift_ns = s.total_ns - CYCLES * PERIOD_NS;
    return s;
}

/*  RIGHT:   t0      t0+2ms    t0+4ms    t0+6ms      release times are a
 *           |work|..|work|....|work|....|work|...   fixed grid; lateness
 *           ^ sleep UNTIL the next grid point        never accumulates.   */
static struct stats absolute_loop(void)
{
    struct stats s = {0};
    struct timespec next;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &next) == 0);
    int64_t start = (int64_t)next.tv_sec * NSEC_PER_SEC + next.tv_nsec;
    for (int i = 0; i < CYCLES; i++) {
        work();
        ts_add_ns(&next, PERIOD_NS);
        int64_t rel = (int64_t)next.tv_sec * NSEC_PER_SEC + next.tv_nsec;
        if (now_ns() > rel)
            s.missed++; /* overran: the sleep below returns at once */
        int rc;
        /* clock_nanosleep returns the error number (not -1/errno). With
         * TIMER_ABSTIME an EINTR retry just sleeps until the same instant:
         * no remaining-time bookkeeping needed. */
        while ((rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL)) == EINTR) {
        }
        CHECK(rc == 0);
        int64_t late = now_ns() - rel;
        CHECK(late >= 0); /* an absolute sleep never wakes EARLY */
        if (late > s.max_late)
            s.max_late = late;
    }
    s.total_ns = now_ns() - start;
    s.drift_ns = s.total_ns - CYCLES * PERIOD_NS;
    return s;
}

int main(void)
{
    alarm(10);
    printf("%d cycles, period %.1f ms, work %.1f ms per cycle (ideal total %.0f ms)\n",
           CYCLES, PERIOD_NS / 1e6, WORK_NS / 1e6, CYCLES * PERIOD_NS / 1e6);

    struct stats r = relative_loop();
    printf("1) relative nanosleep(period) loop: total %.1f ms, drift %+.2f ms "
           "(%.3f ms per cycle)\n",
           r.total_ns / 1e6, r.drift_ns / 1e6, r.drift_ns / 1e6 / CYCLES);

    struct stats a = absolute_loop();
    printf("2) clock_nanosleep(TIMER_ABSTIME) loop: total %.1f ms, drift %+.2f ms, "
           "worst wakeup %.3f ms late, %d missed releases\n",
           a.total_ns / 1e6, a.drift_ns / 1e6, a.max_late / 1e6, a.missed);

    /* Deterministic: every relative cycle lasts at least work + period, so
     * the relative loop is at least CYCLES*WORK late — by construction. */
    CHECK(r.drift_ns >= CYCLES * WORK_NS);
    /* The absolute loop ends at (last release time + one wakeup latency):
     * no accumulation. 20 ms is a very loose bound (10 periods) that holds
     * even on a busy laptop under TSan; on the board's PREEMPT_RT kernel
     * the drift is tens of microseconds. */
    CHECK(a.drift_ns >= 0 && a.drift_ns < 20 * 1000000LL);
    CHECK(a.drift_ns < r.drift_ns);
    printf("   -> absolute loop drift is bounded by ONE wakeup latency; the\n"
           "      relative loop's grows by work+latency every single cycle.\n");
    printf("05_periodic_drift: all checks passed\n");
    return 0;
}
