/*
 * 09_rt_app_recipe.c — the real-time application recipe, step by step:
 *                      lock memory, prefault, keep malloc from returning
 *                      memory, SCHED_FIFO, CPU pinning, PI mutex to a
 *                      non-RT thread, an allocation-free periodic loop and
 *                      a software watchdog; reports what succeeded
 *
 * Concept : A real-time thread on Linux misses deadlines for boring
 *           reasons: a page fault (disk or zeroing a page = 10 us..ms), a
 *           malloc that calls mmap/brk, a printf that blocks on a pipe, a
 *           lower-priority thread holding a lock (priority inversion), the
 *           CPU being shared with IRQs and housekeeping. The recipe removes
 *           each cause before the loop starts, so the loop itself only
 *           sleeps (absolute), computes and hands data off. Every step
 *           that needs privilege (SCHED_FIFO, big mlock) degrades
 *           gracefully and the final table says how to grant it.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/09_rt_app_recipe.c
 * Read    : ../README.md  (section 3.12 "The RT application recipe")
 */
#define _GNU_SOURCE /* pthread_attr_setaffinity_np, RUSAGE_THREAD, CPU_SET */
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
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

#define NSEC_PER_SEC   1000000000LL
#define PERIOD_NS      (1000 * 1000LL) /* 1 kHz loop */
#define CYCLES         300
#define RT_PRIO        80
/* Explicit stack size, so we know what to prefault. Not too small: glibc
 * carves static TLS out of the top of the thread stack, and TSan's
 * per-thread state lives there (hundreds of KiB) — a 512 KiB stack
 * overflows under TSan before the thread function's first line. */
#define RT_STACK       (2 * 1024 * 1024)
#define PREFAULT_STACK (256 * 1024)
#define HEAP_POOL      (1024 * 1024)
#define RING           512             /* power of two */

/* ---- step report ---------------------------------------------------- */
enum { ST_OK, ST_SKIP, ST_FAIL };
static struct { const char *step; int st; char note[160]; } report[12];
static int nreport;
__attribute__((format(printf, 3, 4))) static void step(const char *name, int st,
                                                     const char *fmt, ...)
{
    report[nreport].step = name;
    report[nreport].st = st;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(report[nreport].note, sizeof report[0].note, fmt, ap);
    va_end(ap);
    nreport++;
}

/* ---- data shared between the RT thread and the non-RT logger -------- */
static pthread_mutex_t ring_lock;  /* PTHREAD_PRIO_INHERIT, see step 6 */
static int32_t ring[RING];         /* latency samples in ns, guarded by ring_lock */
static unsigned ring_head, ring_tail; /* guarded by ring_lock */
static unsigned dropped;              /* guarded by ring_lock */
static atomic_uint heartbeat;         /* RT loop -> watchdog, lock-free */
static atomic_bool rt_done;

static unsigned char *pool;           /* preallocated "heap" for the loop */

static void ts_add_ns(struct timespec *t, int64_t add)
{
    t->tv_nsec += (long)add;
    while (t->tv_nsec >= NSEC_PER_SEC) {
        t->tv_nsec -= NSEC_PER_SEC;
        t->tv_sec++;
    }
}
static int64_t ts_ns(const struct timespec *t) { return (int64_t)t->tv_sec * NSEC_PER_SEC + t->tv_nsec; }
static long minflt_thread(void)
{
    struct rusage ru;
    CHECK(getrusage(RUSAGE_THREAD, &ru) == 0);
    return ru.ru_minflt;
}

/* Step 3b: touch the stack we will use, so its pages are mapped (and, with
 * mlockall MCL_FUTURE, locked) before the loop. noinline + the volatile
 * write stop the compiler from removing the "useless" array. */
__attribute__((noinline)) static void prefault_stack(void)
{
    volatile unsigned char buf[PREFAULT_STACK];
    for (size_t i = 0; i < sizeof buf; i += 4096)
        buf[i] = 1;
}

struct rt_result {
    long faults_in_loop;
    int policy;
    int cpu;
};

/*
 *  RT thread                          logger thread (SCHED_OTHER)
 *  -----------                        ---------------------------
 *  loop: sleep_until(next)            every 10 ms:
 *        work on preallocated pool       lock(ring_lock)  <- PI: if RT waits,
 *        lock(ring_lock) - push - unlock     copy samples      logger is boosted
 *        heartbeat++                     unlock
 *        (no malloc, no printf, no         printf / write to disk (slow, OK here)
 *         file I/O, no new pages)
 */
static void *rt_thread(void *arg)
{
    struct rt_result *r = arg;
    prefault_stack();
    struct sched_param sp;
    CHECK(pthread_getschedparam(pthread_self(), &r->policy, &sp) == 0);
    r->cpu = sched_getcpu();
    prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0); /* matters only for SCHED_OTHER */

    long f0 = minflt_thread();
    struct timespec next, now;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (int i = 0; i < CYCLES; i++) {
        ts_add_ns(&next, PERIOD_NS);
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        int32_t lat = (int32_t)(ts_ns(&now) - ts_ns(&next));

        /* "work": touch the preallocated pool (already faulted in) */
        pool[(unsigned)i * 2048u % HEAP_POOL] ^= (unsigned char)lat;

        pthread_mutex_lock(&ring_lock);
        if (ring_head - ring_tail < RING)
            ring[ring_head++ % RING] = lat;
        else
            dropped++; /* never block the RT side on a slow consumer */
        pthread_mutex_unlock(&ring_lock);

        atomic_fetch_add_explicit(&heartbeat, 1, memory_order_relaxed);
    }
    r->faults_in_loop = minflt_thread() - f0;
    atomic_store_explicit(&rt_done, true, memory_order_release);
    return NULL;
}

struct log_result {
    unsigned samples;
    int64_t max_ns;
};

static void *logger_thread(void *arg)
{
    struct log_result *lr = arg;
    for (;;) {
        bool done = atomic_load_explicit(&rt_done, memory_order_acquire);
        int32_t local[RING];
        unsigned n = 0;
        pthread_mutex_lock(&ring_lock);
        while (ring_tail != ring_head)
            local[n++] = ring[ring_tail++ % RING];
        pthread_mutex_unlock(&ring_lock);
        for (unsigned k = 0; k < n; k++) /* slow work happens OUTSIDE the lock */
            if (local[k] > lr->max_ns)
                lr->max_ns = local[k];
        lr->samples += n;
        if (done && n == 0)
            return NULL;
        struct timespec d = {0, 10 * 1000 * 1000};
        nanosleep(&d, NULL);
    }
}

static long read_long_file(const char *path, char *buf, size_t n)
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

int main(void)
{
    alarm(20);
    char buf[256];
    struct rlimit rl_rt, rl_ml;
    CHECK(getrlimit(RLIMIT_RTPRIO, &rl_rt) == 0);
    CHECK(getrlimit(RLIMIT_MEMLOCK, &rl_ml) == 0);
    printf("0) limits: RLIMIT_RTPRIO=%ld, RLIMIT_MEMLOCK=%ld KiB\n", (long)rl_rt.rlim_cur,
           rl_ml.rlim_cur == RLIM_INFINITY ? -1L : (long)(rl_ml.rlim_cur / 1024));
    printf("   isolated CPUs  : %s\n",
           read_long_file("/sys/devices/system/cpu/isolated", buf, sizeof buf) == 0 && buf[0] ? buf : "(none)");
    /* nohz_full reads "(null)" when the feature is compiled in but unused */
    printf("   nohz_full CPUs : %s\n",
           read_long_file("/sys/devices/system/cpu/nohz_full", buf, sizeof buf) == 0 &&
                   buf[0] && !strstr(buf, "null") ? buf : "(none)");
    printf("   default IRQ affinity mask: %s\n",
           read_long_file("/proc/irq/default_smp_affinity", buf, sizeof buf) == 0 ? buf : "n/a");

    /* Step 1: malloc must never give memory back to the kernel (a later
     * malloc would fault in fresh pages) nor use mmap for big blocks. */
    int m1 = mallopt(M_TRIM_THRESHOLD, -1);
    int m2 = mallopt(M_MMAP_MAX, 0);
    step("1 mallopt(no trim, no mmap)", (m1 && m2) ? ST_OK : ST_SKIP,
         (m1 && m2) ? "glibc keeps freed memory, big blocks from the heap"
                    : "allocator ignores mallopt (sanitizer malloc)");

    /* Step 2: lock all current and future pages in RAM: no page-out, no
     * fault-in on first touch of an already-mapped page. */
    if (UNDER_SANITIZER)
        step("2 mlockall(CURRENT|FUTURE)", ST_SKIP,
             "skipped under a sanitizer: TBs of shadow mappings");
    else if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0)
        step("2 mlockall(CURRENT|FUTURE)", ST_OK, "all pages locked");
    else
        step("2 mlockall(CURRENT|FUTURE)", ST_FAIL, "%s -> limits.conf: '@realtime - memlock unlimited'",
             strerror(errno));

    /* Step 3a: preallocate and TOUCH the working memory now. malloc only
     * reserves address space; memset makes the kernel back every page. */
    pool = malloc(HEAP_POOL);
    CHECK(pool != NULL);
    long f0 = minflt_thread();
    memset(pool, 0, HEAP_POOL);
    long fpre = minflt_thread() - f0;
    snprintf(buf, sizeof buf, "%ld minor faults taken NOW instead of in the loop", fpre);
    step("3 prefault heap pool + stack", ST_OK, "%s", buf);

    /* Step 4 + 5: SCHED_FIFO thread pinned to one CPU, with a known stack.
     * PTHREAD_EXPLICIT_SCHED or the policy in attr is ignored. */
    cpu_set_t allowed, one;
    CHECK(sched_getaffinity(0, sizeof allowed, &allowed) == 0);
    int cpu = -1;
    for (int c = CPU_SETSIZE - 1; c >= 0 && cpu < 0; c--)
        if (CPU_ISSET(c, &allowed))
            cpu = c;
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);

    /* Step 6: the mutex shared with a non-RT thread uses priority
     * inheritance, so the logger cannot cause priority inversion
     * (lesson 03, section 3.6). On PREEMPT_RT the kernel's own locks do
     * the same. */
    pthread_mutexattr_t ma;
    CHECK(pthread_mutexattr_init(&ma) == 0);
    CHECK(pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT) == 0);
    CHECK(pthread_mutex_init(&ring_lock, &ma) == 0);
    int proto;
    CHECK(pthread_mutexattr_getprotocol(&ma, &proto) == 0 && proto == PTHREAD_PRIO_INHERIT);
    pthread_mutexattr_destroy(&ma);
    step("6 PI mutex RT <-> logger", ST_OK, "PTHREAD_PRIO_INHERIT (FUTEX_LOCK_PI on contention)");

    pthread_attr_t a;
    CHECK(pthread_attr_init(&a) == 0);
    CHECK(pthread_attr_setstacksize(&a, RT_STACK) == 0);
    CHECK(pthread_attr_setaffinity_np(&a, sizeof one, &one) == 0);
    CHECK(pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED) == 0);
    CHECK(pthread_attr_setschedpolicy(&a, SCHED_FIFO) == 0);
    struct sched_param sp = {.sched_priority = RT_PRIO};
    CHECK(pthread_attr_setschedparam(&a, &sp) == 0);

    struct rt_result rr = {0};
    struct log_result lr = {0};
    pthread_t rt, lg;
    CHECK(pthread_create(&lg, NULL, logger_thread, &lr) == 0);
    int e = pthread_create(&rt, &a, rt_thread, &rr);
    if (e == EPERM) {
        step("4 SCHED_FIFO 80", ST_FAIL,
             "EPERM -> limits.conf 'rtprio 95', setcap cap_sys_nice+ep, or run as root");
        CHECK(pthread_attr_setinheritsched(&a, PTHREAD_INHERIT_SCHED) == 0);
        e = pthread_create(&rt, &a, rt_thread, &rr);
    } else if (e == 0) {
        step("4 SCHED_FIFO 80", ST_OK, "explicit sched attr accepted");
    }
    CHECK(e == 0);
    pthread_attr_destroy(&a);

    /* Step 7: software watchdog. The RT loop bumps `heartbeat`; we check it
     * every 20 ms. If it stops advancing for 500 ms the RT side is stuck
     * (or dead). On the board you would then stop the motor / reset; a
     * hardware /dev/watchdog (petted with WDIOC_KEEPALIVE) catches the case
     * where the whole system hangs, systemd's WatchdogSec= a hung daemon. */
    unsigned last = 0, stalls = 0, stalled_ms = 0;
    bool detected_stop = false;
    for (int k = 0; k < 500 && !detected_stop; k++) { /* <= 10 s */
        struct timespec d = {0, 20 * 1000 * 1000};
        nanosleep(&d, NULL);
        unsigned hb = atomic_load_explicit(&heartbeat, memory_order_relaxed);
        if (hb != last) {
            last = hb;
            stalled_ms = 0;
            continue;
        }
        stalled_ms += 20;
        if (stalled_ms >= 100 && atomic_load_explicit(&rt_done, memory_order_acquire))
            detected_stop = true; /* the expected end: the loop finished */
        else if (stalled_ms == 500)
            stalls++;            /* a real stall while it should be running */
    }
    CHECK(pthread_join(rt, NULL) == 0);
    CHECK(pthread_join(lg, NULL) == 0);
    snprintf(buf, sizeof buf, "heartbeat=%u, stalls=%u, end of loop detected=%s",
             atomic_load(&heartbeat), stalls, detected_stop ? "yes" : "no");
    step("7 software watchdog", detected_stop ? ST_OK : ST_FAIL, "%s", buf);

    snprintf(buf, sizeof buf, "ran on CPU %d (pinned), %s", rr.cpu,
             rr.policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_OTHER");
    step("5 pin to one CPU", rr.cpu == cpu ? ST_OK : ST_FAIL, "%s", buf);
    snprintf(buf, sizeof buf, "%d cycles, %ld minor faults inside the loop%s, max latency %.1f us",
             CYCLES, rr.faults_in_loop, UNDER_SANITIZER ? " (sanitizer shadow pages count)" : "",
             (double)lr.max_ns / 1e3);
    step("8 allocation-free RT loop", ST_OK, "%s", buf);

    printf("\n   %-30s %-5s %s\n", "step", "", "result");
    const char *tag[] = {"ok", "skip", "NO"}; /* NO = needs privilege or setup */
    /* steps were recorded as they completed; print them in recipe order */
    for (int i = 1; i < nreport; i++)
        for (int j = i; j > 0 && strcmp(report[j - 1].step, report[j].step) > 0; j--) {
            __typeof__(report[0]) tmp = report[j];
            report[j] = report[j - 1];
            report[j - 1] = tmp;
        }
    for (int i = 0; i < nreport; i++)
        printf("   %-30s %-5s %s\n", report[i].step, tag[report[i].st], report[i].note);

    CHECK(rr.cpu == cpu);                       /* affinity attr works unprivileged */
    CHECK(atomic_load(&heartbeat) == CYCLES);   /* every cycle ran */
    CHECK(lr.samples + dropped == CYCLES);      /* every sample handed off or counted */
    CHECK(detected_stop && stalls == 0);
    printf("\n   For an isolated RT core on the board, boot with:\n"
           "     isolcpus=1 nohz_full=1 rcu_nocbs=1 irqaffinity=0\n"
           "   then pin the RT thread to CPU 1 and everything else (and IRQs) to CPU 0.\n");

    pthread_mutex_destroy(&ring_lock);
    free(pool);
    if (!UNDER_SANITIZER)
        munlockall();
    printf("09_rt_app_recipe: all checks passed\n");
    return 0;
}
