/*
 * 05_msg_zerocopy.c — MSG_ZEROCOPY on a TCP socket: SO_ZEROCOPY, sending
 *                     without copying, and reading completion notifications
 *                     from the socket error queue
 *
 * Concept : send() normally copies your buffer into kernel socket buffers
 *           and returns; you may reuse the buffer at once. With
 *           MSG_ZEROCOPY the kernel PINS your pages and lets the NIC DMA
 *           from them, so send() returns while the data may still be in
 *           flight: you must NOT touch the buffer until the kernel says
 *           "done". It says so asynchronously, as a notification on the
 *           socket's ERROR QUEUE (recvmsg(MSG_ERRQUEUE)) carrying a range of
 *           completed send() calls [lo, hi]. If the kernel had to copy
 *           anyway (loopback, or a NIC without scatter-gather), the
 *           notification has SO_EE_CODE_ZEROCOPY_COPIED. Worth it only for
 *           large sends (~10 KB+): pinning and notifications have a cost.
 *           Over loopback (this demo) it is always COPIED, no gain — the
 *           point is the protocol. Skips gracefully if unsupported.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/05_msg_zerocopy.c
 * Read    : ../README.md  (section 3.6 "MSG_ZEROCOPY")
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <linux/errqueue.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#ifndef SO_ZEROCOPY
#define SO_ZEROCOPY 60
#endif
#ifndef MSG_ZEROCOPY
#define MSG_ZEROCOPY 0x4000000
#endif

#define CHUNK   (64u << 10) /* 64 KiB per send: large sends are where it pays */
#define NCHUNKS 32          /* 2 MiB total */
#define TOTAL   (CHUNK * NCHUNKS)

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

struct rx {
    int fd;
    unsigned char *dst; /* TOTAL bytes, owned by main */
    size_t got;
};

static void *receiver(void *arg)
{
    struct rx *r = arg;
    while (r->got < TOTAL) {
        ssize_t n = recv(r->fd, r->dst + r->got, TOTAL - r->got, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        r->got += (size_t)n;
    }
    return NULL;
}

/* A connected TCP pair over loopback: listen on port 0 (kernel picks a
 * free port, so parallel runs never collide), connect, accept. */
static void tcp_pair(int *client, int *server)
{
    int l = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(l >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = 0};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(l, (struct sockaddr *)&a, sizeof a) == 0);
    CHECK(listen(l, 1) == 0);
    socklen_t al = sizeof a;
    CHECK(getsockname(l, (struct sockaddr *)&a, &al) == 0);
    *client = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(*client >= 0);
    CHECK(connect(*client, (struct sockaddr *)&a, sizeof a) == 0);
    *server = accept4(l, NULL, NULL, SOCK_CLOEXEC);
    CHECK(*server >= 0);
    close(l);
}

struct zc_state {
    uint32_t completed; /* number of send() calls the kernel released */
    uint32_t copied;    /* ... of which it had to copy anyway        */
};

/* Read every notification currently on the error queue (non-blocking).
 *
 *   recvmsg(fd, MSG_ERRQUEUE) -> cmsg(SOL_IP, IP_RECVERR) -> sock_extended_err
 *       ee_origin = SO_EE_ORIGIN_ZEROCOPY
 *       ee_info   = lo   (first send() call number, counting from 0)
 *       ee_data   = hi   (last one; ranges are coalesced)
 *       ee_code   & SO_EE_CODE_ZEROCOPY_COPIED -> the kernel copied after all
 */
static void drain_errqueue(int fd, struct zc_state *st)
{
    for (;;) {
        char control[128];
        struct msghdr msg = {.msg_control = control, .msg_controllen = sizeof control};
        ssize_t r = recvmsg(fd, &msg, MSG_ERRQUEUE | MSG_DONTWAIT);
        if (r < 0) {
            CHECK(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
            return;
        }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (!((c->cmsg_level == SOL_IP && c->cmsg_type == IP_RECVERR) ||
                  (c->cmsg_level == SOL_IPV6 && c->cmsg_type == IPV6_RECVERR)))
                continue;
            struct sock_extended_err serr;
            memcpy(&serr, CMSG_DATA(c), sizeof serr); /* CMSG_DATA may be unaligned */
            if (serr.ee_origin != SO_EE_ORIGIN_ZEROCOPY || serr.ee_errno != 0)
                continue;
            uint32_t n = serr.ee_data - serr.ee_info + 1;
            st->completed += n;
            if (serr.ee_code & SO_EE_CODE_ZEROCOPY_COPIED)
                st->copied += n;
        }
    }
}

/* Send TOTAL bytes in CHUNK pieces, plain or zero-copy. Returns the number
 * of send() calls made (each zerocopy call gets the next sequence number). */
static uint32_t send_all(int fd, const unsigned char *buf, int flags, struct zc_state *st)
{
    uint32_t calls = 0;
    size_t off = 0;
    while (off < TOTAL) {
        size_t len = TOTAL - off < CHUNK ? TOTAL - off : CHUNK;
        ssize_t n = send(fd, buf + off, len, flags);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == ENOBUFS && (flags & MSG_ZEROCOPY)) {
            /* too many un-notified zerocopy sends (optmem_max): collect
             * notifications, then retry */
            struct pollfd p = {.fd = fd, .events = 0};
            poll(&p, 1, 100);
            drain_errqueue(fd, st);
            continue;
        }
        CHECK(n > 0);
        off += (size_t)n;
        calls++;
    }
    return calls;
}

static int run(const unsigned char *src, int zerocopy, double *ms, struct zc_state *st)
{
    int c, s;
    tcp_pair(&c, &s);
    if (zerocopy) {
        int one = 1;
        if (setsockopt(c, SOL_SOCKET, SO_ZEROCOPY, &one, sizeof one) != 0) {
            int e = errno;
            close(c);
            close(s);
            return e; /* kernel < 4.14 or zerocopy compiled out */
        }
    }
    unsigned char *dst = calloc(1, TOTAL);
    CHECK(dst);
    struct rx r = {.fd = s, .dst = dst};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, receiver, &r) == 0);

    double t0 = now_ms();
    uint32_t calls = send_all(c, src, zerocopy ? MSG_ZEROCOPY : 0, st);
    if (zerocopy) {
        /* Wait (max 5 s) until every send() is released. Only now may the
         * sender modify or free `src`. POLLERR = error queue non-empty;
         * it is always reported, no need to ask for it in .events. */
        double deadline = now_ms() + 5000;
        while (st->completed < calls && now_ms() < deadline) {
            struct pollfd p = {.fd = c, .events = 0};
            poll(&p, 1, 100);
            drain_errqueue(c, st);
        }
        CHECK(st->completed == calls);
    }
    CHECK(pthread_join(t, NULL) == 0);
    *ms = now_ms() - t0;
    CHECK(r.got == TOTAL && memcmp(dst, src, TOTAL) == 0);
    free(dst);
    close(c);
    close(s);
    return 0;
}

int main(void)
{
    alarm(20);
    unsigned char *src = malloc(TOTAL);
    CHECK(src);
    for (size_t i = 0; i < TOTAL; i++)
        src[i] = (unsigned char)(i ^ (i >> 9));

    double t_plain, t_zc;
    struct zc_state none = {0}, st = {0};
    CHECK(run(src, 0, &t_plain, &none) == 0);
    printf("1) plain send():       %u x %u KiB over TCP loopback in %.2f ms\n", NCHUNKS,
           CHUNK >> 10, t_plain);

    int e = run(src, 1, &t_zc, &st);
    if (e != 0) {
        printf("2) SO_ZEROCOPY: %s -> not supported by this kernel; skipping\n", strerror(e));
    } else {
        printf("2) send(MSG_ZEROCOPY): %.2f ms; %u send() calls released by the kernel,\n"
               "   %u of them flagged SO_EE_CODE_ZEROCOPY_COPIED\n",
               t_zc, st.completed, st.copied);
        printf("   Loopback always copies (the receiver is local): no gain here.\n"
               "   On a real NIC with scatter-gather the pages are DMA'd from your buffer:\n"
               "   the win appears with sends of tens of KiB and up, at 10 Gbit/s and more.\n");
        CHECK(st.completed > 0);
    }
    free(src); /* safe only now: every zerocopy send has been released */
    printf("05_msg_zerocopy: all checks passed\n");
    return 0;
}
