/*
 * 07_posix_timers.c — POSIX interval timers: timer_create + SIGEV_SIGNAL,
 *                     waiting with sigtimedwait, overrun counting, one-shot
 *                     timers, and a CPU-time timer
 *
 * Concept : timer_create() gives a process any number of kernel hrtimers
 *           on any clock. On expiry the kernel queues a signal (SIGEV_SIGNAL)
 *           carrying your sigev_value. If the previous signal is still
 *           pending when the timer fires again, the kernel does not queue a
 *           second one: it counts an OVERRUN, readable with
 *           timer_getoverrun() or siginfo.si_overrun — missed periods are
 *           counted, not silently lost. We block the signal and collect it
 *           synchronously with sigtimedwait(): no async handler, no
 *           async-signal-safety worries, and it works under TSan.
 *           (SIGEV_THREAD would run a callback in a glibc-made thread that
 *           TSan cannot see; timerfd, lesson 13, is the fd-based option.)
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/07_posix_timers.c
 * Read    : ../README.md  (section 3.10 "Timers")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
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

static int64_t now_ns(clockid_t c)
{
    struct timespec t;
    CHECK(clock_gettime(c, &t) == 0);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void sleep_ms(long ms)
{
    struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&d, &d) == -1 && errno == EINTR) {
    }
}

/* Wait (max 1 s, never hang) for one signal from `set`. */
static int wait_sig(const sigset_t *set, siginfo_t *si)
{
    struct timespec to = {1, 0};
    int s;
    while ((s = sigtimedwait(set, si, &to)) == -1 && errno == EINTR) {
    }
    return s;
}

static timer_t make_timer(clockid_t clk, int signo, int cookie)
{
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL; /* queue signo to the process        */
    sev.sigev_signo = signo;         /* an RT signal: queued with a value  */
    sev.sigev_value.sival_int = cookie; /* tells timers apart in the handler */
    timer_t t;
    CHECK(timer_create(clk, &sev, &t) == 0);
    return t;
}

int main(void)
{
    alarm(10);
    const int SIG_TICK = SIGRTMIN, SIG_CPU = SIGRTMIN + 1;

    /* Block the timer signals BEFORE anything can fire, so they stay
     * pending until sigtimedwait collects them. (In a threaded program do
     * this in main before creating threads: the mask is inherited.) */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIG_TICK);
    sigaddset(&set, SIG_CPU);
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);
    sigset_t tick_only;
    sigemptyset(&tick_only);
    sigaddset(&tick_only, SIG_TICK);

    /* ---- 1. periodic 5 ms timer on CLOCK_MONOTONIC ---- */
    timer_t tick = make_timer(CLOCK_MONOTONIC, SIG_TICK, 42);
    struct itimerspec its = {.it_value = {0, 5000000}, .it_interval = {0, 5000000}};
    CHECK(timer_settime(tick, 0, &its, NULL) == 0);
    printf("1) periodic 5 ms timer, 10 ticks collected with sigtimedwait:\n   intervals (ms):");
    int64_t prev = now_ns(CLOCK_MONOTONIC);
    for (int i = 0; i < 10; i++) {
        siginfo_t si;
        CHECK(wait_sig(&tick_only, &si) == SIG_TICK);
        CHECK(si.si_code == SI_TIMER);          /* sent by a POSIX timer   */
        CHECK(si.si_value.sival_int == 42);     /* our cookie came back    */
        int64_t t = now_ns(CLOCK_MONOTONIC);
        printf(" %.2f", (double)(t - prev) / 1e6);
        prev = t;
    }
    printf("\n");

    /* ---- 2. overruns: stop collecting for 55 ms (= 11 periods) ---- */
    sleep_ms(55);
    siginfo_t si;
    CHECK(wait_sig(&tick_only, &si) == SIG_TICK);
    int ovr = timer_getoverrun(tick);
    printf("2) after not waiting for 55 ms: ONE signal, si_overrun=%d, timer_getoverrun=%d\n"
           "   (expirations while the signal was pending are counted, not queued)\n",
           si.si_overrun, ovr);
    /* At least floor(55/5)=11 expirations, one became the signal: >= 5 is a
     * very safe lower bound even if we were descheduled. */
    CHECK(ovr >= 5 && si.si_overrun == ovr);

    /* ---- 3. one-shot: it_interval = 0; timer_gettime shows time left ---- */
    struct itimerspec one = {.it_value = {0, 30000000}, .it_interval = {0, 0}};
    CHECK(timer_settime(tick, 0, &one, NULL) == 0); /* re-arm replaces periodic */
    /* Drain a periodic tick that may have been queued before the re-arm. */
    struct timespec zero = {0, 0};
    while (sigtimedwait(&tick_only, &si, &zero) == SIG_TICK) {
    }
    struct itimerspec left;
    CHECK(timer_gettime(tick, &left) == 0);
    printf("3) one-shot 30 ms: timer_gettime says %.2f ms left, interval %ld\n",
           (double)left.it_value.tv_nsec / 1e6, (long)left.it_interval.tv_nsec);
    CHECK(left.it_value.tv_sec == 0 && left.it_value.tv_nsec > 0 &&
          left.it_value.tv_nsec <= 30000000 && left.it_interval.tv_nsec == 0);
    int64_t t0 = now_ns(CLOCK_MONOTONIC);
    CHECK(wait_sig(&tick_only, &si) == SIG_TICK);
    double fired = (double)(now_ns(CLOCK_MONOTONIC) - t0) / 1e6;
    printf("   fired after %.2f ms; afterwards it is disarmed\n", fired);
    CHECK(timer_gettime(tick, &left) == 0);
    CHECK(left.it_value.tv_sec == 0 && left.it_value.tv_nsec == 0);
    CHECK(timer_delete(tick) == 0);

    /* ---- 4. a CPU-time timer: fires after 30 ms of THREAD CPU ----
     * Sleeping does not count; only running does. This is how a watchdog
     * says "this computation used too much CPU" (and how RLIMIT_CPU and
     * SCHED_DEADLINE's overrun signal are built). */
    timer_t cpu = make_timer(CLOCK_THREAD_CPUTIME_ID, SIG_CPU, 7);
    struct itimerspec c30 = {.it_value = {0, 30000000}};
    CHECK(timer_settime(cpu, 0, &c30, NULL) == 0);
    sleep_ms(50); /* 50 ms asleep: costs no CPU, the timer must NOT fire */
    sigset_t pend;
    CHECK(sigpending(&pend) == 0);
    CHECK(!sigismember(&pend, SIG_CPU));
    int64_t c0 = now_ns(CLOCK_THREAD_CPUTIME_ID), w0 = now_ns(CLOCK_MONOTONIC);
    do { /* spin until the CPU timer's signal is pending (2 s cap) */
        CHECK(sigpending(&pend) == 0);
        CHECK(now_ns(CLOCK_MONOTONIC) - w0 < 2000000000LL);
    } while (!sigismember(&pend, SIG_CPU));
    double used = (double)(now_ns(CLOCK_THREAD_CPUTIME_ID) - c0) / 1e6;
    sigset_t cpu_only;
    sigemptyset(&cpu_only);
    sigaddset(&cpu_only, SIG_CPU);
    CHECK(wait_sig(&cpu_only, &si) == SIG_CPU && si.si_value.sival_int == 7);
    printf("4) CPU-time timer: did not fire during a 50 ms sleep; fired after\n"
           "   %.1f ms of spinning (thread CPU used since it was armed: ~30 ms)\n", used);
    CHECK(timer_delete(cpu) == 0);

    printf("   Same idea as an fd: timerfd_create (lesson 13) - read() returns the\n"
           "   expiration count, and it plugs into epoll.\n");
    printf("07_posix_timers: all checks passed\n");
    return 0;
}
