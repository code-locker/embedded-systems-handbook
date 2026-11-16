/*
 * 02_dgram_and_seqpacket.c — message boundaries: SOCK_STREAM merges writes,
 *                            SOCK_DGRAM and SOCK_SEQPACKET keep them
 *
 * Concept : The socket TYPE decides whether the kernel remembers where one
 *           write() ended. STREAM = a byte pipe (boundaries lost).
 *           DGRAM = separate packets, connectionless, addressed with
 *           sendto/recvfrom (and, unlike UDP, reliable and ordered on
 *           AF_UNIX). SEQPACKET = DGRAM boundaries + STREAM connection
 *           (accept, EOF when the peer closes). Too-small receive buffers
 *           TRUNCATE a datagram: the rest is thrown away (MSG_TRUNC).
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/02_dgram_and_seqpacket.c
 * Read    : ../README.md  (section 3.3 "Socket types and message boundaries")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static const char *const MSGS[] = {"one", "two", "three"};
#define NMSGS 3

/*
 * STREAM: three write()s, one read().
 *
 *   write("one") write("two") write("three")
 *        \            |            /
 *         v           v           v
 *   receive queue: [o n e t w o t h r e e]   <- just bytes, no fences
 *   read(64) -> "onetwothree" (11 bytes)
 */
static void demo_stream(void)
{
    printf("1) SOCK_STREAM: boundaries are lost\n");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    for (int i = 0; i < NMSGS; i++)
        CHECK(write(sv[0], MSGS[i], strlen(MSGS[i])) == (ssize_t)strlen(MSGS[i]));
    char buf[64];
    ssize_t n = read(sv[1], buf, sizeof buf);
    CHECK(n > 0);
    printf("  3 writes, 1 read() -> %zd bytes \"%.*s\"\n", n, (int)n, buf);
    /* All three writes were queued before the read, and Linux copies across
     * queued segments until the buffer is full: 11 bytes in one go. */
    CHECK(n == 11 && memcmp(buf, "onetwothree", 11) == 0);
    close(sv[0]);
    close(sv[1]);
}

/* Build a filesystem address inside dir */
static socklen_t make_addr(struct sockaddr_un *a, const char *dir, const char *leaf)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    int w = snprintf(a->sun_path, sizeof a->sun_path, "%s/%s", dir, leaf);
    CHECK(w > 0 && (size_t)w < sizeof a->sun_path);
    return (socklen_t)sizeof *a;
}

/*
 * DGRAM with names: like UDP, but on a path.
 *
 *    client (bound to .../client.sock)        server (bound to .../server.sock)
 *    sendto(server, "one")   ------------->   recvfrom() -> "one",  from client.sock
 *    sendto(server, "two")   ------------->   recvfrom() -> "two"
 *    recvfrom()              <-------------   sendto(from, "ack")
 *
 * The client MUST bind too if it wants replies: an unbound datagram
 * sender is anonymous, and the server has no address to answer to.
 */
static void demo_dgram(const char *dir)
{
    printf("2) SOCK_DGRAM: one send = one receive\n");
    struct sockaddr_un saddr, caddr;
    socklen_t slen = make_addr(&saddr, dir, "server.sock");
    socklen_t clen = make_addr(&caddr, dir, "client.sock");

    int srv = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int cli = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(srv >= 0 && cli >= 0);
    CHECK(bind(srv, (struct sockaddr *)&saddr, slen) == 0);
    CHECK(bind(cli, (struct sockaddr *)&caddr, clen) == 0);

    for (int i = 0; i < NMSGS; i++)
        CHECK(sendto(cli, MSGS[i], strlen(MSGS[i]), 0,
                     (struct sockaddr *)&saddr, slen) == (ssize_t)strlen(MSGS[i]));

    struct sockaddr_un from;
    for (int i = 0; i < NMSGS; i++) {
        char buf[64];
        socklen_t flen = sizeof from;
        ssize_t n = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &flen);
        printf("  recvfrom #%d -> %zd bytes \"%.*s\" from %s\n", i, n, (int)n, buf,
               from.sun_path);
        CHECK(n == (ssize_t)strlen(MSGS[i]) && memcmp(buf, MSGS[i], (size_t)n) == 0);
        CHECK(strcmp(from.sun_path, caddr.sun_path) == 0);
    }
    /* Reply to whoever sent the last datagram */
    CHECK(sendto(srv, "ack", 3, 0, (struct sockaddr *)&from, sizeof from) == 3);
    char ack[8];
    CHECK(recv(cli, ack, sizeof ack, 0) == 3 && memcmp(ack, "ack", 3) == 0);
    printf("  server replied \"ack\" to the sender address\n");

    /*
     * Truncation: a datagram is delivered whole or cut. With recvmsg the
     * kernel sets MSG_TRUNC in msg_flags; the cut-off bytes are GONE.
     * Linux extra: recv(..., MSG_PEEK | MSG_TRUNC) returns the real size,
     * so you can size the buffer first.
     */
    const char big[] = "this datagram is longer than 8 bytes";
    CHECK(sendto(cli, big, sizeof big - 1, 0, (struct sockaddr *)&saddr, slen) ==
          (ssize_t)(sizeof big - 1));
    char tiny[8];
    ssize_t real = recv(srv, tiny, 0, MSG_PEEK | MSG_TRUNC);
    CHECK(real == (ssize_t)(sizeof big - 1));
    struct iovec iov = {.iov_base = tiny, .iov_len = sizeof tiny};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n = recvmsg(srv, &mh, 0);
    printf("  peek says next datagram is %zd bytes; recvmsg into 8 bytes -> %zd, "
           "MSG_TRUNC=%s\n", real, n, (mh.msg_flags & MSG_TRUNC) ? "set" : "clear");
    CHECK(n == 8 && (mh.msg_flags & MSG_TRUNC));
    /* The rest of that datagram was discarded: the queue is now empty. */
    CHECK(recv(srv, tiny, sizeof tiny, MSG_DONTWAIT) == -1 && errno == EAGAIN);

    /*
     * An unbound sender: the receiver's recvfrom() gets an empty address
     * (addrlen 0 on current kernels, sizeof(sa_family_t) on older ones):
     * nobody to reply to. (Linux "autobind" via bind(fd, addr, sizeof(sa_family_t))
     * gives a random abstract name if you need replies without a path.)
     */
    int anon = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    CHECK(anon >= 0);
    CHECK(sendto(anon, "hi", 2, 0, (struct sockaddr *)&saddr, slen) == 2);
    socklen_t flen = sizeof from;
    CHECK(recvfrom(srv, tiny, sizeof tiny, 0, (struct sockaddr *)&from, &flen) == 2);
    printf("  unbound sender: recvfrom addrlen = %u (no name) -> cannot reply\n",
           (unsigned)flen);
    CHECK(flen <= sizeof(sa_family_t));
    close(anon);

    /*
     * Reliable: when the receiver's queue is full, a blocking sender
     * BLOCKS (UDP would drop). Non-blocking, it gets EAGAIN. The queue
     * length is limited by net.unix.max_dgram_qlen and the buffer sizes.
     */
    int fl = fcntl(cli, F_GETFL);
    CHECK(fcntl(cli, F_SETFL, fl | O_NONBLOCK) == 0);
    int sent = 0;
    while (sendto(cli, "x", 1, 0, (struct sockaddr *)&saddr, slen) == 1)
        sent++;
    CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
    printf("  receiver not reading: %d datagrams queued, then EAGAIN (no silent drop)\n",
           sent);
    CHECK(sent > 0);
    int drained = 0;
    while (recv(srv, tiny, sizeof tiny, MSG_DONTWAIT) == 1)
        drained++;
    CHECK(drained == sent); /* every one arrived */

    close(cli);
    close(srv);
    CHECK(unlink(saddr.sun_path) == 0);
    CHECK(unlink(caddr.sun_path) == 0);
}

/*
 * SEQPACKET: like a phone call that delivers telegrams.
 *   connection-oriented (socketpair / listen+accept, EOF on close)
 *   + record boundaries (one send = one recv, truncation like DGRAM)
 */
static void demo_seqpacket(void)
{
    printf("3) SOCK_SEQPACKET: connection + boundaries\n");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) == 0);
    for (int i = 0; i < NMSGS; i++)
        CHECK(send(sv[0], MSGS[i], strlen(MSGS[i]), 0) == (ssize_t)strlen(MSGS[i]));
    close(sv[0]); /* writer hangs up after queuing 3 records */

    for (int i = 0; i < NMSGS; i++) {
        char buf[64];
        ssize_t n = recv(sv[1], buf, sizeof buf, 0);
        printf("  recv #%d -> \"%.*s\"\n", i, (int)n, buf);
        CHECK(n == (ssize_t)strlen(MSGS[i]) && memcmp(buf, MSGS[i], (size_t)n) == 0);
    }
    char buf[8];
    CHECK(recv(sv[1], buf, sizeof buf, 0) == 0); /* EOF: DGRAM has no such thing */
    printf("  then recv -> 0 (EOF, the peer closed)\n");
    close(sv[1]);
}

int main(void)
{
    char dir[] = "/tmp/uds-dgram-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);

    demo_stream();
    demo_dgram(dir);
    demo_seqpacket();

    CHECK(rmdir(dir) == 0); /* fails if we forgot to unlink a socket file */
    printf("02_dgram_and_seqpacket: all checks passed\n");
    return 0;
}
