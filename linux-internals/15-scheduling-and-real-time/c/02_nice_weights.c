/*
 * 02_nice_weights.c — nice values are WEIGHTS: two CPU-bound threads pinned
 *                     to the same CPU share it in proportion 1024 : weight
 *
 * Concept : The fair class (CFS, EEVDF since 6.6) does not give a nice-5
 *           thread "lower priority" in the RT sense; it gives it a smaller
 *           WEIGHT. Each nice step is ~1.25x, so nice 0 (1024) vs nice 5
 *           (335) should split one CPU about 75% : 25%. Each thread's
 *           vruntime advances at (real runtime * 1024 / weight), and the
 *           scheduler keeps vruntimes level, so the lighter thread gets
 *           less real time. We pin both threads to ONE CPU (otherwise
 *           each gets its own core and nice does nothing), let them spin
 *           for ~400 ms, and compare CPU time and loop counts. The ratio is
 *           printed; we only assert the nicer thread did not get MORE.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/c/02_nice_weights.c
 * Read    : ../README.md  (section 3.2 "The fair class: vruntime, weights, EEVDF")
 */
#define _GNU_SOURCE /* gettid, CPU_SET, pthread_setaffinity_np */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* kernel/sched/core.c: sched_prio_to_weight[40], index = nice + 20.
 * Each step is ~1.25x, so +1 nice = ~10% less CPU when competing with one
 * other thread. The table is what makes "nice" multiplicative. */
static const int prio_to_weight[40] = {
    /* -20 */ 88761, 71755, 56483, 46273, 36291,
    /* -15 */ 29154, 23254, 18705, 14949, 11916,
    /* -10 */  9548,  7620,  6100,  4904,  3906,
    /*  -5 */  3121,  2501,  1991,  1586,  1277,
    /*   0 */  1024,   820,   655,   526,   423,
    /*   5 */   335,   272,   215,   172,   137,
    /*  10 */   110,    87,    70,    56,    45,
    /*  15 */    36,    29,    23,    18,    15,
};

#define RUN_MS 400

struct spinner {
    int nice;                 /* set before start, read-only after       */
    int cpu;                  /* the one CPU both threads are pinned to  */
    pthread_barrier_t *start; /* both begin spinning at the same moment  */
    atomic_bool *stop;        /* main sets it; spinners poll it relaxed  */
    unsigned long loops;      /* written by the thread, read after join  */
    double cpu_ms;            /* CLOCK_THREAD_CPUTIME_ID at the end      */
};

static double ts_ms(struct timespec t) { return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6; }

static void *spin(void *arg)
{
    struct spinner *s = arg;

    /* Pin: both threads compete for ONE CPU, or nice has nothing to share. */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(s->cpu, &set);
    CHECK(pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0);
    /* Thread-level nice: on Linux setpriority(PRIO_PROCESS, tid) affects
     * just this thread. Raising nice needs no privilege. */
    CHECK(setpriority(PRIO_PROCESS, (id_t)gettid(), s->nice) == 0);

    pthread_barrier_wait(s->start);
    unsigned long n = 0;
    /* relaxed: we only need to see the flag "soon", it orders nothing.
     * The loop body is a pure counter: no syscalls, so the only reason this
     * thread stops running is the scheduler preempting it. */
    while (!atomic_load_explicit(s->stop, memory_order_relaxed))
        n++;
    s->loops = n;

    struct timespec t;
    CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0);
    s->cpu_ms = ts_ms(t);
    return NULL;
}

/* Print the scheduler's own bookkeeping for a thread if CONFIG_SCHED_DEBUG
 * exposes /proc/<pid>/task/<tid>/sched (it does on Ubuntu and on the board). */
static void print_sched_file_hint(void)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/sched", (int)getpid());
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("   (%s not available: kernel without CONFIG_SCHED_DEBUG)\n", path);
        return;
    }
    char line[256];
    printf("   from %s (main thread):\n", path);
    while (fgets(line, sizeof line, f))
        if (strstr(line, "se.vruntime") || strstr(line, "se.sum_exec_runtime") ||
            strstr(line, "nr_switches") || strstr(line, "se.load.weight") ||
            strstr(line, "policy") || strstr(line, "prio"))
            printf("     %s", line);
    fclose(f);
}

int main(void)
{
    alarm(20);
    printf("1) nice -> weight (kernel/sched/core.c sched_prio_to_weight):\n");
    for (int nice = -20; nice <= 19; nice += 5)
        printf("   nice %3d -> weight %5d\n", nice, prio_to_weight[nice + 20]);
    CHECK(prio_to_weight[20] == 1024); /* nice 0 is the unit weight */

    /* Pick a CPU we are allowed to run on (taskset/cgroups may restrict us). */
    cpu_set_t allowed;
    CHECK(sched_getaffinity(0, sizeof allowed, &allowed) == 0);
    int cpu = -1;
    for (int c = CPU_SETSIZE - 1; c >= 0; c--)
        if (CPU_ISSET(c, &allowed)) {
            cpu = c;
            break;
        }
    CHECK(cpu >= 0);

    pthread_barrier_t start;
    CHECK(pthread_barrier_init(&start, NULL, 3) == 0);
    atomic_bool stop = false;
    struct spinner s[2] = {
        {.nice = 0, .cpu = cpu, .start = &start, .stop = &stop},
        {.nice = 5, .cpu = cpu, .start = &start, .stop = &stop},
    };
    pthread_t t[2];
    for (int i = 0; i < 2; i++)
        CHECK(pthread_create(&t[i], NULL, spin, &s[i]) == 0);

    printf("2) two spinners pinned to CPU %d, nice %d and nice %d, %d ms...\n",
           cpu, s[0].nice, s[1].nice, RUN_MS);
    pthread_barrier_wait(&start);
    struct timespec d = {0, RUN_MS * 1000000L};
    while (nanosleep(&d, &d) == -1 && errno == EINTR) {
    }
    atomic_store_explicit(&stop, true, memory_order_relaxed);
    for (int i = 0; i < 2; i++)
        CHECK(pthread_join(t[i], NULL) == 0); /* join = happens-before for loops/cpu_ms */
    pthread_barrier_destroy(&start);

    double w0 = prio_to_weight[s[0].nice + 20], w1 = prio_to_weight[s[1].nice + 20];
    double expect = w0 / (w0 + w1);
    double total = s[0].cpu_ms + s[1].cpu_ms;
    double share0 = total > 0 ? s[0].cpu_ms / total : 0;
    for (int i = 0; i < 2; i++)
        printf("   nice %d: cpu %7.1f ms, %12lu loops\n", s[i].nice, s[i].cpu_ms, s[i].loops);
    printf("   nice-0 share of the CPU: measured %.1f%%, expected %.1f%% (= %.0f/(%.0f+%.0f))\n",
           100 * share0, 100 * expect, w0, w0, w1);
    if (s[1].cpu_ms > 0)
        printf("   cpu-time ratio %.2f : 1 (weights predict %.2f : 1)\n",
               s[0].cpu_ms / s[1].cpu_ms, w0 / w1);

    /* The only deterministic claim: the nicer thread does not get MORE CPU
     * than the nice-0 one (10% margin for start-up and measurement noise).
     * The exact ratio depends on host load, sanitizer overhead and autogroup,
     * so it is printed, never asserted. */
    CHECK(s[1].cpu_ms <= s[0].cpu_ms * 1.10 + 5.0);
    CHECK(s[0].loops > 0);

    printf("3) the same scheduler state the kernel keeps:\n");
    print_sched_file_hint();
    printf("   Note: with autogroup (kernel.sched_autogroup_enabled=1) nice only\n"
           "   competes within a session; across terminals each SESSION gets a share.\n");
    printf("02_nice_weights: all checks passed\n");
    return 0;
}
