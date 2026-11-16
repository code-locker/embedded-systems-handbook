/*
 * 04_scheduling_and_affinity.c — scheduling policies, priorities, per-thread
 *                                nice, real-time attempt, CPU affinity
 *
 * Concept : The Linux scheduler schedules TASKS (threads), not processes.
 *           Normal threads use SCHED_OTHER (CFS/EEVDF) and a nice value;
 *           real-time threads use SCHED_FIFO / SCHED_RR with a priority
 *           1..99 and always beat normal threads. Asking for real-time
 *           needs CAP_SYS_NICE (or an RLIMIT_RTPRIO), so this program
 *           tries and skips politely on EPERM. CPU affinity pins a thread
 *           to a subset of cores.
 * Build   : make run P=linux-internals/01-processes-and-threads/c/04_scheduling_and_affinity.c
 * Read    : ../README.md  (section 3.10)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static const char *policy_name(int p)
{
    switch (p) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    case SCHED_BATCH: return "SCHED_BATCH";
    case SCHED_IDLE:  return "SCHED_IDLE";
    default:          return "?";
    }
}

/* ===== 1. what am I running with? ====================================== */
static void demo_policies(void)
{
    printf("[1] policies and priority ranges\n");
    int pol;
    struct sched_param sp;
    CHECK(pthread_getschedparam(pthread_self(), &pol, &sp) == 0);
    printf("    main thread: %s, sched_priority %d, nice %d\n", policy_name(pol),
           sp.sched_priority, getpriority(PRIO_PROCESS, 0));

    /*
     *   priority  99 ┐
     *             .. │ SCHED_FIFO / SCHED_RR   (real-time; higher wins,
     *              1 ┘                          always preempts below)
     *   -------------------------------------------------------------
     *   nice -20..19   SCHED_OTHER / BATCH     (sched_priority is 0;
     *                                           nice only changes the SHARE
     *                                           of CPU time, not who wins)
     *   lowest         SCHED_IDLE
     */
    const int pols[] = {SCHED_OTHER, SCHED_FIFO, SCHED_RR};
    for (size_t i = 0; i < sizeof pols / sizeof pols[0]; i++)
        printf("    %-11s priority range %d..%d\n", policy_name(pols[i]),
               sched_get_priority_min(pols[i]), sched_get_priority_max(pols[i]));
    CHECK(sched_get_priority_min(SCHED_FIFO) == 1);
    CHECK(sched_get_priority_max(SCHED_FIFO) == 99);

    struct timespec ts;
    if (sched_rr_get_interval(0, &ts) == 0)
        /* For a SCHED_RR task this is its quantum (default
         * /proc/sys/kernel/sched_rr_timeslice_ms = 100); for a normal task
         * Linux reports its current fair-share slice instead. */
        printf("    sched_rr_get_interval(self) = %ld ms\n",
               ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ===== 2. nice is per-THREAD on Linux ================================== */
typedef struct {
    int before, after, err;
} NiceResult;

static void *nicer_thread(void *arg)
{
    NiceResult *r = arg;
    /* POSIX says nice applies to the process; Linux applies it to the
     * single task named by the TID. Raising nice (being nicer) is always
     * allowed; lowering it needs privilege. */
    pid_t tid = gettid();
    errno = 0;
    r->before = getpriority(PRIO_PROCESS, (id_t)tid);
    if (r->before >= 19) { /* already as nice as it gets */
        r->after = r->before;
        return NULL;
    }
    if (setpriority(PRIO_PROCESS, (id_t)tid, r->before + 1) != 0)
        r->err = errno;
    r->after = getpriority(PRIO_PROCESS, (id_t)tid);
    return NULL;
}

static void demo_nice(void)
{
    printf("\n[2] per-thread nice (Linux-specific)\n");
    int main_before = getpriority(PRIO_PROCESS, 0);
    NiceResult r = {0};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, nicer_thread, &r) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    int main_after = getpriority(PRIO_PROCESS, 0);
    printf("    worker nice %d -> %d, main stays %d -> %d\n", r.before, r.after,
           main_before, main_after);
    CHECK(r.err == 0);
    CHECK(r.before >= 19 || r.after == r.before + 1);
    CHECK(main_after == main_before); /* only the worker's task changed */
}

/* ===== 3. real-time: try, and skip without privilege =================== */
static void *rt_thread(void *arg)
{
    int *out_policy = arg;
    struct sched_param sp;
    CHECK(pthread_getschedparam(pthread_self(), out_policy, &sp) == 0);
    return NULL;
}

static void demo_realtime(void)
{
    printf("\n[3] SCHED_FIFO via thread attributes\n");
    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr) == 0);
    /* Without EXPLICIT_SCHED the new thread INHERITS the creator's policy
     * and silently ignores the policy in attr — a classic mistake. */
    CHECK(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0);
    CHECK(pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == 0);
    struct sched_param sp = {.sched_priority = 10};
    CHECK(pthread_attr_setschedparam(&attr, &sp) == 0);

    pthread_t t;
    int pol = -1;
    int rc = pthread_create(&t, &attr, rt_thread, &pol);
    if (rc == EPERM) {
        printf("    pthread_create -> EPERM: real-time needs CAP_SYS_NICE (root)\n"
               "    or `ulimit -r` > 0 (RLIMIT_RTPRIO). Skipping this part.\n"
               "    On the board: sudo chrt -f 10 ./prog, or set\n"
               "    LimitRTPRIO= in the systemd unit.\n");
    } else {
        CHECK(rc == 0);
        CHECK(pthread_join(t, NULL) == 0);
        printf("    created a %s thread at priority %d\n", policy_name(pol),
               sp.sched_priority);
        CHECK(pol == SCHED_FIFO);
    }
    CHECK(pthread_attr_destroy(&attr) == 0);
}

/* ===== 4. CPU affinity ================================================= */
typedef struct {
    int target_cpu;
    int rc;
    int ran_on;
} PinResult;

static void *pinned_thread(void *arg)
{
    PinResult *r = arg;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(r->target_cpu, &set);
    r->rc = pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    /* After a successful call the kernel migrates us before returning, so
     * from now on we only ever run on target_cpu. */
    r->ran_on = sched_getcpu();
    return NULL;
}

static void demo_affinity(void)
{
    printf("\n[4] CPU affinity\n");
    cpu_set_t allowed;
    CHECK(sched_getaffinity(0, sizeof allowed, &allowed) == 0);
    int first = -1, last = -1;
    for (int c = 0; c < CPU_SETSIZE; c++)
        if (CPU_ISSET(c, &allowed)) {
            if (first < 0)
                first = c;
            last = c;
        }
    printf("    allowed CPUs: %d (first %d, last %d), online: %ld\n",
           CPU_COUNT(&allowed), first, last, sysconf(_SC_NPROCESSORS_ONLN));
    CHECK(first >= 0);

    /* Pin to the LAST allowed CPU (inside a container/cpuset that may not be
     * CPU 0). On the TDA4VM there are only CPUs 0 and 1 (2x A72). */
    PinResult r = {.target_cpu = last};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, pinned_thread, &r) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    if (r.rc != 0) {
        printf("    pthread_setaffinity_np failed: %s — skipping\n", strerror(r.rc));
        return;
    }
    printf("    worker pinned to CPU %d, sched_getcpu() says %d\n", r.target_cpu,
           r.ran_on);
    CHECK(r.ran_on == r.target_cpu);

    /* The main thread's mask was not changed: affinity is per thread too
     * (new threads inherit the creator's mask at creation time). */
    cpu_set_t now;
    CHECK(sched_getaffinity(0, sizeof now, &now) == 0);
    CHECK(CPU_EQUAL(&now, &allowed));
}

int main(void)
{
    demo_policies();
    demo_nice();
    demo_realtime();
    demo_affinity();
    printf("\n04_scheduling_and_affinity: all checks passed\n");
    return 0;
}
