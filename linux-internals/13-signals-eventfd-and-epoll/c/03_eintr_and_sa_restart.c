/*
 * 03_eintr_and_sa_restart.c — a signal interrupts a blocking system call:
 *                             EINTR, retry loops, SA_RESTART and its limits
 *
 * Concept : When a handler runs while the process sleeps in a "slow" system
 *           call (read on an empty pipe, accept, wait, poll, sleep...), the
 *           kernel must abort the call to run the handler. Afterwards the
 *           call either fails with errno == EINTR (no SA_RESTART) or is
 *           transparently restarted (SA_RESTART). SA_RESTART does NOT help
 *           for poll/epoll_wait/select/nanosleep and some others: they
 *           always return EINTR (signal(7), "Interruption of system calls").
 *           So robust code retries on EINTR everywhere.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/03_eintr_and_sa_restart.c
 * Read    : ../README.md  (section 3.5 "EINTR and SA_RESTART")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int g_pipe[2];
static volatile sig_atomic_t g_ticks;
static volatile sig_atomic_t g_handler_writes; /* case 1 only */

/*
 * Periodic SIGALRM handler. In case 1, on the 3rd tick it makes the pipe
 * readable.
 * write() is async-signal-safe; errno is saved and restored because the
 * interrupted code may be just about to look at errno.
 */
static void on_alarm(int sig)
{
    (void)sig;
    int saved = errno;
    g_ticks = g_ticks + 1;
    if (g_handler_writes && g_ticks == 3) {
        char c = 'x';
        ssize_t r = write(g_pipe[1], &c, 1);
        (void)r;
    }
    errno = saved;
}

static void install(int flags)
{
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGALRM, &sa, NULL) == 0);
}

/* Periodic timer: a tick every 30 ms. Periodic (not one-shot) so that even
 * if a tick lands just BEFORE we block, the next one still interrupts. */
static void timer_on(void)
{
    g_ticks = 0;
    struct itimerval it = {.it_interval = {0, 30000}, .it_value = {0, 30000}};
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
}

static void timer_off(void)
{
    struct itimerval it = {0};
    CHECK(setitimer(ITIMER_REAL, &it, NULL) == 0);
}

static void drain_pipe(void)
{
    char buf[16];
    int fl = fcntl(g_pipe[0], F_GETFL);
    fcntl(g_pipe[0], F_SETFL, fl | O_NONBLOCK);
    while (read(g_pipe[0], buf, sizeof buf) > 0) {
    }
    fcntl(g_pipe[0], F_SETFL, fl);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CHECK(pipe2(g_pipe, O_CLOEXEC) == 0);
    /* We keep the write end open ourselves, so read() never sees EOF: it
     * can only return because of data or a signal. */

    /*
     * 1) Without SA_RESTART:
     *
     *   main: read(pipe) ....sleeping....  <- tick 1: handler runs
     *         read returns -1, errno = EINTR (no data was read)
     */
    install(0);
    g_handler_writes = 1;
    timer_on();
    char c;
    errno = 0;
    ssize_t n = read(g_pipe[0], &c, 1);
    int e = errno;
    printf("1) no SA_RESTART: read() -> %zd, errno=%s, after %d tick(s)\n", n,
           e == EINTR ? "EINTR" : strerror(e), (int)g_ticks);
    CHECK(n == -1 && e == EINTR);

    /* The standard retry loop. glibc's TEMP_FAILURE_RETRY(expr) macro is
     * the same thing. */
    int retries = 1; /* the EINTR above */
    while ((n = read(g_pipe[0], &c, 1)) == -1 && errno == EINTR)
        retries++;
    timer_off();
    printf("   retry loop: got '%c' after %d EINTR(s), %d ticks\n", c, retries,
           (int)g_ticks);
    CHECK(n == 1 && c == 'x' && g_ticks >= 3);
    drain_pipe();

    /*
     * 2) With SA_RESTART: the ticks interrupt read(), the handler runs, and
     *    the kernel re-enters read() by itself. The program never sees
     *    EINTR. The data comes from a child process 200 ms later.
     *
     *    time  0    30   60   90  ...  200 ms
     *    tick       |    |    |   ...           handler runs, read restarts
     *    child                          write('x')  -> read() returns 1
     */
    g_handler_writes = 0;
    install(SA_RESTART);
    timer_on(); /* itimers are not inherited by fork(): only we tick */
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        struct timespec d = {0, 200 * 1000000};
        nanosleep(&d, NULL);
        char x = 'x';
        _exit(write(g_pipe[1], &x, 1) == 1 ? 0 : 1);
    }
    errno = 0;
    n = read(g_pipe[0], &c, 1);
    e = errno;
    int ticks = g_ticks;
    timer_off();
    printf("2) SA_RESTART: read() -> %zd ('%c'), %d tick(s) handled meanwhile, no EINTR\n",
           n, n == 1 ? c : '?', ticks);
    CHECK(n == 1 && c == 'x');
    /* >= 1, not == 6: how many ticks ran is timing. Under ThreadSanitizer
     * the handler is deferred until read() returns, so expect 1 there. */
    CHECK(ticks >= 1);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    drain_pipe();

    /*
     * 3) SA_RESTART does not apply to poll(), epoll_wait(), select(),
     *    nanosleep(), sigsuspend(), ... (time-based or multiplexing waits).
     */
    timer_on();
    struct pollfd p = {.fd = g_pipe[0], .events = POLLIN};
    errno = 0;
    int r = poll(&p, 1, 5000);
    e = errno;
    printf("3) SA_RESTART + poll(): returns %d, errno=%s (never restarted)\n", r,
           e == EINTR ? "EINTR" : strerror(e));
    CHECK(r == -1 && e == EINTR);

    struct timespec req = {2, 0}, rem = {0, 0};
    errno = 0;
    r = nanosleep(&req, &rem);
    e = errno;
    timer_off();
    printf("   SA_RESTART + nanosleep(2 s): returns %d, errno=%s, %ld.%03ld s left\n", r,
           e == EINTR ? "EINTR" : strerror(e), (long)rem.tv_sec, rem.tv_nsec / 1000000);
    CHECK(r == -1 && e == EINTR);
    CHECK(rem.tv_sec >= 1); /* resume with nanosleep(&rem, &rem) */
    /* For exact deadlines prefer clock_nanosleep(TIMER_ABSTIME): a retry
     * with the same absolute time cannot drift. */

    close(g_pipe[0]);
    close(g_pipe[1]);
    printf("03_eintr_and_sa_restart: all checks passed\n");
    return 0;
}
