/*
 * ksync_poll.c — user-space test for /dev/ksync: O_NONBLOCK, poll(), epoll,
 *                and a read() that really sleeps in the driver's wait queue
 *
 * Concept : From user space a driver's wait queue is invisible — you only
 *           see that read() blocks, that O_NONBLOCK gives EAGAIN instead,
 *           and that poll()/epoll_wait() wake up when data arrives. This
 *           program checks each of those behaviours of ksync_chardev.ko.
 * Build   : make drivers   (-> build/linux-device-drivers/01-kernel-synchronization/user/ksync_poll)
 * Run     : sudo insmod kernel/ksync_chardev.ko            # all tests
 *           sudo ./ksync_poll
 *           or: sudo insmod kernel/ksync_chardev.ko tick_ms=200   # timer test
 *           Needs the module loaded, so `make test` does NOT run it.
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = no device.
 * Read    : ../README.md  (section 3.5 "Waiting: completions and wait queues")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

#define DEV "/dev/ksync"
#define FIFO_SIZE 4096 /* KSYNC_FIFO_SIZE in ksync_chardev.c */

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* Read everything currently queued so each test starts from empty. */
static size_t drain(int fd_nb)
{
    char buf[512];
    size_t total = 0;
    ssize_t n;

    while ((n = read(fd_nb, buf, sizeof(buf))) > 0)
        total += (size_t)n;
    CHECK(n < 0 && errno == EAGAIN); /* empty FIFO, non-blocking fd */
    return total;
}

static unsigned int module_tick_ms(void)
{
    unsigned int v = 0;
    FILE *f = fopen("/sys/module/ksync_chardev/parameters/tick_ms", "r");

    if (f) {
        if (fscanf(f, "%u", &v) != 1)
            v = 0;
        fclose(f);
    }
    return v;
}

/* ---- test: timer-produced data (module loaded with tick_ms > 0) -------- */
/*
 * The timer callback is our fake interrupt. poll() sleeps in the kernel
 * until tick_fn() calls wake_up_interruptible() on the driver's wait queue.
 */
static void test_ticks(int fd_nb, unsigned int tick_ms)
{
    struct pollfd p = { .fd = fd_nb, .events = POLLIN };
    char buf[128];
    ssize_t n;
    int i;

    for (i = 0; i < 3; i++) {
        drain(fd_nb);
        CHECK(poll(&p, 1, (int)(3 * tick_ms + 100)) == 1);
        CHECK(p.revents & POLLIN);
        n = read(fd_nb, buf, sizeof(buf) - 1);
        CHECK(n > 0);
        buf[n] = '\0';
        CHECK(strncmp(buf, "tick ", 5) == 0);
        printf("  poll woke up for timer data: %s", buf);
    }
    printf("PASS ticks: 3 timer lines delivered through poll()\n");
}

/* ---- test: non-blocking read on an empty device ------------------------ */
static void test_nonblock(int fd_nb)
{
    char c;

    errno = 0;
    CHECK(read(fd_nb, &c, 1) == -1 && errno == EAGAIN);
    printf("PASS nonblock: empty read() -> EAGAIN instead of sleeping\n");
}

/* ---- test: poll() readiness ------------------------------------------- */
static void test_poll(int fd_nb)
{
    struct pollfd p = { .fd = fd_nb, .events = POLLIN | POLLOUT };
    char buf[16] = { 0 };
    double t0;

    /* Empty: writable but not readable. */
    CHECK(poll(&p, 1, 0) == 1);
    CHECK((p.revents & POLLOUT) && !(p.revents & POLLIN));

    /* Only POLLIN asked: nothing ready -> times out after ~100 ms. */
    p.events = POLLIN;
    t0 = now_ms();
    CHECK(poll(&p, 1, 100) == 0);
    CHECK(now_ms() - t0 >= 90);

    CHECK(write(fd_nb, "hello\n", 6) == 6);
    CHECK(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN));
    CHECK(read(fd_nb, buf, sizeof(buf)) == 6);
    CHECK(memcmp(buf, "hello\n", 6) == 0);
    printf("PASS poll: POLLOUT when empty, 100 ms timeout, POLLIN after write\n");
}

/* ---- test: blocking read() sleeps until a writer arrives -------------- */
/*
 *   reader thread                     main thread
 *   read(fd_block) ... sleeping       sleep 200 ms
 *   (wait_event_interruptible)        write("wake\n")  -> wake_up_interruptible
 *   returns "wake\n" after ~200 ms
 */
struct reader_arg {
    int fd;
    char buf[16];
    ssize_t n;
    double waited_ms;
};

static void *blocking_reader(void *p)
{
    struct reader_arg *a = p;
    double t0 = now_ms();

    a->n = read(a->fd, a->buf, sizeof(a->buf));
    a->waited_ms = now_ms() - t0;
    return NULL;
}

static void test_blocking_read(int fd_nb)
{
    struct reader_arg a = { 0 };
    struct timespec d = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };
    pthread_t th;

    a.fd = open(DEV, O_RDWR); /* blocking fd for the reader */
    CHECK(a.fd >= 0);
    CHECK(pthread_create(&th, NULL, blocking_reader, &a) == 0);

    nanosleep(&d, NULL);
    CHECK(write(fd_nb, "wake\n", 5) == 5);
    CHECK(pthread_join(th, NULL) == 0);
    close(a.fd);

    CHECK(a.n == 5 && memcmp(a.buf, "wake\n", 5) == 0);
    CHECK(a.waited_ms >= 150);
    printf("PASS blocking: read() slept %.0f ms in the wait queue until write()\n",
           a.waited_ms);
}

/* ---- test: full FIFO -> write EAGAIN, no POLLOUT ---------------------- */
static void test_full(int fd_nb)
{
    struct pollfd p = { .fd = fd_nb, .events = POLLOUT };
    char chunk[256];
    size_t total = 0;
    ssize_t n;

    memset(chunk, 'x', sizeof(chunk));
    while ((n = write(fd_nb, chunk, sizeof(chunk))) > 0)
        total += (size_t)n;
    CHECK(n == -1 && errno == EAGAIN);
    CHECK(total == FIFO_SIZE);
    CHECK(poll(&p, 1, 0) == 0); /* not writable */
    CHECK(drain(fd_nb) == FIFO_SIZE);
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLOUT));
    printf("PASS full: %zu bytes fill the FIFO, then write() -> EAGAIN and no POLLOUT\n",
           total);
}

/* ---- test: epoll uses the same wait queue via poll_wait() -------------- */
static void test_epoll(int fd_nb)
{
    struct epoll_event ev = { .events = EPOLLIN }, out;
    char buf[8];
    int ep = epoll_create1(EPOLL_CLOEXEC);

    CHECK(ep >= 0);
    ev.data.fd = fd_nb;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd_nb, &ev) == 0);
    CHECK(epoll_wait(ep, &out, 1, 50) == 0); /* empty */
    CHECK(write(fd_nb, "ep\n", 3) == 3);
    CHECK(epoll_wait(ep, &out, 1, 1000) == 1 && out.data.fd == fd_nb);
    CHECK(read(fd_nb, buf, sizeof(buf)) == 3);
    close(ep);
    printf("PASS epoll: epoll_wait() woke for the same driver wait queue\n");
}

int main(void)
{
    unsigned int tick_ms;
    int fd_nb = open(DEV, O_RDWR | O_NONBLOCK);

    if (fd_nb < 0) {
        fprintf(stderr, "open %s: %s\n"
                        "load the module first: sudo insmod ksync_chardev.ko\n",
                DEV, strerror(errno));
        return 2;
    }
    drain(fd_nb);

    tick_ms = module_tick_ms();
    if (tick_ms) {
        /* Timer data would interfere with the exact-byte tests below. */
        printf("module loaded with tick_ms=%u: running the timer test only\n",
               tick_ms);
        test_ticks(fd_nb, tick_ms);
    } else {
        test_nonblock(fd_nb);
        test_poll(fd_nb);
        test_blocking_read(fd_nb);
        test_full(fd_nb);
        test_epoll(fd_nb);
        printf("(load with tick_ms=200 to also test timer-produced data)\n");
    }
    close(fd_nb);
    printf("all /dev/ksync checks passed\n");
    return 0;
}
