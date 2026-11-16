/*
 * 03_mq_notify.c — mq_notify(): be told by a signal when an EMPTY queue
 *                  receives a message; one-shot, one registered process
 *
 * Concept : Instead of blocking in mq_receive, a process can register for
 *           ONE notification: when a message arrives on an empty queue, the
 *           kernel sends a signal (SIGEV_SIGNAL) or starts a thread
 *           (SIGEV_THREAD). Rules worth remembering:
 *             - only on the transition EMPTY -> NON-EMPTY,
 *             - only if no other process is blocked in mq_receive,
 *             - one-shot: after it fires you must register again,
 *             - only ONE process may be registered per queue (EBUSY).
 *           We use a signal and receive it synchronously with
 *           sigtimedwait() — no async handler, no races, and a timeout.
 *           (On Linux, poll()/epoll on the mqd is usually simpler — see
 *           02_mq_producer_consumer.c.)
 * Build   : make run P=linux-internals/10-ipc-message-queues/c/03_mq_notify.c
 * Read    : ../README.md  (section "3.3 mq_notify")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define COOKIE 4242

static void wait_ok(pid_t pid)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void register_notify(mqd_t mq)
{
    struct sigevent sev;
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    sev.sigev_value.sival_int = COOKIE; /* comes back in si_value */
    CHECK(mq_notify(mq, &sev) == 0);
}

/* Wait up to `ms` for SIGUSR1. Returns 1 and fills *si, or 0 on timeout. */
static int wait_signal(siginfo_t *si, long ms)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    for (;;) {
        int s = sigtimedwait(&set, si, &ts);
        if (s == SIGUSR1)
            return 1;
        if (s < 0 && errno == EINTR)
            continue;
        CHECK(s < 0 && errno == EAGAIN); /* timeout */
        return 0;
    }
}

static int drain(mqd_t mq)
{
    char buf[64];
    int n = 0;
    while (mq_receive(mq, buf, sizeof buf, NULL) >= 0)
        n++;
    CHECK(errno == EAGAIN);
    return n;
}

int main(void)
{
    alarm(10);
    /* Block SIGUSR1 so it stays PENDING until sigtimedwait() collects it,
     * instead of running the default action (terminate). Must be done
     * before the signal can possibly arrive. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL) == 0);

    char name[64];
    snprintf(name, sizeof name, "/mq_notify_%d", (int)getpid());
    struct mq_attr attr = {.mq_maxmsg = 4, .mq_msgsize = 64};
    mqd_t mq = mq_open(name, O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK |
                                 O_CLOEXEC, 0600, &attr);
    CHECK(mq != (mqd_t)-1);

    register_notify(mq);
    printf("1) parent %d registered for SIGUSR1 on %s\n", (int)getpid(), name);

    /* Child: (a) tries to register too -> EBUSY, (b) sends one message. */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        mqd_t c = mq_open(name, O_WRONLY | O_CLOEXEC);
        CHECK(c != (mqd_t)-1);
        struct sigevent sev;
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_SIGNAL;
        sev.sigev_signo = SIGUSR1;
        errno = 0;
        CHECK(mq_notify(c, &sev) == -1 && errno == EBUSY);
        struct timespec d = {0, 50 * 1000 * 1000};
        nanosleep(&d, NULL); /* let the parent reach sigtimedwait */
        CHECK(mq_send(c, "wake up", 8, 0) == 0);
        mq_close(c);
        _exit(0);
    }

    siginfo_t si;
    CHECK(wait_signal(&si, 2000));
    wait_ok(pid);
    printf("2) got SIGUSR1: si_code=%s si_value=%d si_pid=%d (sender)\n",
           si.si_code == SI_MESGQ ? "SI_MESGQ" : "other",
           si.si_value.sival_int, (int)si.si_pid);
    CHECK(si.si_code == SI_MESGQ);
    CHECK(si.si_value.sival_int == COOKIE);
    CHECK(si.si_pid == pid);
    printf("   (the child's own mq_notify got EBUSY: one registration per "
           "queue)\n");
    CHECK(drain(mq) == 1);

    /* 3. One-shot: the registration was consumed. A new message does NOT
     * signal again. */
    CHECK(mq_send(mq, "no signal", 10, 0) == 0);
    CHECK(!wait_signal(&si, 100));
    printf("3) after it fired once, a new message produced NO signal "
           "(one-shot)\n");

    /* 4. Registering while the queue is non-empty: no signal either,
     * because there is no EMPTY -> NON-EMPTY transition. The correct loop
     * is therefore:  on signal -> re-register FIRST -> then drain until
     * EAGAIN. (Drain first, re-register second = a message arriving in
     * between is never noticed.) */
    register_notify(mq);
    CHECK(!wait_signal(&si, 100));
    CHECK(drain(mq) == 1); /* now empty, still registered */
    CHECK(mq_send(mq, "again", 6, 0) == 0);
    CHECK(wait_signal(&si, 1000));
    CHECK(si.si_code == SI_MESGQ);
    CHECK(drain(mq) == 1);
    printf("4) re-register + drain + new message -> signal again\n");

    /* 5. Unregister with mq_notify(mq, NULL). */
    register_notify(mq);
    CHECK(mq_notify(mq, NULL) == 0);
    CHECK(mq_send(mq, "quiet", 6, 0) == 0);
    CHECK(!wait_signal(&si, 100));
    printf("5) mq_notify(mq, NULL) unregistered: no signal\n");

    CHECK(mq_close(mq) == 0);
    CHECK(mq_unlink(name) == 0);
    printf("03_mq_notify: all checks passed\n");
    return 0;
}
