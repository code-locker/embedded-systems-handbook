/*
 * 04_clocks.c — the Linux clocks: REALTIME, MONOTONIC, MONOTONIC_RAW,
 *               BOOTTIME, PROCESS_CPUTIME_ID, THREAD_CPUTIME_ID; their
 *               resolution, cost, and which one to use for what
 *
 * Concept : "What time is it?" has several answers. REALTIME is the wall
 *           clock: it can JUMP (NTP step, date -s, RTC sync at boot), so
 *           never measure intervals or set deadlines with it. MONOTONIC
 *           never jumps but is slewed by NTP (rate adjusted by up to
 *           500 ppm) and stops during suspend. MONOTONIC_RAW is the raw
 *           hardware counter (arch timer on the A72, TSC on x86): no NTP
 *           slew. BOOTTIME = MONOTONIC + time spent suspended. The CPUTIME
 *           clocks count only time the process / thread actually RAN, so
 *           sleeping does not advance them. clock_gettime() of the first
 *           four is a vDSO call: no syscall, ~20 ns.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/04_clocks.c
 * Read    : ../README.md  (section 3.8 "Clocks")
 */
#define _GNU_SOURCE /* CLOCK_BOOTTIME, CLOCK_MONOTONIC_RAW, pthread_getcpuclockid */
#include <errno.h>
#include <pthread.h>
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

static int64_t ns(struct timespec t) { return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }

static int64_t now(clockid_t c)
{
    struct timespec t;
    CHECK(clock_gettime(c, &t) == 0);
    return ns(t);
}

static void sleep_ms(long ms)
{
    struct timespec d = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&d, &d) == -1 && errno == EINTR) {
    }
}

struct clk {
    clockid_t id;
    const char *name;
    const char *use;
};

static const struct clk clocks[] = {
    {CLOCK_REALTIME, "CLOCK_REALTIME", "wall time for humans/logs; JUMPS"},
    {CLOCK_MONOTONIC, "CLOCK_MONOTONIC", "intervals, timeouts, deadlines"},
    {CLOCK_MONOTONIC_RAW, "CLOCK_MONOTONIC_RAW", "raw HW counter, no NTP slew"},
    {CLOCK_BOOTTIME, "CLOCK_BOOTTIME", "monotonic incl. suspend"},
    {CLOCK_PROCESS_CPUTIME_ID, "CLOCK_PROCESS_CPUTIME_ID", "CPU used by all threads"},
    {CLOCK_THREAD_CPUTIME_ID, "CLOCK_THREAD_CPUTIME_ID", "CPU used by this thread"},
};
#define NCLK (sizeof clocks / sizeof clocks[0])

/* A busy thread that burns ~80 ms of CPU, so the process CPU clock moves
 * faster than the main thread's own CPU clock. */
static void *burner(void *arg)
{
    (void)arg;
    int64_t start = now(CLOCK_THREAD_CPUTIME_ID);
    while (now(CLOCK_THREAD_CPUTIME_ID) - start < 80 * 1000000LL) {
    }
    return NULL;
}

int main(void)
{
    alarm(10);

    printf("1) resolution, current value, cost per call:\n");
    printf("   %-26s %10s %22s %8s  %s\n", "clock", "res (ns)", "value (s)", "ns/call", "use for");
    for (size_t i = 0; i < NCLK; i++) {
        struct timespec res, v;
        CHECK(clock_getres(clocks[i].id, &res) == 0);
        CHECK(clock_gettime(clocks[i].id, &v) == 0);
        /* Cost: the first four go through the vDSO (a user-space read of
         * a kernel-shared page + the hardware counter); the CPUTIME ones
         * are real syscalls. Printed only: it varies by machine. */
        enum { N = 20000 };
        int64_t t0 = now(CLOCK_MONOTONIC);
        for (int k = 0; k < N; k++)
            CHECK(clock_gettime(clocks[i].id, &v) == 0);
        double per = (double)(now(CLOCK_MONOTONIC) - t0) / N;
        printf("   %-26s %10ld %11lld.%09ld %8.1f  %s\n", clocks[i].name, res.tv_nsec,
               (long long)v.tv_sec, v.tv_nsec, per, clocks[i].use);
        /* High-resolution timers: the kernel reports 1 ns for the hrtimer
         * clocks. Every Linux clock has resolution <= 1 jiffy (<= 10 ms). */
        CHECK(res.tv_sec == 0 && res.tv_nsec > 0 && res.tv_nsec <= 10000000);
    }

    /* 2. MONOTONIC never goes backwards: many consecutive reads. */
    int64_t prev = now(CLOCK_MONOTONIC);
    for (int k = 0; k < 100000; k++) {
        int64_t cur = now(CLOCK_MONOTONIC);
        CHECK(cur >= prev);
        prev = cur;
    }
    printf("2) 100000 consecutive CLOCK_MONOTONIC reads never went backwards\n");

    /* 3. BOOTTIME - MONOTONIC = total time this machine has been suspended. */
    int64_t mono = now(CLOCK_MONOTONIC), boot = now(CLOCK_BOOTTIME);
    CHECK(boot >= mono);
    printf("3) BOOTTIME - MONOTONIC = %.3f s spent in suspend since boot\n",
           (double)(boot - mono) / 1e9);

    /* 4. MONOTONIC vs MONOTONIC_RAW over 200 ms: the difference is the NTP
     * frequency correction currently applied (ppm). 0 if NTP is not
     * running (typical on a board without network time). */
    int64_t m0 = now(CLOCK_MONOTONIC), r0 = now(CLOCK_MONOTONIC_RAW);
    sleep_ms(200);
    int64_t m1 = now(CLOCK_MONOTONIC), r1 = now(CLOCK_MONOTONIC_RAW);
    double ppm = ((double)(m1 - m0) - (double)(r1 - r0)) / (double)(r1 - r0) * 1e6;
    printf("4) over %.0f ms: MONOTONIC vs MONOTONIC_RAW rate differs by %.1f ppm (NTP slew)\n",
           (double)(r1 - r0) / 1e6, ppm);
    CHECK(r1 > r0 && m1 > m0);

    /* 5. REALTIME is "now" in seconds since 1970; time() agrees with it. */
    time_t t = time(NULL);
    int64_t rt = now(CLOCK_REALTIME);
    printf("5) CLOCK_REALTIME %lld s vs time() %lld s; offset REALTIME-MONOTONIC = %.0f s\n"
           "   (a board without an RTC boots at 1970 and jumps ~56 years when NTP syncs:\n"
           "    every REALTIME deadline in flight expires at once)\n",
           (long long)(rt / 1000000000LL), (long long)t,
           (double)(rt - now(CLOCK_MONOTONIC)) / 1e9);
    CHECK(llabs((long long)(rt / 1000000000LL) - (long long)t) <= 1);

    /* 6. CPU-time clocks: sleeping costs no CPU; another thread's work
     * counts for the PROCESS clock but not for MY thread clock. */
    int64_t th0 = now(CLOCK_THREAD_CPUTIME_ID), pr0 = now(CLOCK_PROCESS_CPUTIME_ID);
    int64_t w0 = now(CLOCK_MONOTONIC);
    pthread_t b;
    CHECK(pthread_create(&b, NULL, burner, NULL) == 0);
    sleep_ms(50);
    CHECK(pthread_join(b, NULL) == 0);
    int64_t th1 = now(CLOCK_THREAD_CPUTIME_ID), pr1 = now(CLOCK_PROCESS_CPUTIME_ID);
    int64_t w1 = now(CLOCK_MONOTONIC);
    printf("6) while main slept and a burner thread spun ~80 ms of CPU:\n"
           "   wall %.1f ms, process CPU +%.1f ms, main-thread CPU +%.1f ms\n",
           (double)(w1 - w0) / 1e6, (double)(pr1 - pr0) / 1e6, (double)(th1 - th0) / 1e6);
    CHECK(pr1 - pr0 >= 80 * 1000000LL);   /* the burner's 80 ms is in there   */
    CHECK(th1 - th0 < pr1 - pr0);         /* main's own share is smaller      */

    /* Any thread's CPU clock can be read from outside: pthread_getcpuclockid
     * (and clock_getcpuclockid(pid) for another process). */
    clockid_t self_cpu;
    CHECK(pthread_getcpuclockid(pthread_self(), &self_cpu) == 0);
    printf("   pthread_getcpuclockid(self) -> clock id %d, reads %.1f ms\n",
           (int)self_cpu, (double)now(self_cpu) / 1e6);
    printf("04_clocks: all checks passed\n");
    return 0;
}
