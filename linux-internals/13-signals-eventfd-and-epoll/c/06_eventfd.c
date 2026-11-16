/*
 * 06_eventfd.c — eventfd: a 64-bit kernel counter you can poll()
 *
 * Concept : eventfd is the smallest possible wake-up object: one uint64
 *           counter in the kernel behind an fd. write(8 bytes) ADDS to it;
 *           read(8 bytes) returns the value and resets it to 0 (or, in
 *           EFD_SEMAPHORE mode, returns 1 and decrements). The fd is
 *           readable while the counter is > 0, so it fits into poll/epoll
 *           next to sockets, pipes, timers and signals. Cheaper than a pipe
 *           (one fd instead of two, 8 bytes, no buffer) and it never fills
 *           up with "wake-up bytes" — repeated wake-ups just add up.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/c/06_eventfd.c
 *           make run P=linux-internals/13-signals-eventfd-and-epoll/c/06_eventfd.c SAN=tsan
 * Read    : ../README.md  (section 3.8 "eventfd")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
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

static void efd_add(int fd, uint64_t v) { CHECK(write(fd, &v, sizeof v) == sizeof v); }

static uint64_t efd_take(int fd)
{
    uint64_t v;
    ssize_t n;
    do
        n = read(fd, &v, sizeof v);
    while (n < 0 && errno == EINTR);
    CHECK(n == sizeof v);
    return v;
}

/*
 *   counter mode                         semaphore mode (EFD_SEMAPHORE)
 *   write 3  -> [3]                       write 3 -> [3]
 *   write 4  -> [7]                       read    -> 1, [2]
 *   read     -> 7, [0]                    read    -> 1, [1]
 *   read     -> blocks / EAGAIN           read    -> 1, [0]
 *                                         read    -> blocks / EAGAIN
 */
static void demo_semantics(void)
{
    printf("1) counter semantics\n");
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(fd >= 0);
    efd_add(fd, 3);
    efd_add(fd, 4);
    uint64_t v = efd_take(fd);
    printf("  write 3, write 4, read -> %llu (sum, counter reset)\n", (unsigned long long)v);
    CHECK(v == 7);
    uint64_t dummy;
    CHECK(read(fd, &dummy, sizeof dummy) == -1 && errno == EAGAIN);

    /* Only 8-byte reads/writes are allowed */
    uint32_t small = 1;
    CHECK(write(fd, &small, sizeof small) == -1 && errno == EINVAL);

    /* Overflow: the max value is 2^64 - 2. A write that would exceed it
     * blocks (or EAGAIN), i.e. a producer gets back-pressure. */
    efd_add(fd, UINT64_MAX - 1);
    uint64_t one = 1;
    CHECK(write(fd, &one, sizeof one) == -1 && errno == EAGAIN);
    printf("  counter at 2^64-2: one more write -> EAGAIN (would block)\n");
    CHECK(efd_take(fd) == UINT64_MAX - 1);
    close(fd);

    printf("2) EFD_SEMAPHORE\n");
    fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    CHECK(fd >= 0);
    efd_add(fd, 3);
    int reads = 0;
    while (read(fd, &v, sizeof v) == sizeof v) {
        CHECK(v == 1);
        reads++;
    }
    CHECK(errno == EAGAIN);
    printf("  write 3 -> %d reads of 1 each, then EAGAIN (a counting semaphore)\n", reads);
    CHECK(reads == 3);
    close(fd);
}

/*
 * Parent <-> child: eventfd created BEFORE fork is shared (same counter).
 * One counter per direction, otherwise a process could read its own
 * wake-up back.
 *
 *   parent                         child
 *   efd_add(to_child, 1)  ------>  efd_take(to_child)  (was blocked)
 *   efd_take(to_parent)   <------  efd_add(to_parent, 100)
 */
static void demo_fork(void)
{
    printf("3) parent -> child wake-up across fork()\n");
    int to_child = eventfd(0, EFD_CLOEXEC);
    int to_parent = eventfd(0, EFD_CLOEXEC);
    CHECK(to_child >= 0 && to_parent >= 0);
    pid_t pid = fork(); /* single-threaded at this point: fork first */
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5); /* watchdog: never block forever */
        uint64_t v;
        if (read(to_child, &v, sizeof v) != sizeof v || v != 1)
            _exit(1);
        v = 100;
        _exit(write(to_parent, &v, sizeof v) == sizeof v ? 0 : 1);
    }
    efd_add(to_child, 1);
    uint64_t v = efd_take(to_parent);
    printf("  child woke up and answered %llu\n", (unsigned long long)v);
    CHECK(v == 100);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(to_child);
    close(to_parent);
}

/* ---- thread to thread: a work counter and a stop flag, one poll() ---- */
#define N_PRODUCERS 3
#define N_POSTS 200

struct ctx {
    int work_fd;           /* counter mode: how many items were posted */
    int stop_fd;           /* "please exit" */
    uint64_t consumed;     /* written by the consumer only, read after join */
    unsigned wakeups;
};

static void *producer(void *arg)
{
    struct ctx *c = arg;
    for (int i = 0; i < N_POSTS; i++)
        efd_add(c->work_fd, 1); /* atomic add in the kernel: no lock needed */
    return NULL;
}

static void *consumer(void *arg)
{
    struct ctx *c = arg;
    for (;;) {
        struct pollfd p[2] = {{.fd = c->work_fd, .events = POLLIN},
                              {.fd = c->stop_fd, .events = POLLIN}};
        int r = poll(p, 2, 5000);
        if (r < 0 && errno == EINTR)
            continue;
        CHECK(r > 0);
        c->wakeups++;
        if (p[0].revents & POLLIN)
            c->consumed += efd_take(c->work_fd); /* many posts, one read */
        if (p[1].revents & POLLIN) {
            /* Stop only after draining: work posted before "stop" counts. */
            uint64_t v;
            if (read(c->work_fd, &v, sizeof v) == sizeof v)
                c->consumed += v;
            return NULL;
        }
    }
}

static void demo_threads(void)
{
    printf("4) thread -> thread: %d producers x %d posts, one consumer\n", N_PRODUCERS,
           N_POSTS);
    struct ctx c = {.work_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK),
                    .stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)};
    CHECK(c.work_fd >= 0 && c.stop_fd >= 0);
    pthread_t cons, prod[N_PRODUCERS];
    CHECK(pthread_create(&cons, NULL, consumer, &c) == 0);
    for (int i = 0; i < N_PRODUCERS; i++)
        CHECK(pthread_create(&prod[i], NULL, producer, &c) == 0);
    for (int i = 0; i < N_PRODUCERS; i++)
        CHECK(pthread_join(prod[i], NULL) == 0);
    efd_add(c.stop_fd, 1); /* every post happened before this */
    CHECK(pthread_join(cons, NULL) == 0);
    printf("  consumed %llu posts in %u wake-ups (coalesced; timing-dependent)\n",
           (unsigned long long)c.consumed, c.wakeups);
    CHECK(c.consumed == (uint64_t)N_PRODUCERS * N_POSTS);
    close(c.work_fd);
    close(c.stop_fd);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    demo_semantics();
    demo_fork();    /* fork while still single-threaded ... */
    demo_threads(); /* ... threads afterwards */
    printf("06_eventfd: all checks passed\n");
    return 0;
}
