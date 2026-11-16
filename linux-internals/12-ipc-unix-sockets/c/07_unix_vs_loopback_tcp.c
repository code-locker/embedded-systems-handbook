/*
 * 07_unix_vs_loopback_tcp.c — the same ping-pong code over AF_UNIX and over
 *                             TCP on 127.0.0.1, only the address changes
 *
 * Concept : The socket API is family-agnostic: socket/bind/listen/accept/
 *           connect/read/write take a generic struct sockaddr. Switch
 *           AF_UNIX + sockaddr_un for AF_INET + sockaddr_in and the rest of
 *           the code is identical. What changes is the work the kernel does:
 *           TCP on loopback still runs the whole TCP/IP stack (segments,
 *           ACKs, checksum offload paths, the lo device); AF_UNIX just moves
 *           a buffer from one socket to the other. Round-trip times are
 *           PRINTED (machine-dependent), never asserted. TCP also has no
 *           SO_PEERCRED and no SCM_RIGHTS. No external network is touched:
 *           127.0.0.1 with an ephemeral port chosen by the kernel.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/07_unix_vs_loopback_tcp.c
 * Read    : ../README.md  (section 3.8 "Unix vs loopback TCP")
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
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

#define CHILD_CHECK(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "child CHECK failed: %s  (%s:%d) errno=%d\n",    \
                    #cond, __FILE__, __LINE__, errno);                       \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define ROUNDS 2000
#define MSG 64

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

/* Generic address: big enough for any family (that is what
 * struct sockaddr_storage is for). */
struct endpoint {
    const char *name;
    struct sockaddr_storage ss;
    socklen_t len;
};

/*
 * EVERYTHING below this line is family-agnostic. Only the code that fills
 * `struct endpoint` (make_unix / make_tcp) knows about AF_UNIX or AF_INET.
 *
 *          AF_UNIX                               AF_INET (loopback)
 *   write()                                write()
 *     -> copy into peer's receive queue      -> tcp_sendmsg -> segment
 *   read()                                   -> ip_output -> lo device
 *                                            -> ip_rcv -> tcp_v4_rcv
 *                                            -> receive queue, ACK back
 *                                          read()
 */
static double ping_pong(struct endpoint *ep)
{
    int family = ep->ss.ss_family;
    int lfd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0)
        return -1;
    if (bind(lfd, (struct sockaddr *)&ep->ss, ep->len) < 0 || listen(lfd, 1) < 0) {
        close(lfd);
        return -1;
    }
    /* TCP: port 0 asked the kernel for a free port; read it back. */
    ep->len = sizeof ep->ss;
    CHECK(getsockname(lfd, (struct sockaddr *)&ep->ss, &ep->len) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { /* echo server */
        alarm(10);
        int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        CHILD_CHECK(c >= 0);
        char buf[MSG];
        for (int i = 0; i < ROUNDS; i++) {
            CHILD_CHECK(read_full(c, buf, MSG) == 0);
            CHILD_CHECK(write(c, buf, MSG) == MSG);
        }
        close(c);
        _exit(0);
    }
    close(lfd);

    int fd = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    CHECK(connect(fd, (struct sockaddr *)&ep->ss, ep->len) == 0);
    if (family == AF_INET) {
        /* Disable Nagle: otherwise small writes may be held back waiting
         * for an ACK, and a ping-pong measures delays, not the stack. */
        int one = 1;
        CHECK(setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) == 0);
    }

    /* Peer credentials: a Unix-only feature. On the connecting side they
     * describe whoever called listen() — that was us, before the fork. */
    struct ucred uc = {0};
    socklen_t ul = sizeof uc;
    int r = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &ul);
    printf("  %-14s SO_PEERCRED -> %s\n", ep->name,
           (r == 0 && uc.pid > 0) ? "peer pid known" : "no peer process info");
    if (family == AF_UNIX)
        CHECK(r == 0 && uc.pid == getpid());
    else
        CHECK(r == -1 || uc.pid <= 0); /* TCP has no process identity */

    char out[MSG], in[MSG];
    double t0 = now_us();
    for (int i = 0; i < ROUNDS; i++) {
        memset(out, 'a' + i % 26, MSG);
        CHECK(write(fd, out, MSG) == MSG);
        CHECK(read_full(fd, in, MSG) == 0);
        CHECK(memcmp(in, out, MSG) == 0);
    }
    double dt = now_us() - t0;
    close(fd);

    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    return dt / ROUNDS;
}

static void make_unix(struct endpoint *ep)
{
    /* abstract name: no file, no cleanup, unique per run */
    struct sockaddr_un *a = (struct sockaddr_un *)&ep->ss;
    memset(&ep->ss, 0, sizeof ep->ss);
    a->sun_family = AF_UNIX;
    int n = snprintf(a->sun_path + 1, sizeof a->sun_path - 1, "uds-vs-tcp-%d", (int)getpid());
    CHECK(n > 0 && (size_t)n < sizeof a->sun_path - 1);
    ep->len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)n);
    ep->name = "AF_UNIX";
}

static void make_tcp(struct endpoint *ep)
{
    struct sockaddr_in *a = (struct sockaddr_in *)&ep->ss;
    memset(&ep->ss, 0, sizeof ep->ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(0); /* any free port */
    a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ep->len = sizeof *a;
    ep->name = "TCP 127.0.0.1";
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("%d round trips of %d bytes, same code, two address families\n", ROUNDS, MSG);

    struct endpoint u, t;
    make_unix(&u);
    make_tcp(&t);
    double us_unix = ping_pong(&u);
    CHECK(us_unix > 0);
    double us_tcp = ping_pong(&t);

    printf("  %-14s %.1f us per round trip\n", u.name, us_unix);
    if (us_tcp < 0)
        printf("  %-14s skipped: no loopback interface here (errno %d)\n", t.name, errno);
    else
        printf("  %-14s %.1f us per round trip  (ratio %.2fx, timing-dependent)\n", t.name,
               us_tcp, us_tcp / us_unix);

    printf("07_unix_vs_loopback_tcp: all checks passed\n");
    return 0;
}
