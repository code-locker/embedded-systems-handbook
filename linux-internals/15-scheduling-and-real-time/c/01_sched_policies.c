/*
 * 01_sched_policies.c — scheduling policies and priorities: what this kernel
 *                       runs, what an unprivileged thread may change, and
 *                       what needs CAP_SYS_NICE / RLIMIT_RTPRIO
 *
 * Concept : Every thread has a scheduling POLICY (SCHED_OTHER, BATCH, IDLE
 *           in the fair class; SCHED_FIFO, RR in the real-time class;
 *           SCHED_DEADLINE above them) plus a nice value (fair) or a static
 *           priority 1..99 (RT). Policy is per THREAD, not per process.
 *           A normal user may make itself LESS important (nice up, BATCH,
 *           IDLE) but needs a privilege to become MORE important (nice down
 *           past RLIMIT_NICE, any RT policy). The program prints the
 *           kernel's preemption model and RT-throttling settings, then
 *           changes a worker thread step by step and reads every change back.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/01_sched_policies.c
 * Read    : ../README.md  (sections 3.1 "Scheduling classes" and 3.4 "Policies")
 */
#define _GNU_SOURCE /* gettid, SCHED_BATCH, SCHED_IDLE, pthread_setschedparam */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
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

static const char *policy_name(int p)
{
    switch (p & ~SCHED_RESET_ON_FORK) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_BATCH: return "SCHED_BATCH";
    case SCHED_IDLE:  return "SCHED_IDLE";
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    case 6:           return "SCHED_DEADLINE"; /* not in glibc 2.35 sched.h */
    default:          return "?";
    }
}

/* Read the first line of a small /proc or /sys file into buf. Returns 0 on
 * success, -1 if it does not exist or is not readable (e.g. debugfs as a
 * normal user) — callers print "n/a" instead of failing. */
static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char *r = fgets(buf, (int)n, f);
    fclose(f);
    if (!r)
        return -1;
    buf[strcspn(buf, "\n")] = '\0';
    return 0;
}

static void show_kernel(void)
{
    struct utsname u;
    CHECK(uname(&u) == 0);
    printf("1) kernel %s, version string: %s\n", u.release, u.version);

    /* The preemption model is compiled in (and on PREEMPT_DYNAMIC kernels
     * selectable at boot with preempt=none|voluntary|full). uname -v shows
     * "PREEMPT_RT" on a real-time kernel such as the BeagleBone's 6.18-rt,
     * "PREEMPT_DYNAMIC" on Ubuntu's generic kernel. */
    const char *model = strstr(u.version, "PREEMPT_RT")        ? "PREEMPT_RT (fully preemptible, real-time)"
                        : strstr(u.version, "PREEMPT_DYNAMIC") ? "PREEMPT_DYNAMIC (none/voluntary/full chosen at boot)"
                        : strstr(u.version, "PREEMPT")         ? "PREEMPT (full, low-latency desktop)"
                                                               : "PREEMPT_NONE or VOLUNTARY (server)";
    printf("   preemption model : %s\n", model);

    char buf[128];
    /* /sys/kernel/realtime exists (and reads 1) only on PREEMPT_RT kernels. */
    printf("   /sys/kernel/realtime : %s\n",
           read_line("/sys/kernel/realtime", buf, sizeof buf) == 0 ? buf : "absent (not an RT kernel)");
    /* The live choice on a PREEMPT_DYNAMIC kernel, e.g. "none voluntary (full)".
     * debugfs is root-only, so a normal user usually sees n/a. */
    printf("   /sys/kernel/debug/sched/preempt : %s\n",
           read_line("/sys/kernel/debug/sched/preempt", buf, sizeof buf) == 0 ? buf : "n/a (debugfs needs root)");

    int major = 0, minor = 0;
    sscanf(u.release, "%d.%d", &major, &minor);
    printf("   fair class       : %s\n",
           (major > 6 || (major == 6 && minor >= 6)) ? "EEVDF (6.6+): lag + virtual deadline"
                                                     : "CFS: smallest vruntime runs next");

    /* RT throttling: RT tasks together may use at most runtime/period of
     * each CPU (default 950000/1000000 = 95%). The other 5% lets a shell
     * kill a runaway SCHED_FIFO loop. -1 = no limit. */
    char rt[64], per[64], rr[64];
    if (read_line("/proc/sys/kernel/sched_rt_runtime_us", rt, sizeof rt) == 0 &&
        read_line("/proc/sys/kernel/sched_rt_period_us", per, sizeof per) == 0) {
        long r = atol(rt), p = atol(per);
        printf("   RT throttling    : sched_rt_runtime_us=%s / sched_rt_period_us=%s", rt, per);
        if (r >= 0 && p > 0)
            printf("  -> RT may use %.1f%% of each CPU\n", 100.0 * (double)r / (double)p);
        else
            printf("  -> unlimited\n");
    }
    if (read_line("/proc/sys/kernel/sched_rr_timeslice_ms", rr, sizeof rr) == 0)
        printf("   SCHED_RR slice   : %s ms\n", rr);
}

static void show_priority_ranges(void)
{
    printf("2) priority range per policy (sched_get_priority_min/max):\n");
    const int pol[] = {SCHED_OTHER, SCHED_BATCH, SCHED_IDLE, SCHED_FIFO, SCHED_RR};
    for (size_t i = 0; i < sizeof pol / sizeof pol[0]; i++) {
        int lo = sched_get_priority_min(pol[i]);
        int hi = sched_get_priority_max(pol[i]);
        printf("   %-12s %2d .. %2d\n", policy_name(pol[i]), lo, hi);
        if (pol[i] == SCHED_FIFO || pol[i] == SCHED_RR)
            CHECK(lo == 1 && hi == 99); /* Linux: RT static priority 1..99 */
        else
            CHECK(lo == 0 && hi == 0); /* fair class: priority unused, nice instead */
    }
    printf("   (fair-class threads use nice -20..19 instead; ps/top show\n"
           "    kernel 'prio' = 120+nice for fair, 99-rtprio for RT)\n");
}

/* Everything below runs in a worker thread: policy and nice are per-thread
 * on Linux, so the main thread stays SCHED_OTHER nice 0 and we can prove it. */
static void *worker(void *arg)
{
    (void)arg;
    pid_t tid = gettid();
    printf("3) worker thread tid %d\n", (int)tid);

    /* --- nice: setpriority(PRIO_PROCESS, tid) changes ONE thread on Linux
     * (POSIX says process; Linux's "process" here is a task = thread). */
    errno = 0;
    int n0 = getpriority(PRIO_PROCESS, (id_t)tid);
    CHECK(errno == 0);
    CHECK(setpriority(PRIO_PROCESS, (id_t)tid, n0 + 5) == 0); /* always allowed */
    int n1 = getpriority(PRIO_PROCESS, (id_t)tid);
    CHECK(n1 == n0 + 5);
    printf("   nice %d -> %d (raising nice = being nicer: always allowed)\n", n0, n1);

    /* Lowering nice again needs RLIMIT_NICE (limit = 20 - lowest nice) or
     * CAP_SYS_NICE. Ubuntu's default RLIMIT_NICE is 0: not allowed. */
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NICE, &rl) == 0);
    int r = setpriority(PRIO_PROCESS, (id_t)tid, n0);
    printf("   nice back to %d: %s (RLIMIT_NICE soft=%ld)\n", n0,
           r == 0 ? "ok" : strerror(errno), (long)rl.rlim_cur);

    /* --- SCHED_BATCH: "CPU-bound, don't optimise my wakeup latency".
     * Priority must be 0 for every fair-class policy. */
    struct sched_param sp = {.sched_priority = 0};
    CHECK(pthread_setschedparam(pthread_self(), SCHED_BATCH, &sp) == 0);
    CHECK(sched_getscheduler(tid) == SCHED_BATCH);
    printf("   -> %s ok\n", policy_name(sched_getscheduler(tid)));

    /* --- SCHED_IDLE: runs only when nothing else wants the CPU (weight 3,
     * lower than nice 19's 15). Good for log compression, indexing. */
    CHECK(pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) == 0);
    CHECK(sched_getscheduler(tid) == SCHED_IDLE);
    printf("   -> %s ok\n", policy_name(sched_getscheduler(tid)));

    /* --- SCHED_FIFO prio 10: becoming MORE important needs RLIMIT_RTPRIO
     * >= 10 or CAP_SYS_NICE. pthread_* functions return the error number. */
    CHECK(getrlimit(RLIMIT_RTPRIO, &rl) == 0);
    sp.sched_priority = 10;
    int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (e == 0) {
        CHECK(sched_getscheduler(tid) == SCHED_FIFO);
        printf("   -> SCHED_FIFO prio 10 ok (you have RLIMIT_RTPRIO=%ld or CAP_SYS_NICE)\n",
               (long)rl.rlim_cur);
        struct timespec q;
        CHECK(sched_rr_get_interval(tid, &q) == 0);
        printf("      sched_rr_get_interval for FIFO = %ld.%09ld s (0: no slice)\n",
               (long)q.tv_sec, q.tv_nsec);
    } else {
        CHECK(e == EPERM);
        printf("   -> SCHED_FIFO prio 10: EPERM (RLIMIT_RTPRIO=%ld, no CAP_SYS_NICE).\n"
               "      Grant it with one of:\n"
               "        /etc/security/limits.d/rt.conf:  @realtime - rtprio 95   (+ memlock unlimited)\n"
               "        sudo setcap cap_sys_nice+ep ./prog\n"
               "        sudo chrt -f 10 ./prog         (or just run on the board as root)\n",
               (long)rl.rlim_cur);
    }

    /* Back to OTHER. Leaving SCHED_IDLE counts as "more important", so an
     * unprivileged thread may do it only if its nice is within RLIMIT_NICE
     * (allowed when 20 - nice <= RLIMIT_NICE). With Ubuntu's RLIMIT_NICE=0
     * no nice value qualifies: SCHED_IDLE is a ONE-WAY door without
     * CAP_SYS_NICE. That is why this demo runs in a throwaway thread. */
    sp.sched_priority = 0;
    e = pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
    if (e == 0)
        printf("   back to SCHED_OTHER: ok\n");
    else {
        CHECK(e == EPERM);
        printf("   back to SCHED_OTHER: EPERM -- SCHED_IDLE is one-way without\n"
               "      CAP_SYS_NICE or RLIMIT_NICE >= 20-nice (this thread just ends)\n");
    }

    /* sched_rr_get_interval for a fair task: the kernel returns its current
     * slice estimate (EEVDF base slice, ~3 ms) — informational only. */
    struct timespec q;
    CHECK(sched_rr_get_interval(tid, &q) == 0);
    printf("   sched_rr_get_interval (fair) = %.3f ms\n",
           (double)q.tv_sec * 1e3 + (double)q.tv_nsec / 1e6);
    return NULL;
}

int main(void)
{
    alarm(10); /* watchdog: nothing here should ever block */
    const int pol_start = sched_getscheduler(0);
    show_kernel();
    show_priority_ranges();

    pthread_t t;
    CHECK(pthread_create(&t, NULL, worker, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);

    /* The worker's policy and nice changes did not touch the main thread. */
    errno = 0;
    int pol = sched_getscheduler(0);
    int nice_main = getpriority(PRIO_PROCESS, (id_t)gettid());
    CHECK(errno == 0);
    printf("4) main thread still %s nice %d: policy is per THREAD\n",
           policy_name(pol), nice_main);
    CHECK(pol == pol_start); /* whatever we were started with (chrt may change it) */

    printf("   SCHED_RESET_ON_FORK (or-ed into the policy) makes children start\n"
           "   as SCHED_OTHER nice>=0, so an RT parent cannot leak RT to helpers.\n");
    printf("01_sched_policies: all checks passed\n");
    return 0;
}
