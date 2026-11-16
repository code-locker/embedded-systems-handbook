/*
 * 05_peer_credentials.c — who is on the other end? SO_PEERCRED and
 *                         SCM_CREDENTIALS
 *
 * Concept : A Unix socket peer is a process on THIS machine, so the kernel
 *           can tell you its pid, uid and gid — and the kernel, not the
 *           peer, vouches for them. SO_PEERCRED: ask about a connected
 *           socket (credentials captured at connect()/listen()/socketpair()).
 *           SCM_CREDENTIALS: credentials attached to each message, enabled
 *           by SO_PASSCRED on the receiver; a sender may only claim its own
 *           ids (a fake pid gives EPERM unless it has CAP_SYS_ADMIN).
 *           This is how D-Bus, systemd-journald and the Docker daemon
 *           decide what a client may do. TCP cannot do this.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/05_peer_credentials.c
 * Read    : ../README.md  (section 3.6 "Peer credentials")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
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

static void wait_child_ok(pid_t pid)
{
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static struct ucred peer_cred(int fd)
{
    struct ucred uc;
    socklen_t len = sizeof uc;
    CHECK(getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0);
    CHECK(len == sizeof uc);
    return uc;
}

/*
 * SO_PEERCRED on a listening server:
 *
 *   client pid 4242 ---connect()---> server
 *                                     accept() -> cfd
 *                                     getsockopt(cfd, SO_PEERCRED)
 *                                       -> {pid 4242, uid 1000, gid 1000}
 *
 * The values are what the client had at connect() time, filled in by the
 * kernel. A client cannot lie about them. (Caveat: the pid may be reused
 * after the client exits; Linux >= 6.5 offers SO_PEERPIDFD for a stable
 * handle.)
 */
static void demo_so_peercred(void)
{
    printf("1) SO_PEERCRED on accept()ed and connect()ed sockets\n");
    char dir[] = "/tmp/uds-cred-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    int w = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/s", dir);
    CHECK(w > 0 && (size_t)w < sizeof addr.sun_path);

    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(srv >= 0);
    CHECK(bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(srv, 4) == 0);
    pid_t server_pid = getpid();

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(srv);
        int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHILD_CHECK(c >= 0);
        CHILD_CHECK(connect(c, (struct sockaddr *)&addr, sizeof addr) == 0);
        /* The client side sees the credentials the SERVER had at listen() */
        struct ucred uc;
        socklen_t len = sizeof uc;
        CHILD_CHECK(getsockopt(c, SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0);
        CHILD_CHECK(uc.pid == server_pid && uc.uid == getuid());
        printf("  client: my server is pid %d uid %u\n", (int)uc.pid, (unsigned)uc.uid);
        char b;
        CHILD_CHECK(read(c, &b, 1) == 0); /* wait until the server hangs up */
        close(c);
        _exit(0);
    }

    int cfd = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
    CHECK(cfd >= 0);
    struct ucred uc = peer_cred(cfd);
    printf("  server: client is pid %d uid %u gid %u (fork returned %d)\n",
           (int)uc.pid, (unsigned)uc.uid, (unsigned)uc.gid, (int)pid);
    CHECK(uc.pid == pid);
    CHECK(uc.uid == getuid() && uc.gid == getgid());
    /* A real daemon decides here: e.g. allow only uid 0 or members of a
     * group (that is exactly the Docker socket's "docker" group check,
     * done with file permissions plus peer credentials). */
    close(cfd);
    wait_child_ok(pid);
    close(srv);
    CHECK(unlink(addr.sun_path) == 0);
    CHECK(rmdir(dir) == 0);
}

/* Receive one datagram and its SCM_CREDENTIALS. Returns 0 if credentials
 * were attached. */
static int recv_with_cred(int fd, char *buf, size_t cap, struct ucred *out)
{
    union {
        char b[CMSG_SPACE(sizeof(struct ucred))];
        struct cmsghdr align;
    } ctrl;
    struct iovec iov = {.iov_base = buf, .iov_len = cap};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = ctrl.b, .msg_controllen = sizeof ctrl.b};
    if (recvmsg(fd, &mh, 0) < 0)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
            memcpy(out, CMSG_DATA(c), sizeof *out);
            return 0;
        }
    return -1;
}

/* Send a datagram that CLAIMS credentials uc. */
static ssize_t send_with_cred(int fd, const char *msg, const struct ucred *uc)
{
    union {
        char b[CMSG_SPACE(sizeof(struct ucred))];
        struct cmsghdr align;
    } ctrl;
    memset(&ctrl, 0, sizeof ctrl);
    struct iovec iov = {.iov_base = (void *)msg, .iov_len = strlen(msg)};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = ctrl.b, .msg_controllen = sizeof ctrl.b};
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_CREDENTIALS;
    c->cmsg_len = CMSG_LEN(sizeof *uc);
    memcpy(CMSG_DATA(c), uc, sizeof *uc);
    return sendmsg(fd, &mh, 0);
}

static void demo_scm_credentials(void)
{
    printf("2) SCM_CREDENTIALS: per-message credentials, checked by the kernel\n");
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) == 0);
    int on = 1;
    /* Receiver opts in. Without SO_PASSCRED no credentials are delivered. */
    CHECK(setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &on, sizeof on) == 0);
    int is_root = geteuid() == 0;

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(sv[0]);
        /* (a) no cmsg at all: the kernel attaches our real credentials */
        CHILD_CHECK(send(sv[1], "plain", 5, 0) == 5);
        /* (b) explicit, truthful credentials: accepted */
        struct ucred me = {.pid = getpid(), .uid = getuid(), .gid = getgid()};
        CHILD_CHECK(send_with_cred(sv[1], "honest", &me) == 6);
        /* (c) a lie: claim to be pid 1 (init). Unprivileged -> EPERM. */
        struct ucred fake = me;
        fake.pid = 1;
        errno = 0;
        ssize_t r = send_with_cred(sv[1], "liar", &fake);
        if (is_root && r == 4) {
            printf("  child: running as root, the fake pid was ALLOWED (CAP_SYS_ADMIN)\n");
        } else if (is_root) {
            /* root in a container usually lacks CAP_SYS_ADMIN: still EPERM */
            printf("  child: root WITHOUT CAP_SYS_ADMIN (container?) -> %s\n",
                   errno == EPERM ? "EPERM" : strerror(errno));
            CHILD_CHECK(r == -1 && errno == EPERM);
        } else {
            printf("  child: claiming pid 1 -> sendmsg failed with %s\n",
                   errno == EPERM ? "EPERM" : strerror(errno));
            CHILD_CHECK(r == -1 && errno == EPERM);
        }
        close(sv[1]);
        _exit(0);
    }
    close(sv[1]);

    char buf[16];
    struct ucred uc;
    const char *labels[] = {"plain (kernel-filled)", "honest (explicit)"};
    for (int i = 0; i < 2; i++) {
        memset(buf, 0, sizeof buf);
        CHECK(recv_with_cred(sv[0], buf, sizeof buf - 1, &uc) == 0);
        printf("  parent: \"%s\" %s -> pid %d uid %u\n", buf, labels[i], (int)uc.pid,
               (unsigned)uc.uid);
        CHECK(uc.pid == pid && uc.uid == getuid());
    }
    wait_child_ok(pid);
    /* A third message is queued only if the kernel allowed the fake pid,
     * which needs CAP_SYS_ADMIN (not just uid 0). The child has exited, so
     * a zero-timeout poll() tells us whether it is there. */
    struct pollfd pf = {.fd = sv[0], .events = POLLIN};
    if (poll(&pf, 1, 0) == 1) {
        CHECK(is_root);
        CHECK(recv_with_cred(sv[0], buf, sizeof buf - 1, &uc) == 0);
        CHECK(uc.pid == 1);
    }
    close(sv[0]);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    demo_so_peercred();
    demo_scm_credentials();
    printf("05_peer_credentials: all checks passed\n");
    return 0;
}
