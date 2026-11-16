/*
 * 02_rt_signals_queue.c — standard signals COALESCE, real-time signals QUEUE
 *                         (with a value each, in order)
 *
 * Concept : For a standard signal (1..31) the kernel keeps ONE pending bit:
 *           send SIGUSR1 five times while it is blocked and it is delivered
 *           once. Real-time signals (SIGRTMIN..SIGRTMAX) are queued: five
 *           sigqueue() calls = five deliveries, in FIFO order, each with
 *           its own si_value. Among pending signals, lower numbers are
 *           delivered first. A signal is a notification, not a counter —
 *           unless it is a real-time one (and even those have a limit:
 *           RLIMIT_SIGPENDING).
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/02_rt_signals_queue.c
 * Read    : ../README.md  (section 3.3 "Standard vs real-time signals")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define N_SEND 5

/*
 * The pending state of a process, per signal:
 *
 *   standard (1..31):  one BIT        SIGUSR1 x5  ->  [1]          -> 1 delivery
 *   real-time (34..64): a QUEUE       SIGRTMIN x5 ->  [v0 v1 v2 v3 v4] -> 5 deliveries
 *
 * (glibc reserves the first 2-3 kernel RT signals for threads, so always
 *  write SIGRTMIN + n, never a hard-coded 34.)
 */
static void send_batch(void)
{
    for (int i = 0; i < N_SEND; i++) {
        CHECK(kill(getpid(), SIGUSR1) == 0); /* coalesces */
        union sigval v = {.sival_int = 100 + i};
        CHECK(sigqueue(getpid(), SIGRTMIN, v) == 0); /* queues with value */
    }
    union sigval v = {.sival_int = 999};
    CHECK(sigqueue(getpid(), SIGRTMIN + 1, v) == 0);
}

/* ---- version A: synchronous consumption with sigtimedwait() ---- */
static void demo_sigtimedwait(const sigset_t *set)
{
    printf("A) blocked, sent %d x SIGUSR1 and %d x SIGRTMIN, then sigtimedwait():\n",
           N_SEND, N_SEND);
    send_batch();

    sigset_t pend;
    CHECK(sigpending(&pend) == 0);
    CHECK(sigismember(&pend, SIGUSR1) && sigismember(&pend, SIGRTMIN));

    int usr1 = 0, rt = 0, rt1 = 0, order_ok = 1, expect = 100, idx = 0;
    struct timespec zero = {0, 0}; /* poll: never block */
    siginfo_t si;
    int sig;
    while ((sig = sigtimedwait(set, &si, &zero)) > 0) {
        if (sig == SIGUSR1) {
            usr1++;
            if (idx != 0)
                order_ok = 0; /* lowest number first */
        } else if (sig == SIGRTMIN) {
            rt++;
            if (si.si_value.sival_int != expect++)
                order_ok = 0; /* FIFO with values */
        } else if (sig == SIGRTMIN + 1) {
            rt1++;
            if (rt != N_SEND)
                order_ok = 0; /* RTMIN before RTMIN+1 */
        }
        printf("   #%d %-9s value=%d\n", idx++,
               sig == SIGUSR1 ? "SIGUSR1" : sig == SIGRTMIN ? "SIGRTMIN" : "SIGRTMIN+1",
               sig == SIGUSR1 ? 0 : si.si_value.sival_int);
    }
    CHECK(errno == EAGAIN); /* nothing left */
    printf("   SIGUSR1 delivered %d time(s), SIGRTMIN %d time(s), SIGRTMIN+1 %d\n", usr1,
           rt, rt1);
    CHECK(usr1 == 1);
    CHECK(rt == N_SEND);
    CHECK(rt1 == 1);
    CHECK(order_ok);
}

/* ---- version B: asynchronous handlers ---- */
static volatile sig_atomic_t g_log[16]; /* signal numbers in delivery order */
static volatile sig_atomic_t g_val[16];
static volatile sig_atomic_t g_n;

static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    int i = g_n;
    if (i < 16) {
        g_log[i] = sig;
        g_val[i] = si->si_value.sival_int;
        g_n = i + 1;
    }
}

static void demo_handlers(const sigset_t *set)
{
    printf("B) same batch, delivered to SA_SIGINFO handlers on unblock:\n");
    struct sigaction sa = {0};
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    /* sa_mask = all three: while one handler runs, the others wait. Without
     * it, the next pending signal would interrupt the handler (nested
     * frames) and g_log would be filled in a confusing order. */
    sa.sa_mask = *set;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    CHECK(sigaction(SIGRTMIN, &sa, NULL) == 0);
    CHECK(sigaction(SIGRTMIN + 1, &sa, NULL) == 0);

    send_batch(); /* still blocked: all pending */

    /*
     * sigsuspend(empty mask) = atomically unblock everything and wait.
     * A pending signal is delivered at once. Its handler's sa_mask blocks
     * the others, and when the handler returns the mask from BEFORE
     * sigsuspend (all three blocked) comes back: so each sigsuspend()
     * delivers exactly ONE signal. Loop while anything is still pending;
     * checking sigpending() first means we can never block forever.
     */
    sigset_t none, pend;
    sigemptyset(&none);
    int calls = 0;
    for (;;) {
        CHECK(sigpending(&pend) == 0);
        if (!sigismember(&pend, SIGUSR1) && !sigismember(&pend, SIGRTMIN) &&
            !sigismember(&pend, SIGRTMIN + 1))
            break;
        sigsuspend(&none); /* returns -1/EINTR after one handler ran */
        calls++;
    }
    printf("   %d sigsuspend() calls drained the pending set\n", calls);

    int n = g_n;
    printf("   %d handler calls:", n);
    for (int i = 0; i < n; i++)
        printf(" %s(%d)", g_log[i] == SIGUSR1 ? "USR1" : g_log[i] == SIGRTMIN ? "RTMIN" : "RTMIN+1",
               g_log[i] == SIGUSR1 ? 0 : (int)g_val[i]);
    printf("\n");
    CHECK(n == 1 + N_SEND + 1);
    int rt = 0;
    for (int i = 0; i < n; i++) {
        if (g_log[i] == SIGUSR1)
            continue;
        if (g_log[i] == SIGRTMIN)
            CHECK(g_val[i] == 100 + rt++); /* values in FIFO order */
    }
    CHECK(rt == N_SEND);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_SIGPENDING, &rl) == 0);
    printf("SIGRTMIN=%d SIGRTMAX=%d, RLIMIT_SIGPENDING=%llu queued signals per user\n",
           SIGRTMIN, SIGRTMAX, (unsigned long long)rl.rlim_cur);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGRTMIN);
    sigaddset(&set, SIGRTMIN + 1);
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);

    demo_sigtimedwait(&set);
    demo_handlers(&set);

    printf("02_rt_signals_queue: all checks passed\n");
    return 0;
}
