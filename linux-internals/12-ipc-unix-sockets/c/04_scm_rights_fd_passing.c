/*
 * 04_scm_rights_fd_passing.c — send open file descriptors to another
 *                              process with SCM_RIGHTS
 *
 * Concept : An fd number means nothing outside its process. SCM_RIGHTS
 *           ancillary data makes the KERNEL install a new fd in the
 *           receiver that refers to the SAME open file description (same
 *           file, same offset, same flags) — like dup() across processes.
 *           The receiver needs no path and no permission to open the file:
 *           holding the fd is the capability. This is how Wayland passes
 *           buffers, how systemd hands sockets to services, and how a
 *           privileged helper gives an unprivileged process one device.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/04_scm_rights_fd_passing.c
 * Read    : ../README.md  (section 3.5 "Passing file descriptors")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> /* memfd_create */
#include <sys/socket.h>
#include <sys/stat.h>
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

#define MAX_FDS 4
static const char MEMFD_TEXT[] = "config from a memfd: rate=30fps\n";
static const char FILE_TEXT[] = "contents of an already-deleted temp file\n";
static const char CHILD_TEXT[] = "child was here\n";

/* What travels in the normal data part next to the fds: the inode numbers,
 * so the receiver can prove it got the very same files. */
struct fd_note {
    uint32_t nfds;
    uint64_t ino[MAX_FDS];
};

/*
 * sendmsg() layout for fd passing:
 *
 *   struct msghdr
 *     msg_iov ---------> [ struct fd_note ]      <- at least 1 byte of real
 *                                                   data is REQUIRED on a
 *                                                   stream socket
 *     msg_control -----> +-----------------------------------+
 *                        | struct cmsghdr                    |
 *                        |   cmsg_level = SOL_SOCKET         |
 *                        |   cmsg_type  = SCM_RIGHTS         |
 *                        |   cmsg_len   = CMSG_LEN(n*int)    |
 *                        +-----------------------------------+
 *                        | int fds[n]   (CMSG_DATA)          |
 *                        +-----------------------------------+
 * The control buffer must be suitably aligned: use a union with
 * struct cmsghdr (or alignas) and size it with CMSG_SPACE().
 */
static int send_fds(int sock, const int *fds, uint32_t n, const struct fd_note *note)
{
    union {
        char buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
        struct cmsghdr align;
    } ctrl;
    memset(&ctrl, 0, sizeof ctrl);

    struct iovec iov = {.iov_base = (void *)note, .iov_len = sizeof *note};
    struct msghdr mh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl.buf,
        .msg_controllen = CMSG_SPACE(sizeof(int) * n),
    };
    struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * n);
    memcpy(CMSG_DATA(c), fds, sizeof(int) * n); /* memcpy: CMSG_DATA may be unaligned */
    return sendmsg(sock, &mh, 0) == (ssize_t)sizeof *note ? 0 : -1;
}

/* Returns the number of fds received (written into fds[]), -1 on error. */
static int recv_fds(int sock, int *fds, struct fd_note *note)
{
    union {
        char buf[CMSG_SPACE(sizeof(int) * MAX_FDS)];
        struct cmsghdr align;
    } ctrl;
    struct iovec iov = {.iov_base = note, .iov_len = sizeof *note};
    struct msghdr mh = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = ctrl.buf,
        .msg_controllen = sizeof ctrl.buf,
    };
    /* MSG_CMSG_CLOEXEC: the new fds get FD_CLOEXEC atomically, so they do
     * not leak into programs we might exec later. */
    ssize_t r = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    if (r != (ssize_t)sizeof *note)
        return -1;
    /* MSG_CTRUNC: our control buffer was too small. The kernel CLOSED the
     * fds that did not fit, so this is an error you must handle. */
    if (mh.msg_flags & MSG_CTRUNC)
        return -1;
    int n = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds + n, CMSG_DATA(c), sizeof(int) * (size_t)k);
            n += k;
        }
    }
    return n;
}

static uint64_t inode_of(int fd)
{
    struct stat sb;
    return fstat(fd, &sb) == 0 ? (uint64_t)sb.st_ino : 0;
}

/* Child: receives the fds, reads them WITHOUT knowing any path, appends
 * to the memfd. */
static void child(int sock)
{
    alarm(5);
    int fds[MAX_FDS];
    struct fd_note note;
    int n = recv_fds(sock, fds, &note);
    CHILD_CHECK(n == 2 && note.nfds == 2);
    printf("  child: received fds %d and %d (numbers picked in the CHILD's fd table;"
           " same files)\n", fds[0], fds[1]);
    for (int i = 0; i < n; i++)
        CHILD_CHECK(inode_of(fds[i]) == note.ino[i]);
    CHILD_CHECK(fcntl(fds[0], F_GETFD) & FD_CLOEXEC); /* MSG_CMSG_CLOEXEC */

    char buf[128];
    ssize_t r = pread(fds[0], buf, sizeof buf, 0);
    CHILD_CHECK(r == (ssize_t)strlen(MEMFD_TEXT) && memcmp(buf, MEMFD_TEXT, (size_t)r) == 0);
    printf("  child: memfd says   \"%.*s\"\n", (int)r - 1, buf);
    r = pread(fds[1], buf, sizeof buf, 0);
    CHILD_CHECK(r == (ssize_t)strlen(FILE_TEXT) && memcmp(buf, FILE_TEXT, (size_t)r) == 0);
    printf("  child: temp file says \"%.*s\"\n", (int)r - 1, buf);

    /* write() (not pwrite) uses and moves the SHARED file offset. The
     * parent's offset is at the end of MEMFD_TEXT; we append there. */
    CHILD_CHECK(write(fds[0], CHILD_TEXT, strlen(CHILD_TEXT)) ==
                (ssize_t)strlen(CHILD_TEXT));
    for (int i = 0; i < n; i++)
        close(fds[i]);
    CHILD_CHECK(write(sock, "k", 1) == 1); /* "done" */
    close(sock);
    _exit(0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("passing two fds from parent to child over a socketpair\n");

    /* fd #1: an anonymous in-memory file. It has no path anywhere, so
     * passing the fd is the ONLY way another process can reach it. */
    int mfd = memfd_create("demo-config", MFD_CLOEXEC);
    CHECK(mfd >= 0);
    CHECK(write(mfd, MEMFD_TEXT, strlen(MEMFD_TEXT)) == (ssize_t)strlen(MEMFD_TEXT));

    /* fd #2: a temp file, unlinked right away. The name is gone; the open
     * file lives on as long as an fd refers to it. */
    char path[] = "/tmp/uds-fdpass-XXXXXX";
    int tfd = mkstemp(path);
    CHECK(tfd >= 0);
    CHECK(unlink(path) == 0);
    CHECK(write(tfd, FILE_TEXT, strlen(FILE_TEXT)) == (ssize_t)strlen(FILE_TEXT));

    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    pid_t pid = fork(); /* parent is single-threaded: fork is safe */
    CHECK(pid >= 0);
    if (pid == 0) {
        close(sv[0]);
        close(mfd); /* prove the child did not just inherit them */
        close(tfd);
        /* Occupy the lowest free slot so the received numbers visibly
         * differ from the parent's: fd numbers are per-process. */
        int devnull = open("/dev/null", O_RDONLY | O_CLOEXEC);
        CHILD_CHECK(devnull >= 0);
        child(sv[1]);
    }
    close(sv[1]);

    int fds[2] = {mfd, tfd};
    struct fd_note note = {.nfds = 2, .ino = {inode_of(mfd), inode_of(tfd)}};
    printf("  parent: sending fds %d (memfd) and %d (temp file)\n", mfd, tfd);
    CHECK(send_fds(sv[0], fds, 2, &note) == 0);
    /* The fds are "in flight" inside the socket buffer now. We may close
     * ours immediately: the in-flight reference keeps the files alive.
     * We keep mfd to look at the child's write. */
    close(tfd);

    char done;
    CHECK(read(sv[0], &done, 1) == 1 && done == 'k');

    /* The child's write landed at the shared offset, right after our text */
    off_t off = lseek(mfd, 0, SEEK_CUR);
    printf("  parent: memfd offset is now %lld (moved by the CHILD's write)\n",
           (long long)off);
    CHECK(off == (off_t)(strlen(MEMFD_TEXT) + strlen(CHILD_TEXT)));
    char buf[128];
    ssize_t r = pread(mfd, buf, sizeof buf, 0);
    CHECK(r == off);
    CHECK(memcmp(buf + strlen(MEMFD_TEXT), CHILD_TEXT, strlen(CHILD_TEXT)) == 0);
    printf("  parent: memfd now holds the child's line too\n");

    close(mfd);
    close(sv[0]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("04_scm_rights_fd_passing: all checks passed\n");
    return 0;
}
