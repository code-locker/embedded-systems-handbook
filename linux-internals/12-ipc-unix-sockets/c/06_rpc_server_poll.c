/*
 * 06_rpc_server_poll.c — a tiny request/response (RPC) server that serves
 *                        several clients at once with poll()
 *
 * Concept : One thread, one poll() loop, many connections. The server never
 *           blocks on any single client: it waits until the kernel says "fd
 *           X is readable", does ONE read(), appends the bytes to that
 *           client's buffer and executes every COMPLETE frame in it. Bytes
 *           of a half-arrived frame simply wait in the buffer for the next
 *           poll() round. This per-connection reassembly buffer is the core
 *           of every event-driven server (nginx, Redis, D-Bus daemon, ...).
 *           Clients are forked processes, each with its own requests.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/06_rpc_server_poll.c
 * Read    : ../README.md  (section 3.7 "A request/response server")
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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

#define N_CLIENTS 4
#define N_REQ 25
#define MAX_PAYLOAD 256

/* --------------------------- framing helpers ---------------------------- */
static int write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
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

/* frame = [u32 big-endian length][text payload], e.g. "ADD 3 4" -> "OK 7" */
static int send_text(int fd, const char *s)
{
    uint8_t out[4 + MAX_PAYLOAD];
    size_t len = strlen(s);
    if (len > MAX_PAYLOAD)
        return -1;
    uint32_t be = htonl((uint32_t)len);
    memcpy(out, &be, 4);
    memcpy(out + 4, s, len);
    return write_full(fd, out, 4 + len); /* one write: header + payload */
}

static int recv_text(int fd, char *s, size_t cap)
{
    uint32_t be;
    if (read_full(fd, &be, 4) < 0)
        return -1;
    uint32_t len = ntohl(be);
    if (len >= cap)
        return -1;
    if (read_full(fd, s, len) < 0)
        return -1;
    s[len] = '\0';
    return 0;
}

/* ------------------------------ the "RPC" ------------------------------- */
/* Execute one request. Pure function of the text: deterministic replies. */
static void handle_request(const char *req, char *reply, size_t cap)
{
    long a, b;
    if (sscanf(req, "ADD %ld %ld", &a, &b) == 2)
        snprintf(reply, cap, "OK %ld", a + b);
    else if (sscanf(req, "MUL %ld %ld", &a, &b) == 2)
        snprintf(reply, cap, "OK %ld", a * b);
    else if (strcmp(req, "PING") == 0)
        snprintf(reply, cap, "PONG");
    else
        snprintf(reply, cap, "ERR unknown request");
}

/* ------------------------------ client ---------------------------------- */
static void client(const struct sockaddr_un *addr, int id)
{
    alarm(5); /* watchdog */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHILD_CHECK(fd >= 0);
    CHILD_CHECK(connect(fd, (const struct sockaddr *)addr, sizeof *addr) == 0);
    char req[64], rep[MAX_PAYLOAD + 1], want[64];
    for (int i = 0; i < N_REQ; i++) {
        long a = id * 100 + i, b = i - 7;
        switch (i % 4) {
        case 0: snprintf(req, sizeof req, "ADD %ld %ld", a, b); snprintf(want, sizeof want, "OK %ld", a + b); break;
        case 1: snprintf(req, sizeof req, "MUL %ld %ld", a, b); snprintf(want, sizeof want, "OK %ld", a * b); break;
        case 2: snprintf(req, sizeof req, "PING");              snprintf(want, sizeof want, "PONG"); break;
        default: snprintf(req, sizeof req, "DIV %ld %ld", a, b); snprintf(want, sizeof want, "ERR unknown request"); break;
        }
        if (i % 5 == 4) {
            /* Split a frame across two writes on purpose: the server must
             * reassemble it from two separate read()s. */
            uint32_t be = htonl((uint32_t)strlen(req));
            CHILD_CHECK(write_full(fd, &be, 2) == 0);
            CHILD_CHECK(write_full(fd, (uint8_t *)&be + 2, 2) == 0);
            CHILD_CHECK(write_full(fd, req, strlen(req)) == 0);
        } else {
            CHILD_CHECK(send_text(fd, req) == 0);
        }
        CHILD_CHECK(recv_text(fd, rep, sizeof rep) == 0);
        if (strcmp(rep, want) != 0) {
            fprintf(stderr, "client %d: \"%s\" -> \"%s\", want \"%s\"\n", id, req, rep, want);
            _exit(1);
        }
    }
    close(fd); /* server sees EOF and drops us */
    _exit(0);
}

/* ------------------------------ server ---------------------------------- */
struct conn {
    int fd;
    size_t used;               /* bytes waiting in buf */
    uint8_t buf[4 + MAX_PAYLOAD + 64];
    int served;
};

/* Run every complete frame in c->buf. Returns -1 on protocol error. */
static int drain_frames(struct conn *c)
{
    for (;;) {
        if (c->used < 4)
            return 0; /* header incomplete: wait for more bytes */
        uint32_t be;
        memcpy(&be, c->buf, 4);
        uint32_t len = ntohl(be);
        if (len > MAX_PAYLOAD)
            return -1;
        if (c->used < 4 + len)
            return 0; /* payload incomplete */
        char req[MAX_PAYLOAD + 1], rep[MAX_PAYLOAD];
        memcpy(req, c->buf + 4, len);
        req[len] = '\0';
        handle_request(req, rep, sizeof rep);
        if (send_text(c->fd, rep) < 0)
            return -1;
        c->served++;
        /* Shift the remaining bytes to the front. O(bytes left); fine for
         * small buffers. Big servers use a ring buffer instead. */
        memmove(c->buf, c->buf + 4 + len, c->used - 4 - len);
        c->used -= 4 + len;
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);

    char dir[] = "/tmp/uds-rpc-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    int w = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/rpc.sock", dir);
    CHECK(w > 0 && (size_t)w < sizeof addr.sun_path);

    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(lfd >= 0);
    CHECK(bind(lfd, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(lfd, N_CLIENTS) == 0); /* listen BEFORE forking clients */

    pid_t pids[N_CLIENTS];
    for (int i = 0; i < N_CLIENTS; i++) {
        pids[i] = fork();
        CHECK(pids[i] >= 0);
        if (pids[i] == 0) {
            close(lfd);
            client(&addr, i + 1);
        }
    }
    printf("server: %d clients forked, %d requests each, one poll() loop\n",
           N_CLIENTS, N_REQ);

    /*
     * pollfd array:  [0] = listening socket (POLLIN = a client is waiting
     *                      in the accept queue)
     *                [1..N] = one connection each (POLLIN = bytes or EOF)
     *
     *   loop:  poll(all) -> for each ready fd:
     *            listener -> accept, add a slot
     *            conn     -> read once, append, drain complete frames;
     *                        read()==0 -> EOF -> close, remove slot
     */
    struct pollfd pfd[1 + N_CLIENTS];
    struct conn *conns = calloc(N_CLIENTS, sizeof *conns); /* freed below */
    CHECK(conns != NULL);
    int nconn = 0, accepted = 0, closed = 0, total = 0, rounds = 0;

    while (closed < N_CLIENTS) {
        int n = 0;
        pfd[n++] = (struct pollfd){.fd = accepted < N_CLIENTS ? lfd : -1, .events = POLLIN};
        for (int i = 0; i < nconn; i++)
            pfd[n++] = (struct pollfd){.fd = conns[i].fd, .events = POLLIN};

        int r = poll(pfd, (nfds_t)n, 3000); /* timeout: never hang forever */
        CHECK(r > 0);
        rounds++;

        if (pfd[0].revents & POLLIN) {
            int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
            CHECK(cfd >= 0);
            conns[nconn++] = (struct conn){.fd = cfd};
            accepted++;
        }
        /* Walk backwards so removing slot i does not skip anybody. The
         * pfd[] index for conns[i] is i + 1 (conns added this round have
         * no pfd entry yet: guard with i < n - 1). */
        for (int i = nconn - 1; i >= 0; i--) {
            if (i + 1 >= n || !(pfd[i + 1].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            struct conn *c = &conns[i];
            ssize_t got = read(c->fd, c->buf + c->used, sizeof c->buf - c->used);
            if (got > 0) {
                c->used += (size_t)got;
                CHECK(drain_frames(c) == 0);
                continue;
            }
            CHECK(got == 0); /* EOF: client finished */
            CHECK(c->used == 0); /* no half frame left behind */
            total += c->served;
            close(c->fd);
            conns[i] = conns[--nconn]; /* O(1) unordered removal */
            closed++;
        }
    }
    printf("server: %d poll() rounds, %d requests served for %d clients\n",
           rounds, total, accepted);
    CHECK(total == N_CLIENTS * N_REQ);
    free(conns);

    for (int i = 0; i < N_CLIENTS; i++) {
        int st;
        CHECK(waitpid(pids[i], &st, 0) == pids[i]);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    printf("server: every client verified all %d of its replies\n", N_REQ);

    close(lfd);
    CHECK(unlink(addr.sun_path) == 0);
    CHECK(rmdir(dir) == 0);
    printf("06_rpc_server_poll: all checks passed\n");
    return 0;
}
