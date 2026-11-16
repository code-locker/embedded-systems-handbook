/*
 * irqsim_watch.c — look at irqsim_demo's interrupt the way you look at any
 *                  IRQ on a board: /proc/interrupts, /proc/irq/N/, the irq
 *                  thread's scheduling class, and the driver's debugfs stats
 *
 * Concept : An interrupt is visible from user space in four places:
 *           /proc/interrupts (per-CPU counts, chip, hwirq, trigger, name),
 *           /proc/irq/<N>/ (smp_affinity, the actions), the "irq/<N>-name"
 *           kthread that runs the threaded handler (SCHED_FIFO, prio 50) and
 *           whatever the driver exports (here debugfs). This program finds
 *           irqsim_demo's line, measures the IRQ rate over one second and
 *           checks it against the module's period_us when keep_running=1.
 * Build   : make drivers   (-> build/linux-device-drivers/04-interrupts-timers-and-deferred-work/user/irqsim_watch)
 * Run     : sudo insmod kernel/irqsim_demo.ko keep_running=1 period_us=1000
 *           sudo ./irqsim_watch            (root only for the debugfs part)
 *           Needs the module loaded, so `make test` does NOT run it.
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = no module.
 * Read    : ../README.md  (section 3.1 "From pin to handler", "Seeing it")
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define NAME "irqsim_demo"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/*
 * Find our line in /proc/interrupts and return the sum over all CPUs.
 *
 *            CPU0       CPU1
 *  45:        512        488  esh-swirq   0-level      irqsim_demo
 *  ^irq       ^per-CPU counts ^chip      ^hwirq-flow   ^action name(s)
 */
static long long read_count(int *irq, char *line_out, size_t line_len)
{
    FILE *f = fopen("/proc/interrupts", "r");
    char line[1024];
    long long total = -1;

    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f)) {
        char *p, *end;

        if (!strstr(line, NAME))
            continue;
        *irq = (int)strtol(line, &p, 10);
        if (*p != ':')
            continue;
        p++;
        total = 0;
        for (;;) {          /* the per-CPU columns are the leading numbers */
            long long v = strtoll(p, &end, 10);

            if (end == p)
                break;
            total += v;
            p = end;
        }
        if (line_out) {
            snprintf(line_out, line_len, "%s", line);
        }
        break;
    }
    fclose(f);
    return total;
}

static int read_int_file(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    int ok;

    if (!f)
        return -1;
    ok = fscanf(f, "%d", out) == 1;
    fclose(f);
    return ok ? 0 : -1;
}

static void print_file(const char *label, const char *path)
{
    char buf[4096];
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f) {
        printf("%-22s (cannot open %s: %s)\n", label, path, strerror(errno));
        return;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    printf("%-22s %s%s", label, buf, (n && buf[n - 1] == '\n') ? "" : "\n");
}

/* The threaded handler runs in a kthread named "irq/<N>-<name>". Find it. */
static pid_t find_irq_thread(int irq)
{
    char want[64], path[300], comm[64];
    DIR *d = opendir("/proc");
    struct dirent *de;
    pid_t found = 0;

    if (!d)
        return 0;
    snprintf(want, sizeof(want), "irq/%d-", irq);
    while ((de = readdir(d)) && !found) {
        FILE *f;

        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
        f = fopen(path, "r");
        if (!f)
            continue;
        if (fgets(comm, sizeof(comm), f) && strncmp(comm, want, strlen(want)) == 0 &&
            strstr(comm, "irqsim") && !strstr(comm, "-s-"))
            found = (pid_t)atoi(de->d_name);
        fclose(f);
    }
    closedir(d);
    return found;
}

static double now_s(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
    char line[1024], path[128];
    int irq = -1, period_us = 0;
    long long c0, c1;
    double t0, t1, rate;
    struct timespec one_s = { 1, 0 };
    char running = 'N';
    FILE *f;
    pid_t tid;

    c0 = read_count(&irq, line, sizeof(line));
    if (c0 < 0) {
        fprintf(stderr, "no '%s' line in /proc/interrupts.\n"
                        "Load the module first (board or VM, never your desktop):\n"
                        "  sudo insmod irqsim_demo.ko keep_running=1\n", NAME);
        return 2;
    }
    printf("/proc/interrupts:      %s", line);

    snprintf(path, sizeof(path), "/proc/irq/%d/smp_affinity_list", irq);
    print_file("smp_affinity_list:", path);
    snprintf(path, sizeof(path), "/proc/irq/%d/spurious", irq);
    print_file("spurious:", path);  /* IRQ_NONE returns are counted here */

    tid = find_irq_thread(irq);
    if (tid > 0) {
        struct sched_param sp;
        int pol = sched_getscheduler(tid);

        CHECK(sched_getparam(tid, &sp) == 0);
        printf("irq thread:            pid %d, policy %s, priority %d\n", (int)tid,
               pol == SCHED_FIFO ? "SCHED_FIFO" : pol == SCHED_OTHER ? "SCHED_OTHER" : "other",
               sp.sched_priority);
        CHECK(pol == SCHED_FIFO);   /* genirq creates irq threads as SCHED_FIFO */
        printf("PASS irq-thread is SCHED_FIFO (try: chrt -f -p 80 %d)\n", (int)tid);
    } else {
        printf("irq thread:            not found (no permission to read /proc/*/comm?)\n");
    }

    read_int_file("/sys/module/" NAME "/parameters/period_us", &period_us);
    f = fopen("/sys/module/" NAME "/parameters/keep_running", "r");
    if (f) {
        if (fscanf(f, " %c", &running) != 1)
            running = 'N';
        fclose(f);
    }

    t0 = now_s();
    nanosleep(&one_s, NULL);
    c1 = read_count(&irq, NULL, 0);
    t1 = now_s();
    CHECK(c1 >= c0);
    rate = (double)(c1 - c0) / (t1 - t0);
    printf("rate:                  %.0f IRQs/s over %.2f s (keep_running=%c, period_us=%d)\n",
           rate, t1 - t0, running, period_us);

    if (running == 'Y' && period_us > 0) {
        double expect = 1e6 / period_us;

        /*
         * Not every tick becomes an IRQ: while the ONESHOT thread runs the
         * line is masked and events coalesce. So: at most the tick rate,
         * and at least half of it on an idle machine.
         */
        CHECK(rate <= expect * 1.1);
        CHECK(rate >= expect * 0.5);
        printf("PASS rate within 50..110%% of %.0f/s\n", expect);
    } else {
        printf("INFO device stopped after the self-test; load with keep_running=1 to see a rate\n");
    }

    print_file("debugfs stats:\n", "/sys/kernel/debug/" NAME "/stats");
    return 0;
}
