/*
 * 03_socketpair_framing.c — socketpair() between parent and child, and a
 *                           length-prefix protocol over a byte stream
 *
 * Concept : socketpair() = a pipe that is (a) bidirectional and (b) a real
 *           socket (fd passing, credentials, SEQPACKET...). Over SOCK_STREAM
 *           you must add your own message boundaries ("framing"): here a
 *           4-byte big-endian length, then the payload. read() and write()
 *           may move FEWER bytes than asked, so both sides loop:
 *           read_full() / write_full(). The program proves short reads and
 *           short writes really happen.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/03_socketpair_framing.c
 * Read    : ../README.md  (section 3.4 "Framing a stream")
 */
#define _GNU_SOURCE
#include <arpa/inet.h> /* htonl / ntohl */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

#define CHILD_CHECK(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "child CHECK failed: %s  (%s:%d) errno=%d\n",    \
                    #cond, __FILE__, __LINE__, errno);                       \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define MAX_FRAME (1u << 20) /* refuse absurd lengths: a corrupt or hostile
                                peer must not make us malloc 4 GiB */
#define N_MSGS 40
#define BIG_LEN (512u * 1024u) /* bigger than the default socket buffer */

/* Counts read() calls inside read_full: proof that short reads happen. */
static unsigned g_read_calls;

/*
 * write_full: keep writing until every byte is out.
 *   n < len  : a SHORT write (non-blocking socket with a full buffer, or a
 *              signal arrived mid-way). Not an error: advance and retry.
 *   EINTR    : interrupted before anything was written: retry.
 * Returns 0, or -1 on a real error (EPIPE = peer gone, ...).
 */
static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * read_full: keep reading until exactly len bytes arrived.
 *   returns 1 = got all, 0 = clean EOF before the first byte,
 *          -1 = error or EOF in the MIDDLE of a frame (truncated message).
 */
static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        g_read_calls++;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return got == 0 ? 0 : -1;
        got += (size_t)n;
    }
    return 1;
}

/*
 * Wire format of one frame:
 *
 *   +--------+--------+--------+--------+---------------------------+
 *   |   length (uint32, BIG-endian)     |  payload: length bytes    |
 *   +--------+--------+--------+--------+---------------------------+
 *
 * Fixed-size header first, so the reader always knows how much to read
 * next: read_full(4) -> len -> read_full(len). Network byte order keeps
 * the format the same if this ever crosses to a big-endian CPU or TCP.
 */
static int send_frame(int fd, const void *payload, uint32_t len)
{
    uint32_t be = htonl(len);
    /* Two write()s: fine for correctness. For performance use writev() so
     * header and payload leave in one syscall (and one skb). */
    if (write_full(fd, &be, sizeof be) < 0)
        return -1;
    return write_full(fd, payload, len);
}

/* Returns payload length, -1 on EOF (no more frames), -2 on error.
 * *out is malloc'ed: the CALLER frees it. */
static int64_t recv_frame(int fd, uint8_t **out)
{
    uint32_t be;
    int r = read_full(fd, &be, sizeof be);
    if (r == 0)
        return -1;
    if (r < 0)
        return -2;
    uint32_t len = ntohl(be);
    if (len > MAX_FRAME)
        return -2; /* protocol error: never trust a length off the wire */
    uint8_t *buf = malloc(len ? len : 1);
    if (!buf)
        return -2;
    if (len && read_full(fd, buf, len) != 1) {
        free(buf);
        return -2;
    }
    *out = buf;
    return len;
}

/* Message i has a deterministic size and content so the reader can verify
 * every byte. Message 7 is the big one. */
static uint32_t msg_len(int i) { return i == 7 ? BIG_LEN : (uint32_t)(i * 37 % 300); }
static uint8_t msg_byte(int i, uint32_t k) { return (uint8_t)(i * 31 + k); }

/* Child: sends N_MSGS frames, some of them byte by byte ("trickle") to
 * prove that the reader does not care how the bytes were chunked. */
static void child_sender(int fd)
{
    alarm(5);
    static uint8_t buf[BIG_LEN];
    for (int i = 0; i < N_MSGS; i++) {
        uint32_t len = msg_len(i);
        for (uint32_t k = 0; k < len; k++)
            buf[k] = msg_byte(i, k);
        if (i % 10 == 3) {
            /* Trickle: header and payload one byte per write(). On the
             * wire this is indistinguishable from a single write. */
            uint32_t be = htonl(len);
            for (size_t k = 0; k < sizeof be; k++)
                CHILD_CHECK(write_full(fd, (uint8_t *)&be + k, 1) == 0);
            for (uint32_t k = 0; k < len; k++)
                CHILD_CHECK(write_full(fd, buf + k, 1) == 0);
        } else {
            CHILD_CHECK(send_frame(fd, buf, len) == 0);
        }
    }
    /* Wait for the parent's ACK frame: the socket is bidirectional. */
    uint8_t *ack = NULL;
    int64_t n = recv_frame(fd, &ack);
    CHILD_CHECK(n > 0);
    printf("  child got ACK frame: \"%.*s\"\n", (int)n, (char *)ack);
    CHILD_CHECK(n == 6 && memcmp(ack, "got 40", 6) == 0);
    free(ack);
    close(fd); /* parent will then see EOF */
    _exit(0);
}

static void demo_framing(void)
{
    printf("1) length-prefixed frames over socketpair(SOCK_STREAM), parent <-> child\n");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]); /* each side closes the end it does not use */
        child_sender(sv[1]);
    }
    close(sv[1]);

    int count = 0;
    uint64_t bytes = 0;
    unsigned big_reads = 0;
    for (;;) {
        unsigned before = g_read_calls;
        uint8_t *p = NULL;
        int64_t n = recv_frame(sv[0], &p);
        if (n == -1)
            break; /* EOF: child closed */
        CHECK(n >= 0);
        CHECK((uint32_t)n == msg_len(count));
        for (uint32_t k = 0; k < (uint32_t)n; k++)
            CHECK(p[k] == msg_byte(count, k));
        if (count == 7)
            big_reads = g_read_calls - before;
        free(p);
        bytes += (uint64_t)n;
        if (++count == N_MSGS)
            CHECK(send_frame(sv[0], "got 40", 6) == 0); /* ACK, then wait for EOF */
    }
    printf("  parent received %d frames, %llu payload bytes, every byte verified\n",
           count, (unsigned long long)bytes);
    printf("  the 512 KiB frame needed %u read() calls (short reads; timing-dependent)\n",
           big_reads);
    CHECK(count == N_MSGS);
    close(sv[0]);

    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* A short WRITE made visible: on a non-blocking socket whose buffer fills
 * up, write() returns how much it took (< len), and the next write gives
 * EAGAIN. A blocking socket would instead sleep until the peer reads. */
static void demo_short_write(void)
{
    printf("2) short write on a non-blocking socket\n");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) == 0);
    int sndbuf = 0;
    socklen_t sl = sizeof sndbuf;
    CHECK(getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sndbuf, &sl) == 0);

    size_t len = 8u << 20; /* 8 MiB: far more than any default buffer */
    char *big = calloc(1, len);
    CHECK(big != NULL);
    ssize_t n = write(sv[0], big, len);
    printf("  SO_SNDBUF = %d; write(8 MiB) returned %zd -> short write\n", sndbuf, n);
    CHECK(n > 0 && (size_t)n < len);
    errno = 0;
    CHECK(write(sv[0], big, len) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    printf("  next write -> EAGAIN: caller must wait for POLLOUT and resume at offset %zd\n",
           n);
    free(big);
    close(sv[0]);
    close(sv[1]);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* EPIPE instead of death if the peer dies */
    setvbuf(stdout, NULL, _IONBF, 0);

    demo_framing();
    demo_short_write();

    printf("03_socketpair_framing: all checks passed\n");
    return 0;
}
