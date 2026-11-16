/*
 * 05_timed_wait_monotonic.c — timed waits on a condition variable that are
 *                             immune to wall-clock jumps
 *
 * Concept : pthread_cond_timedwait() takes an ABSOLUTE deadline measured on
 *           the cv's clock. By default that clock is CLOCK_REALTIME, which
 *           jumps when NTP/the user sets the time. pthread_condattr_setclock
 *           (&a, CLOCK_MONOTONIC) fixes it at init time. The deadline is
 *           computed ONCE before the loop, so spurious wake-ups do not
 *           extend the total wait.
 *           glibc >= 2.30 also has pthread_cond_clockwait(cv, m, clock, abs)
 *           which picks the clock per call (shown on the timeout path only).
 * Build   : make run P=linux-internals/04-condition-variables/c/05_timed_wait_monotonic.c
 *           make run P=linux-internals/04-condition-variables/c/05_timed_wait_monotonic.c SAN=tsan
 * Read    : ../README.md  (section 3.8 "Timed waits")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
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

/*
 *   deadline on REALTIME, 100 ms ahead     deadline on MONOTONIC, 100 ms ahead
 *   wall: 12:00:00.0 -> wait until .1      mono: 5000.0 s -> wait until 5000.1
 *   NTP sets wall clock back 1 hour        NTP changes wall clock: irrelevant,
 *   -> the wait lasts 1 h 0.1 s !           MONOTONIC only moves forward
 *
 *   C++ note: std::condition_variable::wait_for/wait_until(steady_clock)
 *   use CLOCK_MONOTONIC internally (libstdc++ calls pthread_cond_clockwait).
 */
static struct timespec deadline_ms(clockid_t clk, long ms)
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

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t cv; /* CLOCK_MONOTONIC */
    bool done;         /* guarded by m */
} Event;

/* Wait for ev->done for at most `ms` milliseconds. Returns true if it
 * became true, false on timeout. The canonical timed-wait loop. */
static bool event_wait_ms(Event *ev, long ms)
{
    struct timespec dl = deadline_ms(CLOCK_MONOTONIC, ms); /* ONCE */
    pthread_mutex_lock(&ev->m);
    int rc = 0;
    while (!ev->done && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&ev->cv, &ev->m, &dl);
    /* Re-check the predicate, not rc: the event may have happened in the
     * same instant the timeout fired. The predicate is the truth. */
    bool ok = ev->done;
    pthread_mutex_unlock(&ev->m);
    return ok;
}

static void *setter(void *arg)
{
    Event *ev = arg;
    struct timespec t = {0, 50 * 1000000L};
    nanosleep(&t, NULL);
    pthread_mutex_lock(&ev->m);
    ev->done = true;
    pthread_cond_signal(&ev->cv);
    pthread_mutex_unlock(&ev->m);
    return NULL;
}

int main(void)
{
    Event ev = {.done = false};
    pthread_condattr_t ca;
    clockid_t clk = -1;
    CHECK(pthread_condattr_init(&ca) == 0);
    CHECK(pthread_condattr_getclock(&ca, &clk) == 0);
    printf("[0] default cv clock: %s\n",
           clk == CLOCK_REALTIME ? "CLOCK_REALTIME" : "other");
    CHECK(clk == CLOCK_REALTIME);
    CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_condattr_getclock(&ca, &clk) == 0);
    CHECK(clk == CLOCK_MONOTONIC);
    CHECK(pthread_mutex_init(&ev.m, NULL) == 0);
    CHECK(pthread_cond_init(&ev.cv, &ca) == 0);
    CHECK(pthread_condattr_destroy(&ca) == 0);

    /* 1. Nobody sets the event: we must time out, never early. */
    double t0 = now_ms();
    bool ok = event_wait_ms(&ev, 120);
    double waited = now_ms() - t0;
    printf("[1] event never set: timed out=%s after %.1f ms (asked 120 ms)\n",
           ok ? "no" : "yes", waited);
    CHECK(!ok);
    CHECK(waited >= 119.0); /* a timed wait never returns ETIMEDOUT early */

    /* 2. Event set after ~50 ms, deadline 2 s: we return early, with it. */
    pthread_t t;
    CHECK(pthread_create(&t, NULL, setter, &ev) == 0);
    t0 = now_ms();
    ok = event_wait_ms(&ev, 2000);
    waited = now_ms() - t0;
    CHECK(pthread_join(t, NULL) == 0);
    printf("[2] event set by another thread: ok=%s after %.1f ms "
           "(deadline 2000 ms)\n", ok ? "yes" : "no", waited);
    CHECK(ok);

    /* 3. pthread_cond_clockwait: choose the clock per call (glibc 2.30+).
     * Shown on a timeout path with no other thread involved: older
     * ThreadSanitizer runtimes (GCC 11) do not intercept clockwait and would
     * miss the internal unlock/relock, so we keep it out of any handoff. */
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 30)
    {
        pthread_cond_t plain = PTHREAD_COND_INITIALIZER; /* REALTIME cv */
        pthread_mutex_t pm = PTHREAD_MUTEX_INITIALIZER;
        struct timespec dl = deadline_ms(CLOCK_MONOTONIC, 30);
        pthread_mutex_lock(&pm);
        int rc = 0;
        while (rc != ETIMEDOUT) /* no predicate: we only want the timeout */
            rc = pthread_cond_clockwait(&plain, &pm, CLOCK_MONOTONIC, &dl);
        pthread_mutex_unlock(&pm);
        printf("[3] pthread_cond_clockwait(CLOCK_MONOTONIC) on a default cv "
               "-> %s\n", strerror(rc));
        CHECK(rc == ETIMEDOUT);
    }
#else
    printf("[3] pthread_cond_clockwait: glibc < 2.30, skipped\n");
#endif
#else
    printf("[3] pthread_cond_clockwait: not glibc, skipped\n");
#endif

    CHECK(pthread_cond_destroy(&ev.cv) == 0);
    CHECK(pthread_mutex_destroy(&ev.m) == 0);
    printf("05_timed_wait_monotonic: all checks passed\n");
    return 0;
}
