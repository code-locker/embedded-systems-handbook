/*
 * 03_sched_deadline.c — SCHED_DEADLINE: runtime / deadline / period set with
 *                       the raw sched_setattr() syscall, admission control,
 *                       and the EPERM path for normal users
 *
 * Concept : SCHED_DEADLINE (Earliest Deadline First + Constant Bandwidth
 *           Server) is the highest user-visible class: it runs before any
 *           SCHED_FIFO task. Instead of a priority you declare a budget:
 *           "every PERIOD I need at most RUNTIME of CPU, finished by
 *           DEADLINE after the period starts". The kernel ADMITS the task
 *           only if the sum of runtime/period over all DL tasks fits, and
 *           THROTTLES it if it overruns its runtime. glibc 2.35 has no
 *           wrapper, so we call syscall(SYS_sched_setattr) with our own
 *           struct sched_attr. Needs CAP_SYS_NICE (RLIMIT_RTPRIO is not
 *           enough), so as a normal user you see EPERM and an explanation.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/03_sched_deadline.c
 * Read    : ../README.md  (section 3.5 "SCHED_DEADLINE")
 */
#define _GNU_SOURCE /* syscall, gettid */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif
#define SCHED_FLAG_DL_OVERRUN 0x04 /* SIGXCPU when the runtime is overrun */

/* Our own copy of the UAPI struct (include/uapi/linux/sched/types.h).
 * Including <linux/sched/types.h> next to glibc's <sched.h> clashes on
 * struct sched_param in some header versions, so every DL example defines
 * it like this. size = 48 (SCHED_ATTR_SIZE_VER0) is the minimum the kernel
 * accepts; newer kernels add util-clamp fields after it. All times in ns. */
struct sched_attr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;      /* SCHED_OTHER/BATCH */
    uint32_t sched_priority; /* SCHED_FIFO/RR */
    uint64_t sched_runtime;  /* SCHED_DEADLINE ... */
    uint64_t sched_deadline;
    uint64_t sched_period;
};

static int sched_setattr(pid_t tid, const struct sched_attr *a, unsigned flags)
{
    return (int)syscall(SYS_sched_setattr, tid, a, flags);
}
static int sched_getattr(pid_t tid, struct sched_attr *a, unsigned size, unsigned flags)
{
    return (int)syscall(SYS_sched_getattr, tid, a, size, flags);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

#define RUNTIME_NS  (2 * 1000 * 1000ULL)  /*  2 ms of CPU ...        */
#define DEADLINE_NS (5 * 1000 * 1000ULL)  /*  ... done within 5 ms   */
#define PERIOD_NS   (10 * 1000 * 1000ULL) /*  ... every 10 ms        */
#define JOBS 10

struct result {
    int err;           /* 0 = became SCHED_DEADLINE, else errno */
    int jobs;          /* periods completed                    */
    double span_ms;    /* wall time for JOBS periods           */
};

/*
 *  one DL "job" per period:
 *
 *   period start          deadline              next period
 *   |<--- runtime --->|      |                       |
 *   |#################|......|.......................|####...
 *   0                2 ms   5 ms                   10 ms
 *       work, then sched_yield() = "job done, sleep until next period"
 *       (overrun the 2 ms and the CBS throttles us until the next period)
 */
static void *dl_thread(void *arg)
{
    struct result *r = arg;
    struct sched_attr a = {
        .size = sizeof a,
        .sched_policy = SCHED_DEADLINE,
        .sched_runtime = RUNTIME_NS,
        .sched_deadline = DEADLINE_NS,
        .sched_period = PERIOD_NS,
    };
    /* Rule checked by the kernel: runtime <= deadline <= period, runtime
     * >= 1024 ns; and the bandwidth 2/10 = 20% must fit the root domain's
     * free DL bandwidth (default: sched_rt_runtime_us/period * nr_cpus). */
    if (sched_setattr(0, &a, 0) != 0) {
        r->err = errno;
        return NULL;
    }
    struct sched_attr g;
    CHECK(sched_getattr(0, &g, sizeof g, 0) == 0);
    CHECK(g.sched_policy == SCHED_DEADLINE && g.sched_runtime == RUNTIME_NS);

    double t0 = now_ms();
    for (int j = 0; j < JOBS; j++) {
        /* "work": ~0.5 ms of spinning, well inside the 2 ms budget */
        double s = now_ms();
        while (now_ms() - s < 0.5) {
        }
        r->jobs++;
        sched_yield(); /* for DL: give up the rest of this period's runtime */
    }
    r->span_ms = now_ms() - t0;
    return NULL;
}

int main(void)
{
    alarm(10);

    /* 1. What does a normal thread look like through sched_getattr()? */
    struct sched_attr me;
    memset(&me, 0, sizeof me);
    CHECK(sched_getattr(0, &me, sizeof me, 0) == 0);
    printf("1) sched_getattr(self): size=%u policy=%u nice=%d prio=%u runtime=%llu\n",
           me.size, me.sched_policy, me.sched_nice, me.sched_priority,
           (unsigned long long)me.sched_runtime);
    CHECK(me.sched_policy != SCHED_DEADLINE);

    /* 2. Invalid parameters are rejected before any permission check is
     * worth discussing: runtime > deadline is EINVAL for everybody. */
    struct sched_attr bad = {.size = sizeof bad, .sched_policy = SCHED_DEADLINE,
                             .sched_runtime = DEADLINE_NS * 2,
                             .sched_deadline = DEADLINE_NS, .sched_period = PERIOD_NS};
    errno = 0;
    int rc = sched_setattr(0, &bad, 0);
    printf("2) runtime > deadline -> %s\n", rc == 0 ? "accepted?!" : strerror(errno));
    CHECK(rc == -1 && (errno == EINVAL || errno == EPERM));

    /* 3. A thread with a valid 2/5/10 ms reservation. */
    printf("3) SCHED_DEADLINE runtime=2 ms deadline=5 ms period=10 ms (20%% of one CPU)\n");
    struct result r = {0};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, dl_thread, &r) == 0);
    CHECK(pthread_join(t, NULL) == 0);

    if (r.err == 0) {
        printf("   admitted: %d jobs in %.1f ms (expect ~%d ms: one job per period)\n",
               r.jobs, r.span_ms, (JOBS - 1) * 10);
        CHECK(r.jobs == JOBS);
        /* JOBS jobs need at least JOBS-1 full periods after the first one */
        CHECK(r.span_ms >= (JOBS - 1) * 10.0 * 0.9);
    } else if (r.err == EPERM) {
        printf("   EPERM: SCHED_DEADLINE needs CAP_SYS_NICE (RLIMIT_RTPRIO does NOT\n"
               "   grant it). Try:  sudo ./prog   or   sudo chrt -d --sched-runtime 2000000 \\\n"
               "     --sched-deadline 5000000 --sched-period 10000000 0 ./your_task\n");
    } else if (r.err == EBUSY) {
        printf("   EBUSY: admission control refused, the DL bandwidth is already\n"
               "   used up (or RT throttling is disabled: sched_rt_runtime_us=-1).\n");
    } else {
        printf("   sched_setattr: %s (kernel without CONFIG_SCHED_DEADLINE?)\n",
               strerror(r.err));
    }

    printf("   DL rules to remember: a DL task cannot fork (EAGAIN unless\n"
           "   SCHED_FLAG_RESET_ON_FORK), its affinity must cover its whole root\n"
           "   domain (use cpusets to partition), and SCHED_FLAG_DL_OVERRUN (0x%x)\n"
           "   sends SIGXCPU on a budget overrun.\n", SCHED_FLAG_DL_OVERRUN);
    printf("03_sched_deadline: all checks passed\n");
    return 0;
}
