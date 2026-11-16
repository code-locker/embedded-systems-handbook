/*
 * 02_mq_producer_consumer.c — a producer process and a consumer process
 *                             exchange struct messages over a POSIX mq;
 *                             the consumer waits with poll() on the mqd
 *
 * Concept : A message queue is a ready-made bounded producer/consumer
 *           buffer in the kernel: the producer blocks when it is full, the
 *           consumer when it is empty — no mutex, no condvar, no ring code.
 *           Messages are COPIED in and out (2 copies per message), so this
 *           suits small control/telemetry messages, not video frames.
 *           On Linux an mqd_t is a file descriptor: you can poll()/epoll()
 *           it together with sockets and timers (not portable POSIX).
 * Build   : make run P=linux-internals/10-ipc-message-queues/c/02_mq_producer_consumer.c
 * Read    : ../README.md  (section "3.2 Producer/consumer over an mq")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

#define N_DATA 500
#define URGENT_AT 250 /* producer sends one urgent message after this seq */
#define PRIO_DATA 0
#define PRIO_URGENT 10

enum kind { MSG_DATA = 1, MSG_URGENT = 2, MSG_END = 3 };

/* The wire format. Fixed size, no pointers (a pointer means nothing in
 * another process), explicit-width types so both sides agree. */
struct msg {
    uint32_t kind;
    uint32_t seq;
    uint64_t value;
    uint32_t checksum; /* lets the consumer detect corrupted/mixed data */
    uint32_t pad;      /* explicit padding: no uninitialised bytes sent */
};

static uint32_t checksum(const struct msg *m)
{
    return (m->kind * 2654435761u) ^ (m->seq * 40503u) ^
           (uint32_t)(m->value ^ (m->value >> 32));
}

static void send_msg(mqd_t mq, uint32_t kind, uint32_t seq, unsigned prio)
{
    struct msg m = {.kind = kind, .seq = seq, .value = (uint64_t)seq * seq};
    m.checksum = checksum(&m);
    /* Blocking send: sleeps while the queue holds maxmsg messages. That
     * is the BACKPRESSURE that keeps a fast producer from running away. */
    int r;
    do
        r = mq_send(mq, (const char *)&m, sizeof m, prio);
    while (r < 0 && errno == EINTR);
    CHECK(r == 0);
}

int main(void)
{
    alarm(10);
    char name[64];
    snprintf(name, sizeof name, "/mq_prodcons_%d", (int)getpid());
    struct mq_attr attr = {.mq_maxmsg = 8, .mq_msgsize = sizeof(struct msg)};
    /* Consumer side: non-blocking, because we wait in poll() instead. */
    mqd_t rx = mq_open(name, O_CREAT | O_EXCL | O_RDONLY | O_NONBLOCK |
                                 O_CLOEXEC, 0600, &attr);
    CHECK(rx != (mqd_t)-1);

    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* ---- producer: opens the queue BY NAME, like a separate program
         * would. (It also inherited rx, which it does not use.) ---- */
        alarm(5);
        mq_close(rx);
        mqd_t tx = mq_open(name, O_WRONLY | O_CLOEXEC);
        CHECK(tx != (mqd_t)-1);
        for (uint32_t s = 0; s < N_DATA; s++) {
            send_msg(tx, MSG_DATA, s, PRIO_DATA);
            if (s == URGENT_AT)
                send_msg(tx, MSG_URGENT, s, PRIO_URGENT); /* jumps queue */
        }
        /* END at the SAME priority as data: it must not overtake data still
         * queued. Sending END at a high priority is a classic bug — the
         * consumer would stop before draining the queue. */
        send_msg(tx, MSG_END, N_DATA, PRIO_DATA);
        mq_close(tx);
        _exit(0);
    }

    /* ---- consumer ---- */
    struct pollfd pfd = {.fd = (int)rx, .events = POLLIN};
    uint32_t next = 0;
    int urgent_seen = 0, polls = 0;
    uint32_t data_before_urgent = 0;
    for (;;) {
        int pr = poll(&pfd, 1, 2000); /* 2 s: producer died? don't hang */
        if (pr < 0 && errno == EINTR)
            continue;
        CHECK(pr == 1);
        polls++;
        /* Drain everything that is there now. */
        for (;;) {
            struct msg m;
            unsigned prio;
            ssize_t n = mq_receive(rx, (char *)&m, sizeof m, &prio);
            if (n < 0 && errno == EAGAIN)
                break;
            CHECK(n == (ssize_t)sizeof m); /* whole message or nothing */
            CHECK(m.checksum == checksum(&m));
            if (m.kind == MSG_DATA) {
                CHECK(prio == PRIO_DATA);
                CHECK(m.seq == next); /* FIFO within one priority */
                CHECK(m.value == (uint64_t)m.seq * m.seq);
                next++;
            } else if (m.kind == MSG_URGENT) {
                CHECK(prio == PRIO_URGENT);
                urgent_seen++;
                data_before_urgent = next;
            } else {
                CHECK(m.kind == MSG_END);
                goto done;
            }
        }
    }
done:
    CHECK(mq_close(rx) == 0);
    CHECK(mq_unlink(name) == 0);

    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    printf("consumer: %u data messages in order, checksums OK, then END\n",
           next);
    printf("urgent message (sent right after data #%d) was received after "
           "%u data messages: priority let it overtake %u queued ones "
           "(timing-dependent)\n",
           URGENT_AT, data_before_urgent,
           URGENT_AT + 1 > data_before_urgent
               ? URGENT_AT + 1 - data_before_urgent
               : 0);
    printf("poll() returned %d time(s) for %d messages "
           "(timing-dependent)\n", polls, N_DATA + 2);
    CHECK(next == N_DATA);
    CHECK(urgent_seen == 1);
    CHECK(data_before_urgent <= URGENT_AT + 1);

    printf("02_mq_producer_consumer: all checks passed\n");
    return 0;
}
