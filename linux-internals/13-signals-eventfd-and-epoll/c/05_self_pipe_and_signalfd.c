/*
 * 05_self_pipe_and_signalfd.c — turn signals into readable file descriptors
 *                               so poll()/epoll can wait for them
 *
 * Concept : An event loop sleeps in poll(). A signal handler cannot safely
 *           do the real work (async-signal-safety), and setting a flag is
 *           racy: the signal may arrive just BEFORE poll() starts sleeping,
 *           and the loop sleeps on with the flag set. Two fixes:
 *             1. self-pipe trick (portable, any Unix): the handler write()s
 *                one byte into a non-blocking pipe; poll() watches the
 *                read end. write() is async-signal-safe, and the byte stays
 *                there even if poll() starts later: no lost wakeup.
 *             2. signalfd (Linux): block the signals and read them from an
 *                fd as struct signalfd_siginfo. No handler at all.
 *           Demo uses SIGCHLD from a real child and SIGUSR1 with a value.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/05_self_pipe_and_signalfd.c
 * Read    : ../README.md  (section 3.7 "Signals as file descriptors")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
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

/* ------------------------------------------------------------------ */
/* 1. self-pipe trick                                                 */
/* ------------------------------------------------------------------ */
static int g_self_pipe[2];

static void self_pipe_handler(int sig)
{
    int saved = errno;
    unsigned char b = (unsigned char)sig; /* the byte says WHICH signal */
    /* Non-blocking write end: if the pipe is full (64 KiB of pending
     * signals!) we drop the byte instead of deadlocking inside a handler.
     * The reader will still wake for the bytes already in the pipe. */
    ssize_t r = write(g_self_pipe[1], &b, 1);
    (void)r;
    errno = saved;
}

/*
 *   signal ---> handler: write(pipe[1], signo)        (async, tiny, safe)
 *                               |
 *   main loop:  poll({pipe[0], other fds...})  <-- wakes up
 *               read(pipe[0]) -> signo -> do the REAL work here
 *               (waitpid, logging, reconfiguring: normal context)
 */
static void demo_self_pipe(void)
{
    printf("1) self-pipe trick\n");
    CHECK(pipe2(g_self_pipe, O_CLOEXEC | O_NONBLOCK) == 0);
    struct sigaction sa = {0};
    sa.sa_handler = self_pipe_handler;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGCHLD, &sa, NULL) == 0);
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        _exit(3); /* -> SIGCHLD to the parent */
    CHECK(raise(SIGUSR1) == 0);

    int got_chld = 0, got_usr1 = 0;
    while (!(got_chld && got_usr1)) {
        struct pollfd p = {.fd = g_self_pipe[0], .events = POLLIN};
        int r = poll(&p, 1, 3000);
        if (r < 0 && errno == EINTR)
            continue; /* a signal interrupted poll: its byte is in the pipe */
        CHECK(r == 1);
        unsigned char b;
        while (read(g_self_pipe[0], &b, 1) == 1) {
            if (b == SIGCHLD) {
                /* SIGCHLD coalesces: one signal may stand for SEVERAL dead
                 * children. Always reap in a WNOHANG loop. */
                int st;
                pid_t w;
                while ((w = waitpid(-1, &st, WNOHANG)) > 0) {
                    printf("  SIGCHLD -> reaped child %d, exit code %d\n", (int)w,
                           WEXITSTATUS(st));
                    CHECK(w == pid && WIFEXITED(st) && WEXITSTATUS(st) == 3);
                    got_chld = 1;
                }
            } else if (b == SIGUSR1) {
                printf("  SIGUSR1 -> handled in the loop, not in the handler\n");
                got_usr1 = 1;
            }
        }
    }
    signal(SIGCHLD, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    close(g_self_pipe[0]);
    close(g_self_pipe[1]);
}

/* ------------------------------------------------------------------ */
/* 2. signalfd                                                        */
/* ------------------------------------------------------------------ */
/*
 *   sigprocmask(BLOCK, {CHLD, USR1})     <- otherwise the default action
 *   sfd = signalfd(-1, {CHLD, USR1})        or a handler would take them
 *   poll(sfd) -> read(sfd, struct signalfd_siginfo[]) -> ssi_signo, ssi_pid,
 *                                                       ssi_status, ssi_int
 */
static void demo_signalfd(void)
{
    printf("2) signalfd\n");
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigaddset(&mask, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    CHECK(sfd >= 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        _exit(7);
    union sigval v = {.sival_int = 1234};
    CHECK(sigqueue(getpid(), SIGUSR1, v) == 0);

    int got_chld = 0, got_usr1 = 0;
    while (!(got_chld && got_usr1)) {
        struct pollfd p = {.fd = sfd, .events = POLLIN};
        int r = poll(&p, 1, 3000);
        CHECK(r == 1);
        struct signalfd_siginfo si[4];
        ssize_t n = read(sfd, si, sizeof si); /* may return several at once */
        CHECK(n > 0 && n % (ssize_t)sizeof si[0] == 0);
        for (ssize_t i = 0; i < n / (ssize_t)sizeof si[0]; i++) {
            if (si[i].ssi_signo == SIGCHLD) {
                printf("  SIGCHLD: ssi_pid=%u ssi_status=%d (child %d)\n", si[i].ssi_pid,
                       si[i].ssi_status, (int)pid);
                CHECK((pid_t)si[i].ssi_pid == pid && si[i].ssi_status == 7);
                int st; /* signalfd does NOT reap: still waitpid() */
                CHECK(waitpid(pid, &st, 0) == pid && WEXITSTATUS(st) == 7);
                got_chld = 1;
            } else if (si[i].ssi_signo == SIGUSR1) {
                printf("  SIGUSR1: ssi_code=%s ssi_int=%d ssi_pid=%u\n",
                       si[i].ssi_code == SI_QUEUE ? "SI_QUEUE" : "?", si[i].ssi_int,
                       si[i].ssi_pid);
                CHECK(si[i].ssi_int == 1234 && (pid_t)si[i].ssi_pid == getpid());
                got_usr1 = 1;
            }
        }
    }
    /* Nothing left: a non-blocking signalfd says EAGAIN */
    struct signalfd_siginfo extra;
    CHECK(read(sfd, &extra, sizeof extra) == -1 && errno == EAGAIN);
    close(sfd);
    CHECK(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    demo_self_pipe();
    demo_signalfd();
    printf("05_self_pipe_and_signalfd: all checks passed\n");
    return 0;
}
