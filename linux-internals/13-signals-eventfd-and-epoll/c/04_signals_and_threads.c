/*
 * 04_signals_and_threads.c — which thread gets a signal? Block everywhere,
 *                            handle in ONE dedicated thread with sigwaitinfo
 *
 * Concept : Dispositions (handler / ignore / default) are per PROCESS, but
 *           the signal MASK is per THREAD. A process-directed signal
 *           (kill(pid, ...)) goes to any one thread that does not block it;
 *           a thread-directed one (pthread_kill, tgkill, a fault like
 *           SIGSEGV) goes to that thread. The clean design for threaded
 *           programs: block the signals in main BEFORE creating threads
 *           (new threads inherit the mask), then let one thread pick them
 *           up synchronously with sigwaitinfo(). No async handler, so no
 *           async-signal-safety rules: that thread may lock, log, malloc.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/04_signals_and_threads.c
 *           make run P=linux-internals/13-signals-eventfd-and-epoll/c/04_signals_and_threads.c SAN=tsan
 * Read    : ../README.md  (section 3.6 "Signals and threads")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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

#define N_USR1 3

static pid_t my_tid(void) { return (pid_t)syscall(SYS_gettid); }

/* Shared between threads (NOT touched by any async handler except the
 * one marked below): plain C11 atomics are enough. */
static atomic_int g_usr1_seen;       /* incremented by the signal thread */
static atomic_int g_signal_thread_tid;
static atomic_int g_term_seen;

/*
 * The dedicated signal thread:
 *
 *   main           worker          sigthread
 *   block {USR1, USR2, TERM}  <- before pthread_create: everyone inherits
 *   create ------> (blocked)       (blocked)
 *                                  sigwaitinfo({USR1, TERM}) ... sleeps
 *   kill(getpid(), USR1) ------------------------> returns USR1 -> handle
 *   kill(getpid(), TERM) ------------------------> returns TERM -> exit
 *
 * sigwaitinfo() only takes signals that are BLOCKED in the caller (that is
 * the point: blocked = pending until someone asks for it).
 */
static void *signal_thread(void *arg)
{
    (void)arg;
    atomic_store(&g_signal_thread_tid, my_tid());
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGTERM);
    for (;;) {
        siginfo_t si;
        int sig = sigwaitinfo(&set, &si);
        if (sig < 0) {
            if (errno == EINTR)
                continue;
            return NULL;
        }
        if (sig == SIGUSR1) {
            /* Normal thread context: printf, locks, malloc are all fine. */
            printf("  sigthread (tid %d): SIGUSR1 from pid %d\n", my_tid(), (int)si.si_pid);
            atomic_fetch_add(&g_usr1_seen, 1);
        } else if (sig == SIGTERM) {
            printf("  sigthread: SIGTERM -> shutting down\n");
            atomic_store(&g_term_seen, 1);
            return NULL;
        }
    }
}

/* Async handler for the thread-directed SIGUSR2: records which thread it
 * ran on. Only async-signal-safe things: syscall(gettid) + an atomic store. */
static atomic_int g_usr2_tid;
static void on_usr2(int sig)
{
    (void)sig;
    atomic_store(&g_usr2_tid, my_tid());
}

static atomic_int g_worker_tid;
static void *worker(void *arg)
{
    (void)arg;
    atomic_store(&g_worker_tid, my_tid());
    /* Wait for SIGUSR2 aimed at THIS thread: atomically unblock it and
     * sleep. (The mask arg is the temporary mask: everything blocked but
     * SIGUSR2.) */
    sigset_t only_usr2;
    sigfillset(&only_usr2);
    sigdelset(&only_usr2, SIGUSR2);
    while (atomic_load(&g_usr2_tid) == 0)
        sigsuspend(&only_usr2);
    return NULL;
}

static void wait_for(atomic_int *v, int value)
{
    for (int i = 0; i < 5000 && atomic_load(v) < value; i++) {
        struct timespec ts = {0, 1000000}; /* 1 ms, max 5 s total */
        nanosleep(&ts, NULL);
    }
    CHECK(atomic_load(v) >= value);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    struct sigaction sa = {0};
    sa.sa_handler = on_usr2;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGUSR2, &sa, NULL) == 0);

    /* 1) Block BEFORE creating any thread. A thread created earlier would
     *    keep an unblocked mask and could receive SIGTERM itself, running
     *    the default action (killing the whole process). */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    sigaddset(&set, SIGTERM);
    CHECK(pthread_sigmask(SIG_BLOCK, &set, NULL) == 0);

    pthread_t st, wt;
    CHECK(pthread_create(&st, NULL, signal_thread, NULL) == 0);
    CHECK(pthread_create(&wt, NULL, worker, NULL) == 0);
    wait_for(&g_signal_thread_tid, 1);
    wait_for(&g_worker_tid, 1);
    printf("main tid %d, sigthread tid %d, worker tid %d (pid %d)\n", my_tid(),
           atomic_load(&g_signal_thread_tid), atomic_load(&g_worker_tid), (int)getpid());

    /* 2) Process-directed signals: all threads block them, so they stay
     *    pending until the signal thread's sigwaitinfo() takes them. We
     *    wait for each to be seen before sending the next one, because
     *    pending standard signals COALESCE (lesson 02). */
    printf("1) kill(getpid(), SIGUSR1) x%d -> picked up by the signal thread\n", N_USR1);
    for (int i = 1; i <= N_USR1; i++) {
        CHECK(kill(getpid(), SIGUSR1) == 0);
        wait_for(&g_usr1_seen, i);
    }
    CHECK(atomic_load(&g_usr1_seen) == N_USR1);

    /* 3) Thread-directed: pthread_kill targets one thread. The handler
     *    runs on the worker, which unblocked SIGUSR2 inside sigsuspend. */
    printf("2) pthread_kill(worker, SIGUSR2) -> handler runs on the worker\n");
    CHECK(pthread_kill(wt, SIGUSR2) == 0);
    CHECK(pthread_join(wt, NULL) == 0);
    printf("  SIGUSR2 handler ran on tid %d (worker tid %d)\n", atomic_load(&g_usr2_tid),
           atomic_load(&g_worker_tid));
    CHECK(atomic_load(&g_usr2_tid) == atomic_load(&g_worker_tid));

    /* 4) Clean shutdown through the same path a real `kill <pid>` takes. */
    printf("3) kill(getpid(), SIGTERM) -> orderly shutdown\n");
    CHECK(kill(getpid(), SIGTERM) == 0);
    CHECK(pthread_join(st, NULL) == 0);
    CHECK(atomic_load(&g_term_seen) == 1);

    printf("04_signals_and_threads: all checks passed\n");
    return 0;
}
