/*
 * 09_unsafe_handler_bug.c — INTENTIONALLY BROKEN: a signal handler that
 *                           calls printf() and malloc()/free()
 *
 * Concept : A handler interrupts the program at an ARBITRARY instruction,
 *           possibly in the middle of malloc() (holding the heap lock, with
 *           the free lists half-updated) or of printf() (holding the stdout
 *           lock, with the buffer half-written). If the handler then calls
 *           malloc() or printf() itself:
 *             - glibc's malloc lock is not recursive -> the handler waits
 *               for a lock its own thread holds -> DEADLOCK (hang forever);
 *             - stdio's FILE lock IS recursive for the same thread -> the
 *               handler gets in and scribbles over a half-updated buffer
 *               -> duplicated/lost/garbled output, or heap corruption.
 *           Only async-signal-safe functions (signal-safety(7): write,
 *           read, _exit, sigaction, kill, sem_post, ...) and
 *           `volatile sig_atomic_t` / lock-free atomics are allowed.
 *
 * Finds it: 1. ThreadSanitizer, every time, without needing the unlucky
 *              timing:
 *                make run P=linux-internals/13-signals-eventfd-and-epoll/c/09_unsafe_handler_bug.c SAN=tsan
 *              -> "WARNING: ThreadSanitizer: signal-unsafe call inside of
 *                 a signal" pointing at malloc/printf in on_prof().
 *              It also reports "signal handler spoils errno": the
 *              handler does not save/restore errno. TSan defers the
 *              handler to a safe point, so under TSan the run completes
 *              and exits 66 ("reported N warnings").
 *           2. Code review against signal-safety(7): "is every function
 *              called from this handler on the list?" printf and malloc
 *              are not.
 *           3. Running it and watching it hang. On the author's x86 PC it
 *              deadlocked in every run, both in the default ASan build and
 *              with `make run P=... SAN=0`; the alarm(5) watchdog then
 *              kills it ("Alarm clock", exit status 142). On another
 *              machine, libc or load it may survive many runs in a row —
 *              that is exactly what makes this bug dangerous: it can pass
 *              every test and hang a device in the field once a month.
 * Fix     : the handler only sets a `volatile sig_atomic_t` flag or
 *           write()s one byte to a self-pipe, and the main loop does the
 *           printing (05_self_pipe_and_signalfd.c), or use signalfd.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/09_unsafe_handler_bug.c
 * Read    : ../README.md  (section 3.4 "Async-signal-safety")
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_in_handler_calls;
static FILE *g_log; /* /dev/null: the output itself does not matter */

/* BUG: printf/fprintf and malloc/free are NOT async-signal-safe. */
static void on_prof(int sig)
{
    char *msg = malloc(64);                       /* BUG */
    if (msg) {
        snprintf(msg, 64, "signal %d #%d", sig, (int)g_in_handler_calls);
        fprintf(g_log, "handler: %s\n", msg);     /* BUG */
        free(msg);                                /* BUG */
    }
    g_in_handler_calls = g_in_handler_calls + 1;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    g_log = fopen("/dev/null", "w");
    if (!g_log)
        return 1;
    alarm(5); /* watchdog: a deadlocked run is killed by SIGALRM */

    struct sigaction sa = {0};
    sa.sa_handler = on_prof;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, NULL);

    /* SIGPROF every 200 us of CPU time: the main loop burns CPU in
     * malloc/free/fprintf, so the signal very often lands inside them. */
    struct itimerval it = {.it_interval = {0, 200}, .it_value = {0, 200}};
    setitimer(ITIMER_PROF, &it, NULL);

    printf("hammering malloc/free/fprintf for 1.5 s while SIGPROF fires...\n");
    unsigned long iters = 0;
    double end = now_s() + 1.5;
    while (now_s() < end) {
        for (int i = 0; i < 1000; i++) {
            void *p = malloc((size_t)(16 + i % 512));
            fprintf(g_log, "main loop %lu\n", iters);
            free(p);
            iters++;
        }
    }
    struct itimerval off = {0};
    setitimer(ITIMER_PROF, &off, NULL);
    fclose(g_log);

    printf("%lu iterations, handler ran %d times, and we did not hang THIS time.\n",
           iters, (int)g_in_handler_calls);
    printf("The bug is still there: run with SAN=tsan to see it reported.\n");
    printf("09_unsafe_handler_bug: finished (not proof of correctness)\n");
    return 0;
}
