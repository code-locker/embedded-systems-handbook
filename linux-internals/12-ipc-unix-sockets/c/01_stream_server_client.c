/*
 * 01_stream_server_client.c — AF_UNIX SOCK_STREAM server and client:
 *                             filesystem path vs abstract namespace
 *
 * Concept : A Unix domain socket is the BSD socket API (socket, bind, listen,
 *           accept, connect) with a *file name* instead of an IP:port. The
 *           kernel copies bytes straight from one socket buffer to the other:
 *           no TCP, no IP, no checksums, no network device. A filesystem
 *           name is a real inode that YOU must unlink; an abstract name
 *           (leading NUL byte, Linux only) lives only as long as the socket.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/01_stream_server_client.c
 * Read    : ../README.md  (sections 3.1 "Server and client" and 3.2 "Names")
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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

/* In a forked child: report and _exit(1) so the parent's waitpid sees it.
 * Children never call exit(): that would run the parent's atexit handlers
 * and flush a copy of the parent's stdio buffers a second time. */
#define CHILD_CHECK(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "child CHECK failed: %s  (%s:%d) errno=%d\n",    \
                    #cond, __FILE__, __LINE__, errno);                       \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

static const char REQUEST[] = "hello over a unix socket";

/* Read until EOF (peer closed or shut down its write side). Returns bytes
 * read. A stream socket has no message boundaries, so "until EOF" is one of
 * the two ways to know a message is complete (the other is framing, 03). */
static ssize_t read_until_eof(int fd, char *buf, size_t cap)
{
    size_t got = 0;
    while (got < cap) {
        ssize_t n = read(fd, buf + got, cap - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        if (n == 0)
            break; /* EOF */
        got += (size_t)n;
    }
    return (ssize_t)got;
}

static int write_all(int fd, const void *p, size_t len)
{
    const char *c = p;
    while (len > 0) {
        ssize_t n = write(fd, c, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return -1;
        c += n;
        len -= (size_t)n;
    }
    return 0;
}

/*
 * The client: socket -> connect -> write -> shutdown(WR) -> read reply.
 *
 *   client                         server
 *   ------                         ------
 *   socket()                       socket()
 *                                  bind("/tmp/.../server.sock")
 *                                  listen(backlog)
 *   connect(path)  ---- SYN-less handshake: the kernel just links the
 *                       two sockets and queues the new one on the
 *                       listener's backlog ---->      accept() -> cfd
 *   write(req)     -------- bytes ---------->         read(cfd)
 *   shutdown(WR)   -------- EOF  ----------->         read() == 0
 *   read(reply)    <------- bytes ----------          write(cfd)
 *   close()                                           close(cfd)
 */
static void run_client(const struct sockaddr_un *addr, socklen_t len)
{
    alarm(5); /* watchdog: SIGALRM's default action kills a hung child */
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHILD_CHECK(c >= 0);
    CHILD_CHECK(connect(c, (const struct sockaddr *)addr, len) == 0);
    CHILD_CHECK(write_all(c, REQUEST, strlen(REQUEST)) == 0);
    /* Half-close: "I am done sending", but I can still receive. */
    CHILD_CHECK(shutdown(c, SHUT_WR) == 0);
    char reply[128];
    ssize_t n = read_until_eof(c, reply, sizeof reply - 1);
    CHILD_CHECK(n == (ssize_t)strlen(REQUEST));
    reply[n] = '\0';
    for (size_t i = 0; i < strlen(REQUEST); i++)
        CHILD_CHECK(reply[i] == toupper((unsigned char)REQUEST[i]));
    close(c);
    _exit(0);
}

/* Server side of one connection: read request until EOF, upper-case it,
 * send it back, close. */
static void serve_one(int srv)
{
    int cfd = accept4(srv, NULL, NULL, SOCK_CLOEXEC);
    CHECK(cfd >= 0);
    char buf[128];
    ssize_t n = read_until_eof(cfd, buf, sizeof buf - 1);
    CHECK(n == (ssize_t)strlen(REQUEST));
    buf[n] = '\0';
    printf("  server got  : \"%s\"\n", buf);
    for (ssize_t i = 0; i < n; i++)
        buf[i] = (char)toupper((unsigned char)buf[i]);
    CHECK(write_all(cfd, buf, (size_t)n) == 0);
    printf("  server sent : \"%s\"\n", buf);
    close(cfd);
}

static void wait_child_ok(pid_t pid)
{
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static void demo_filesystem_name(void)
{
    printf("1) filesystem name\n");
    /* Unique per run (pid) and private (mkdtemp gives mode 0700): nobody
     * else can connect, and parallel runs never collide. */
    char dir[] = "/tmp/uds-demo-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    /* sun_path is only 108 bytes on Linux: check, never truncate silently */
    int w = snprintf(addr.sun_path, sizeof addr.sun_path, "%s/server.sock", dir);
    CHECK(w > 0 && (size_t)w < sizeof addr.sun_path);
    printf("  sizeof(sun_path) = %zu bytes; path = %s\n",
           sizeof addr.sun_path, addr.sun_path);

    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(srv >= 0);
    CHECK(bind(srv, (struct sockaddr *)&addr, sizeof addr) == 0);
    CHECK(listen(srv, 8) == 0);

    /* bind() created an inode of type "socket" (ls -l shows 's') */
    struct stat sb;
    CHECK(stat(addr.sun_path, &sb) == 0);
    CHECK(S_ISSOCK(sb.st_mode));
    printf("  bind created a socket inode (S_ISSOCK), mode %o\n",
           (unsigned)(sb.st_mode & 0777));

    pid_t pid = fork(); /* single-threaded here, so fork is safe */
    CHECK(pid >= 0);
    if (pid == 0) {
        close(srv);
        run_client(&addr, sizeof addr);
    }
    serve_one(srv);
    wait_child_ok(pid);

    /* Pitfall: closing the socket does NOT remove the file. A restarted
     * server then fails with EADDRINUSE. That is why daemons unlink()
     * before bind() (or systemd hands them an already-bound socket). */
    close(srv);
    CHECK(access(addr.sun_path, F_OK) == 0);
    int srv2 = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(srv2 >= 0);
    errno = 0;
    CHECK(bind(srv2, (struct sockaddr *)&addr, sizeof addr) == -1 &&
          errno == EADDRINUSE);
    printf("  after close(): file still there, re-bind -> EADDRINUSE\n");
    CHECK(unlink(addr.sun_path) == 0);
    CHECK(bind(srv2, (struct sockaddr *)&addr, sizeof addr) == 0);
    printf("  after unlink(): re-bind OK\n");
    close(srv2);

    /* Cleanup: always unlink the socket file, then the private dir */
    CHECK(unlink(addr.sun_path) == 0);
    CHECK(rmdir(dir) == 0);
}

/* Is "@name" listed in /proc/net/unix? (abstract names show with '@') */
static int proc_net_unix_has(const char *name)
{
    FILE *f = fopen("/proc/net/unix", "r");
    if (!f)
        return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, name))
            found = 1;
    fclose(f);
    return found;
}

static void demo_abstract_name(void)
{
    printf("2) abstract namespace (Linux only)\n");
    /*
     * sun_path:  [ \0 | u | d | s | - | d | e | m | o | - | 1 | 2 | 3 ]
     *              ^ leading NUL = "abstract": no file is created
     * The name is exactly addrlen - offsetof(sun_path) bytes long; it is
     * NOT NUL-terminated, and trailing bytes count. So pass the exact
     * length, not sizeof(addr), or "x" and "x\0\0\0..." would differ.
     */
    char name[64];
    int nl = snprintf(name, sizeof name, "uds-demo-%d", (int)getpid());
    CHECK(nl > 0 && (size_t)nl < sizeof name);

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, name, (size_t)nl);
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + (size_t)nl);

    int srv = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(srv >= 0);
    CHECK(bind(srv, (struct sockaddr *)&addr, len) == 0);
    CHECK(listen(srv, 8) == 0);

    char at_name[80];
    snprintf(at_name, sizeof at_name, "@%s", name);
    int listed = proc_net_unix_has(at_name);
    printf("  bound \"\\0%s\"; /proc/net/unix lists %s: %s\n", name, at_name,
           listed == 1 ? "yes" : listed == 0 ? "no" : "(no /proc)");
    if (listed >= 0)
        CHECK(listed == 1);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(srv);
        run_client(&addr, len);
    }
    serve_one(srv);
    wait_child_ok(pid);

    /* The name dies with the socket: nothing to unlink, no stale file. */
    close(srv);
    int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(c >= 0);
    errno = 0;
    CHECK(connect(c, (struct sockaddr *)&addr, len) == -1 && errno == ECONNREFUSED);
    close(c);
    printf("  after close(): connect -> ECONNREFUSED, name is gone, no cleanup\n");
}

int main(void)
{
    /* The demo writes to sockets whose peer might die on a failed check:
     * get EPIPE as an error instead of being killed by SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0); /* no duplicated buffers across fork */

    demo_filesystem_name();
    demo_abstract_name();

    printf("01_stream_server_client: all checks passed\n");
    return 0;
}
