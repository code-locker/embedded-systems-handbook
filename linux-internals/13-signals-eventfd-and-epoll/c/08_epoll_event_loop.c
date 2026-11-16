/*
 * 08_epoll_event_loop.c — one epoll loop multiplexing a signalfd, an
 *                         eventfd, a timerfd and a pipe; clean shutdown on
 *                         SIGTERM
 *
 * Concept : The shape of every Linux daemon's main loop (systemd, GLib's
 *           GMainLoop, libuv, nginx workers): ONE thread sleeps in
 *           epoll_wait() on everything it cares about, and each fd type has
 *           a handler. Signals arrive through signalfd, so the "handler" is
 *           ordinary code; shutdown is just another event. Here:
 *             timerfd  every 10 ms  -> tick: send a line into the pipe and
 *                                       add 1 to the eventfd
 *             pipe     readable     -> count received lines
 *             eventfd  readable     -> count notifications
 *             signalfd SIGTERM      -> stop the loop, drain, clean up
 *           After 5 ticks the program sends SIGTERM to ITSELF, exactly like
 *           `systemctl stop` or `kill <pid>` would.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/08_epoll_event_loop.c
 * Read    : ../README.md  (section 3.11 "An epoll event loop")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define TICKS_BEFORE_TERM 5

enum src { SRC_SIGNAL, SRC_EVENT, SRC_TIMER, SRC_PIPE };
static const char *const SRC_NAME[] = {"signalfd", "eventfd", "timerfd", "pipe"};

struct loop {
    int ep, sfd, efd, tfd, pipe_r, pipe_w;
    int running;
    /* statistics, all owned by the single loop thread */
    uint64_t ticks, notes_sent, notes_seen, lines_sent, lines_seen;
    int term_sent, term_seen;
    char linebuf[256];
    size_t linelen;
};

static void add_fd(struct loop *L, int fd, enum src s)
{
    /* data.u32 = which source: the loop dispatches on it (GLib stores a
     * GSource pointer, libuv a uv_handle_t* — same idea). */
    struct epoll_event ev = {.events = EPOLLIN, .data.u32 = s};
    CHECK(epoll_ctl(L->ep, EPOLL_CTL_ADD, fd, &ev) == 0);
}

/* ----------------------------- handlers --------------------------------- */
static void on_timer(struct loop *L)
{
    uint64_t exp;
    if (read(L->tfd, &exp, sizeof exp) != sizeof exp)
        return; /* EAGAIN: spurious, fine */
    for (uint64_t i = 0; i < exp; i++) {
        L->ticks++;
        char line[32];
        int n = snprintf(line, sizeof line, "tick %llu\n", (unsigned long long)L->ticks);
        CHECK(write(L->pipe_w, line, (size_t)n) == n); /* < PIPE_BUF: atomic */
        L->lines_sent++;
        uint64_t one = 1;
        CHECK(write(L->efd, &one, sizeof one) == sizeof one);
        L->notes_sent++;
    }
    if (L->ticks >= TICKS_BEFORE_TERM && !L->term_sent) {
        printf("  [timer] %llu ticks: sending SIGTERM to myself\n",
               (unsigned long long)L->ticks);
        CHECK(kill(getpid(), SIGTERM) == 0); /* arrives via signalfd */
        L->term_sent = 1;
    }
}

static void on_pipe(struct loop *L)
{
    /* Bytes may come in any chunking: accumulate and split on '\n' */
    ssize_t n;
    while ((n = read(L->pipe_r, L->linebuf + L->linelen,
                     sizeof L->linebuf - L->linelen)) > 0) {
        L->linelen += (size_t)n;
        char *nl;
        while ((nl = memchr(L->linebuf, '\n', L->linelen)) != NULL) {
            size_t len = (size_t)(nl - L->linebuf) + 1;
            L->lines_seen++;
            memmove(L->linebuf, L->linebuf + len, L->linelen - len);
            L->linelen -= len;
        }
    }
    CHECK(n == -1 && errno == EAGAIN); /* non-blocking: read until empty */
}

static void on_event(struct loop *L)
{
    uint64_t v;
    if (read(L->efd, &v, sizeof v) == sizeof v)
        L->notes_seen += v; /* several writes, one read: they add up */
}

static void on_signal(struct loop *L)
{
    struct signalfd_siginfo si;
    while (read(L->sfd, &si, sizeof si) == sizeof si) {
        printf("  [signal] %s from pid %u -> stopping the loop\n",
               sigabbrev_np((int)si.ssi_signo), si.ssi_pid);
        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) {
            CHECK((pid_t)si.ssi_pid == getpid());
            L->term_seen = 1;
            L->running = 0; /* finish this round, then leave */
        }
    }
}

/* ------------------------------- setup ---------------------------------- */
static void loop_init(struct loop *L)
{
    memset(L, 0, sizeof *L);
    /* 1. Block the signals FIRST, so they can only be read via signalfd
     *    (and the default action can never kill us halfway). */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    CHECK(sigprocmask(SIG_BLOCK, &mask, NULL) == 0);

    L->ep = epoll_create1(EPOLL_CLOEXEC);
    L->sfd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
    L->efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    L->tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
    L->pipe_r = p[0];
    L->pipe_w = p[1];
    CHECK(L->ep >= 0 && L->sfd >= 0 && L->efd >= 0 && L->tfd >= 0);

    struct itimerspec its = {.it_value = {0, 10000000}, .it_interval = {0, 10000000}};
    CHECK(timerfd_settime(L->tfd, 0, &its, NULL) == 0);

    add_fd(L, L->sfd, SRC_SIGNAL);
    add_fd(L, L->efd, SRC_EVENT);
    add_fd(L, L->tfd, SRC_TIMER);
    add_fd(L, L->pipe_r, SRC_PIPE);
    L->running = 1;
}

/*
 *   +-------------------------------------------------------------+
 *   |  while (running)                                            |
 *   |     n = epoll_wait(ep, events, 8, timeout)                  |
 *   |     for each event: switch (data.u32)                       |
 *   |        SIGNAL -> on_signal   EVENT -> on_event              |
 *   |        TIMER  -> on_timer    PIPE  -> on_pipe               |
 *   |  drain what is still queued, close everything               |
 *   +-------------------------------------------------------------+
 */
static void loop_run(struct loop *L)
{
    unsigned wakeups = 0, per_src[4] = {0};
    while (L->running) {
        struct epoll_event ev[8];
        int n = epoll_wait(L->ep, ev, 8, 2000); /* timeout = watchdog */
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0); /* 0 = 2 s without any event: something is broken */
        wakeups++;
        for (int i = 0; i < n; i++) {
            enum src s = (enum src)ev[i].data.u32;
            per_src[s]++;
            switch (s) {
            case SRC_SIGNAL: on_signal(L); break;
            case SRC_EVENT:  on_event(L);  break;
            case SRC_TIMER:  on_timer(L);  break;
            case SRC_PIPE:   on_pipe(L);   break;
            }
        }
    }
    printf("  loop: %u wake-ups;", wakeups);
    for (int s = 0; s < 4; s++)
        printf(" %s=%u", SRC_NAME[s], per_src[s]);
    printf(" (timing-dependent)\n");
}

static void loop_shutdown(struct loop *L)
{
    /* Clean shutdown: stop producing, then consume what is in flight, so
     * nothing sent before SIGTERM is lost. */
    struct itimerspec off = {0};
    CHECK(timerfd_settime(L->tfd, 0, &off, NULL) == 0);
    on_pipe(L);
    on_event(L);
    int fds[] = {L->sfd, L->efd, L->tfd, L->pipe_r, L->pipe_w, L->ep};
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++)
        CHECK(close(fds[i]) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("epoll loop over signalfd + eventfd + timerfd + pipe\n");
    struct loop L;
    loop_init(&L);
    loop_run(&L);
    loop_shutdown(&L);

    printf("  ticks=%llu  lines sent/seen=%llu/%llu  notifications sent/seen=%llu/%llu\n",
           (unsigned long long)L.ticks, (unsigned long long)L.lines_sent,
           (unsigned long long)L.lines_seen, (unsigned long long)L.notes_sent,
           (unsigned long long)L.notes_seen);
    CHECK(L.term_sent && L.term_seen);
    CHECK(L.ticks >= TICKS_BEFORE_TERM);
    CHECK(L.lines_seen == L.lines_sent && L.linelen == 0);
    CHECK(L.notes_seen == L.notes_sent);
    printf("08_epoll_event_loop: all checks passed\n");
    return 0;
}
