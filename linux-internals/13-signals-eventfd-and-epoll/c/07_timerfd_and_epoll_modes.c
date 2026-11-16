/*
 * 07_timerfd_and_epoll_modes.c — timerfd; select vs poll vs epoll; level-
 *                                vs edge-triggered; EPOLLONESHOT
 *
 * Concept : timerfd turns a kernel timer into an fd that becomes readable
 *           on expiry; read() returns how many times it expired (missed
 *           ticks are counted, not lost). select(), poll() and epoll all
 *           answer "which of these fds is ready?", with different costs:
 *           select/poll pass the whole list on every call (O(n)), epoll
 *           keeps the interest list in the kernel and returns only ready
 *           fds (O(ready)). epoll has two reporting modes:
 *             level-triggered (default): "the fd IS readable" -> reported
 *               on every epoll_wait while data remains.
 *             edge-triggered (EPOLLET): "the fd BECAME readable" -> reported
 *               once per new arrival; you must read until EAGAIN.
 *           EPOLLONESHOT: report once, then disarm until EPOLL_CTL_MOD.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/07_timerfd_and_epoll_modes.c
 * Read    : ../README.md  (sections 3.9 "timerfd" and 3.10 "select, poll, epoll")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/timerfd.h>
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

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static void demo_timerfd(void)
{
    printf("1) timerfd: periodic 10 ms timer, then sleep 55 ms without reading\n");
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    CHECK(tfd >= 0);
    struct itimerspec its = {.it_value = {0, 10000000}, .it_interval = {0, 10000000}};
    CHECK(timerfd_settime(tfd, 0, &its, NULL) == 0);

    sleep_ms(55);
    uint64_t exp;
    CHECK(read(tfd, &exp, sizeof exp) == sizeof exp);
    printf("  read -> %llu expirations at once (missed ticks are counted)\n",
           (unsigned long long)exp);
    CHECK(exp >= 5); /* at least floor(55/10); more if we were descheduled */

    /* Nothing pending right after a read: EAGAIN (non-blocking) */
    CHECK(read(tfd, &exp, sizeof exp) == -1 && errno == EAGAIN);

    /* Wait for the next tick with poll: this is how an event loop uses it */
    struct pollfd p = {.fd = tfd, .events = POLLIN};
    CHECK(poll(&p, 1, 1000) == 1);
    CHECK(read(tfd, &exp, sizeof exp) == sizeof exp && exp >= 1);

    /* Disarm: all-zero it_value */
    struct itimerspec off = {0};
    CHECK(timerfd_settime(tfd, 0, &off, NULL) == 0);
    close(tfd);
}

static void demo_three_apis(void)
{
    printf("2) the same question three ways: is the pipe readable?\n");
    int pfd[2];
    CHECK(pipe2(pfd, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(write(pfd[1], "x", 1) == 1);

    /* select: a bitmap of fd numbers, rebuilt before EVERY call (select
     * overwrites it), limited to fds < FD_SETSIZE (1024). */
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(pfd[0], &rset);
    struct timeval tv = {0, 0};
    int r = select(pfd[0] + 1, &rset, NULL, NULL, &tv);
    CHECK(r == 1 && FD_ISSET(pfd[0], &rset));
    printf("  select: nfds=%d, FD_ISSET -> readable (FD_SETSIZE=%d)\n", pfd[0] + 1,
           FD_SETSIZE);

    /* poll: an array of {fd, events, revents}; no fd limit, still O(n) */
    struct pollfd p = {.fd = pfd[0], .events = POLLIN};
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN));
    printf("  poll  : revents=POLLIN\n");

    /* epoll: register once (epoll_ctl), then wait many times */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep >= 0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = pfd[0]};
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == 0);
    struct epoll_event out[4];
    CHECK(epoll_wait(ep, out, 4, 0) == 1 && out[0].data.fd == pfd[0]);
    printf("  epoll : epoll_wait -> 1 event, data.fd=%d\n", out[0].data.fd);

    /* epoll refuses regular files: they are "always ready" (EPERM).
     * poll/select would just report them readable forever. */
    char tmpl[] = "/tmp/epoll-file-XXXXXX";
    int ffd = mkstemp(tmpl);
    CHECK(ffd >= 0);
    CHECK(unlink(tmpl) == 0);
    struct epoll_event fev = {.events = EPOLLIN, .data.fd = ffd};
    errno = 0;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ffd, &fev) == -1 && errno == EPERM);
    printf("  epoll_ctl(ADD, regular file) -> EPERM\n");
    close(ffd);
    close(ep);
    close(pfd[0]);
    close(pfd[1]);
}

/*
 * 10 bytes arrive, we read only 1 each time:
 *
 *   level-triggered:  wait -> ready, read 1 | wait -> ready, read 1 | ...
 *                     (reported while ANY data is left)
 *   edge-triggered:   wait -> ready, read 1 | wait -> NOTHING (9 bytes
 *                     stranded!) ... until NEW data arrives -> ready again
 */
static int ready_now(int ep)
{
    struct epoll_event e[2];
    int n = epoll_wait(ep, e, 2, 0);
    CHECK(n >= 0);
    return n;
}

static void demo_lt_et_oneshot(void)
{
    printf("3) level-triggered vs edge-triggered vs EPOLLONESHOT\n");
    int lt[2], et[2], os[2];
    CHECK(pipe2(lt, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(pipe2(et, O_CLOEXEC | O_NONBLOCK) == 0);
    CHECK(pipe2(os, O_CLOEXEC | O_NONBLOCK) == 0);
    int ep_lt = epoll_create1(EPOLL_CLOEXEC), ep_et = epoll_create1(EPOLL_CLOEXEC),
        ep_os = epoll_create1(EPOLL_CLOEXEC);
    CHECK(ep_lt >= 0 && ep_et >= 0 && ep_os >= 0);
    struct epoll_event e = {.events = EPOLLIN, .data.fd = lt[0]};
    CHECK(epoll_ctl(ep_lt, EPOLL_CTL_ADD, lt[0], &e) == 0);
    e = (struct epoll_event){.events = EPOLLIN | EPOLLET, .data.fd = et[0]};
    CHECK(epoll_ctl(ep_et, EPOLL_CTL_ADD, et[0], &e) == 0);
    e = (struct epoll_event){.events = EPOLLIN | EPOLLONESHOT, .data.fd = os[0]};
    CHECK(epoll_ctl(ep_os, EPOLL_CTL_ADD, os[0], &e) == 0);

    CHECK(write(lt[1], "0123456789", 10) == 10);
    CHECK(write(et[1], "0123456789", 10) == 10);
    CHECK(write(os[1], "0123456789", 10) == 10);
    char c;

    int lt_reports = 0;
    while (ready_now(ep_lt) == 1) {
        lt_reports++;
        CHECK(read(lt[0], &c, 1) == 1); /* read only ONE byte */
    }
    printf("  LT : reported %d times (once per wait while data remained)\n", lt_reports);
    CHECK(lt_reports == 10);

    CHECK(ready_now(ep_et) == 1);
    CHECK(read(et[0], &c, 1) == 1);
    int again = ready_now(ep_et);
    printf("  ET : reported once; after reading 1 byte, next wait -> %d events "
           "(9 bytes stranded)\n", again);
    CHECK(again == 0);
    CHECK(write(et[1], "!", 1) == 1); /* new data = new edge */
    CHECK(ready_now(ep_et) == 1);
    /* Correct ET consumer: read until EAGAIN */
    int drained = 0;
    while (read(et[0], &c, 1) == 1)
        drained++;
    CHECK(errno == EAGAIN && drained == 10);
    printf("  ET : new write -> reported again; drained %d bytes until EAGAIN\n", drained);

    CHECK(ready_now(ep_os) == 1);
    CHECK(ready_now(ep_os) == 0); /* disarmed although data remains */
    e = (struct epoll_event){.events = EPOLLIN | EPOLLONESHOT, .data.fd = os[0]};
    CHECK(epoll_ctl(ep_os, EPOLL_CTL_MOD, os[0], &e) == 0); /* re-arm */
    CHECK(ready_now(ep_os) == 1);
    printf("  ONESHOT: reported once, silent until EPOLL_CTL_MOD re-arms it\n");
    /* ONESHOT is how a thread pool shares one epoll fd: the thread that
     * got the event owns the fd until it re-arms it, so two threads never
     * handle the same connection at once. (EPOLLEXCLUSIVE is the other
     * multi-thread flag: it wakes only one of several epoll fds waiting on
     * the same listening socket — avoids the "thundering herd".) */

    int fds[] = {lt[0], lt[1], et[0], et[1], os[0], os[1], ep_lt, ep_et, ep_os};
    for (size_t i = 0; i < sizeof fds / sizeof fds[0]; i++)
        close(fds[i]);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    demo_timerfd();
    demo_three_apis();
    demo_lt_et_oneshot();
    printf("07_timerfd_and_epoll_modes: all checks passed\n");
    return 0;
}
