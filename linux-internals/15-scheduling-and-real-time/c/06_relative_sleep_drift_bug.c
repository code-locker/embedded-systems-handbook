/*
 * 06_relative_sleep_drift_bug.c — BUG: a "1 kHz control loop" written with
 *                                 usleep(period) after the work
 *
 * Concept : This is the most common periodic-task bug in embedded Linux
 *           code. It looks right, it compiles cleanly, no sanitizer can see
 *           it, and it runs "at 1 kHz" — except every cycle is
 *           period + work + wakeup latency long, so the loop falls behind a
 *           little more each cycle. A PID loop, an audio feeder or a CAN
 *           heartbeat built this way drifts against the outside world.
 *           The TOOL that catches it is the program's own self-check: it
 *           compares the loop's elapsed time with N * period on
 *           CLOCK_MONOTONIC and fails when the accumulated drift exceeds one
 *           period. Expect: "CHECK failed" and exit status 1.
 *           Fix: c/05_periodic_drift.c (absolute deadlines with
 *           clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...)).
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/06_relative_sleep_drift_bug.c
 * Read    : ../README.md  (section 3.9 "Sleeping accurately: relative vs absolute")
 */
#define _GNU_SOURCE /* usleep */
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
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define PERIOD_US 1000 /* 1 kHz */
#define WORK_US   200
#define CYCLES    500  /* 0.5 s of "control loop" */

static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static void control_step(void)
{
    /* read sensor, compute PID, write actuator: ~200 us of work */
    int64_t end = now_us() + WORK_US;
    while (now_us() < end) {
    }
}

int main(void)
{
    alarm(10);
    int64_t start = now_us();
    for (int i = 0; i < CYCLES; i++) {
        control_step();
        usleep(PERIOD_US); /* BUG: relative sleep; the period silently becomes
                            * PERIOD + WORK + latency, and errors accumulate */
    }
    int64_t elapsed = now_us() - start;
    int64_t drift = elapsed - (int64_t)CYCLES * PERIOD_US;
    printf("%d cycles of a %d us loop took %.1f ms (ideal %.1f ms): drift %+.1f ms,\n"
           "i.e. the loop really ran at %.0f Hz instead of %d Hz\n",
           CYCLES, PERIOD_US, elapsed / 1e3, CYCLES * PERIOD_US / 1e3, drift / 1e3,
           CYCLES * 1e6 / (double)elapsed, 1000000 / PERIOD_US);

    /* A periodic task must not fall behind by more than one period overall. */
    CHECK(drift < PERIOD_US);
    printf("06_relative_sleep_drift_bug: all checks passed\n");
    return 0;
}
