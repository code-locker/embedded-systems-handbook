/*
 * 01_signal_basics.c — signals as asynchronous notifications: kill, raise,
 *                      dispositions, default actions, sigaction + SA_SIGINFO
 *
 * Concept : A signal is a tiny message (just a NUMBER, optionally one int or
 *           pointer of payload) that the kernel delivers to a process by
 *           interrupting whatever it is doing. For every signal a process
 *           has a DISPOSITION: default action, ignore, or call a handler.
 *           sigaction() with SA_SIGINFO tells the handler WHO sent it
 *           (si_pid, si_uid) and HOW (si_code: kill, sigqueue, timer, ...).
 *           The race-free way to wait for a signal is: block it, then
 *           sigsuspend() with it unblocked.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/01_signal_basics.c
 * Read    : ../README.md  (sections 3.1 "Signals" and 3.2 "sigaction")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/*
 * What a handler may touch: only `volatile sig_atomic_t` variables (or
 * lock-free atomics) and async-signal-safe functions (signal-safety(7)).
 * So the handler just RECORDS what happened; main() prints it later.
 * pid_t and the codes fit in an int, which sig_atomic_t is on Linux.
 */
static volatile sig_atomic_t g_count;
static volatile sig_atomic_t g_signo;
static volatile sig_atomic_t g_pid;
static volatile sig_atomic_t g_code;
static volatile sig_atomic_t g_value;

static void on_signal(int sig, siginfo_t *si, void *ucontext)
{
    (void)ucontext;
    g_signo = sig;
    g_pid = si->si_pid;
    g_code = si->si_code;
    g_value = si->si_value.sival_int; /* only meaningful for SI_QUEUE */
    g_count = g_count + 1;
}

static const char *code_name(int code)
{
    switch (code) {
    case SI_USER:  return "SI_USER (kill)";
    case SI_TKILL: return "SI_TKILL (tgkill/raise)";
    case SI_QUEUE: return "SI_QUEUE (sigqueue)";
    case SI_TIMER: return "SI_TIMER";
    default:       return "other";
    }
}

static void print_default_actions(void)
{
    /* The default action is fixed per signal (signal(7)). */
    static const struct { int sig; const char *action; } t[] = {
        {SIGTERM, "terminate"}, {SIGINT, "terminate"},  {SIGHUP, "terminate"},
        {SIGKILL, "terminate (cannot be caught/ignored)"},
        {SIGUSR1, "terminate"}, {SIGPIPE, "terminate"},  {SIGALRM, "terminate"},
        {SIGSEGV, "core dump"}, {SIGABRT, "core dump"},  {SIGQUIT, "core dump"},
        {SIGCHLD, "ignore"},    {SIGWINCH, "ignore"},    {SIGURG, "ignore"},
        {SIGSTOP, "stop (cannot be caught/ignored)"},    {SIGTSTP, "stop"},
        {SIGCONT, "continue"},
    };
    printf("default actions (signal(7)):\n");
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        printf("  %2d %-8s %-28s -> %s\n", t[i].sig, sigabbrev_np(t[i].sig),
               strsignal(t[i].sig), t[i].action);
}

static int wait_status(pid_t pid, int options)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, options);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    return st;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    print_default_actions();

    /* ---- 1. install a handler with sigaction + SA_SIGINFO ---- */
    struct sigaction sa = {0};
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask); /* extra signals to block while the handler runs */
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    /* SIGKILL and SIGSTOP can never be caught: the kernel says EINVAL. */
    errno = 0;
    CHECK(sigaction(SIGKILL, &sa, NULL) == -1 && errno == EINVAL);
    printf("sigaction(SIGKILL) -> EINVAL: SIGKILL/SIGSTOP cannot be caught\n");

    /* ---- 2. raise(): send to myself; the handler runs before raise returns ---- */
    CHECK(raise(SIGUSR1) == 0);
    printf("raise(SIGUSR1): handler ran %d time(s), si_code=%s, si_pid=%d (me=%d)\n",
           (int)g_count, code_name(g_code), (int)g_pid, (int)getpid());
    CHECK(g_count == 1 && g_signo == SIGUSR1 && g_pid == getpid());
    CHECK(g_code == SI_TKILL || g_code == SI_USER);

    /*
     * ---- 3. another process sends: block, fork, sigsuspend ----
     *
     *   parent                                  child
     *   block SIGUSR1  (it can now only become PENDING, not delivered)
     *   fork() --------------------------------> kill(parent, SIGUSR1)
     *   sigsuspend(mask WITHOUT SIGUSR1)            sigqueue(parent, SIGUSR1, 42)
     *      atomically: unblock + sleep;
     *      handler runs; sigsuspend returns -1/EINTR; mask restored
     *
     * Without the block, the child could send BEFORE we start waiting and
     * pause() would sleep forever: the classic lost-wakeup race.
     */
    sigset_t usr1, old;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &usr1, &old) == 0);
    g_count = 0;

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        if (kill(getppid(), SIGUSR1) != 0)
            _exit(1);
        _exit(0);
    }
    sigset_t waitmask = old;
    sigdelset(&waitmask, SIGUSR1);
    while (g_count == 0)
        sigsuspend(&waitmask); /* always returns -1 with EINTR */
    printf("child %d sent kill(): si_pid=%d si_code=%s\n", (int)pid, (int)g_pid,
           code_name(g_code));
    CHECK(g_pid == pid && g_code == SI_USER);
    int st = wait_status(pid, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* sigqueue: the same, plus one int of payload */
    g_count = 0;
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        union sigval v = {.sival_int = 42};
        _exit(sigqueue(getppid(), SIGUSR1, v) == 0 ? 0 : 1);
    }
    while (g_count == 0)
        sigsuspend(&waitmask);
    printf("child %d sent sigqueue(42): si_code=%s si_value=%d\n", (int)pid,
           code_name(g_code), (int)g_value);
    CHECK(g_pid == pid && g_code == SI_QUEUE && g_value == 42);
    st = wait_status(pid, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(sigprocmask(SIG_SETMASK, &old, NULL) == 0);

    /* ---- 4. default action: terminate. The parent sees it in waitpid ---- */
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        signal(SIGTERM, SIG_DFL);
        raise(SIGTERM); /* default action: the process dies here */
        _exit(99);      /* never reached */
    }
    st = wait_status(pid, 0);
    printf("child raised SIGTERM with default action -> WIFSIGNALED, WTERMSIG=%s\n",
           sigabbrev_np(WTERMSIG(st)));
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);

    /*
     * ---- 5. SIG_IGN, stop/continue, and SIGKILL ----
     * Dispositions are inherited across fork(), so setting SIG_IGN before
     * fork() means the child ignores SIGTERM from its first instruction
     * (no race). (exec() keeps SIG_IGN but resets handlers to default.)
     */
    struct sigaction ign = {.sa_handler = SIG_IGN}, prev;
    CHECK(sigaction(SIGTERM, &ign, &prev) == 0);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5); /* watchdog in case the parent dies */
        for (;;)
            pause();
    }
    CHECK(sigaction(SIGTERM, &prev, NULL) == 0); /* parent restores */

    CHECK(kill(pid, SIGTERM) == 0); /* ignored: discarded at once */
    CHECK(waitpid(pid, &st, WNOHANG) == 0);
    printf("child ignores SIGTERM: still alive after kill(SIGTERM)\n");

    CHECK(kill(pid, SIGSTOP) == 0);
    st = wait_status(pid, WUNTRACED);
    CHECK(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
    printf("kill(SIGSTOP) -> WIFSTOPPED (like Ctrl-Z, but uncatchable)\n");
    CHECK(kill(pid, SIGCONT) == 0);
    st = wait_status(pid, WCONTINUED);
    CHECK(WIFCONTINUED(st));
    printf("kill(SIGCONT) -> WIFCONTINUED\n");

    CHECK(kill(pid, SIGKILL) == 0); /* cannot be ignored */
    st = wait_status(pid, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    printf("kill(SIGKILL) -> killed although SIGTERM was ignored\n");

    printf("01_signal_basics: all checks passed\n");
    return 0;
}
