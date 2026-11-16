/*
 * 03_dup_fcntl_vectored.c — the three kernel tables behind an fd: dup,
 *                           dup2 redirection, fork sharing, fcntl flags,
 *                           pread/pwrite and readv/writev
 *
 * Concept : An fd points to an OPEN FILE DESCRIPTION (struct file in the
 *           kernel), which holds the OFFSET and the status flags
 *           (O_APPEND, O_NONBLOCK), and points to the INODE (the file
 *           itself). dup() and fork() copy the fd -> they SHARE the offset.
 *           Two open() calls make two descriptions -> independent offsets.
 *           pread/pwrite take an explicit offset and do not move the shared
 *           one (safe from many threads); readv/writev move several buffers
 *           in ONE syscall (header + payload without copying them together).
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/03_dup_fcntl_vectored.c
 * Read    : ../README.md  (sections "3.4 Everything is a file" and
 *           "3.7 dup, fcntl, pread/pwrite, readv/writev")
 */
#define _GNU_SOURCE /* mkstemp, F_DUPFD_CLOEXEC */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
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

static char g_path[64];

static off_t pos(int fd) { return lseek(fd, 0, SEEK_CUR); }

/* ---- 1. two open()s vs dup() ----
 *
 *  fd table (per process)   open file descriptions      inodes
 *  +----+                   (system-wide, refcounted)
 *  | a  |------------------> [ off=5  flags=RDWR ] ----+
 *  | b  |------------------> [ off=0  flags=RDWR ] ----+--> [ inode 1234 ]
 *  | d  |---+                                          |    size, blocks,
 *  +----+   +--(dup of a)--> same description as a ----+    page cache
 */
static void demo_dup(void)
{
    int a = open(g_path, O_RDWR | O_CLOEXEC);
    int b = open(g_path, O_RDWR | O_CLOEXEC);
    CHECK(a >= 0 && b >= 0);
    CHECK(write(a, "12345", 5) == 5);
    printf("1) two open()s : after writing 5 bytes via a, offset(a)=%ld "
           "offset(b)=%ld (independent)\n", (long)pos(a), (long)pos(b));
    CHECK(pos(a) == 5 && pos(b) == 0);

    int d = dup(a); /* lowest free fd, same description as a */
    CHECK(d >= 0);
    CHECK(write(d, "678", 3) == 3);
    printf("   dup(a)=%d    : after writing 3 bytes via d, offset(a)=%ld "
           "offset(d)=%ld (shared)\n", d, (long)pos(a), (long)pos(d));
    CHECK(pos(a) == 8 && pos(d) == 8);

    /* Status flags live in the description -> shared by dup.
     * FD_CLOEXEC lives in the fd table slot -> NOT shared (dup clears it). */
    CHECK(fcntl(d, F_SETFL, fcntl(d, F_GETFL) | O_APPEND) == 0);
    CHECK((fcntl(a, F_GETFL) & O_APPEND) != 0);
    CHECK((fcntl(a, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(d, F_GETFD) & FD_CLOEXEC) == 0);
    printf("   O_APPEND set via d is visible via a (description flag); "
           "FD_CLOEXEC is per fd: a has it, d does not\n");

    int e = fcntl(a, F_DUPFD_CLOEXEC, 100); /* lowest free fd >= 100 */
    CHECK(e >= 100 && (fcntl(e, F_GETFD) & FD_CLOEXEC));
    printf("   fcntl(F_DUPFD_CLOEXEC, 100) = %d, with FD_CLOEXEC\n", e);
    close(a);
    close(b);
    close(d);
    close(e);
}

/* ---- 2. fork() shares the description too ---- */
static void demo_fork_offset(void)
{
    int fd = open(g_path, O_RDWR | O_TRUNC | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(write(fd, "parent;", 7) == 7);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        /* The child's fd table is a copy: same description, same offset. */
        _exit(write(fd, "child;", 6) == 6 ? 0 : 1);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    printf("2) after the child wrote 6 bytes, the PARENT's offset is %ld "
           "(7 + 6): fork shares the offset\n", (long)pos(fd));
    CHECK(pos(fd) == 13);
    CHECK(write(fd, "parent", 6) == 6);
    char buf[32] = {0};
    CHECK(pread(fd, buf, sizeof buf - 1, 0) == 19);
    printf("   file: \"%s\" (nothing overwritten)\n", buf);
    CHECK(strcmp(buf, "parent;child;parent") == 0);
    close(fd);
}

/* ---- 3. dup2: how the shell does `prog > file` ----
 *
 *   saved = dup(1)          fd 1 -> terminal, saved -> terminal
 *   dup2(file, 1)           fd 1 -> file      (old fd 1 closed atomically)
 *   printf(...); fflush     goes to the file
 *   dup2(saved, 1)          fd 1 -> terminal again
 */
static void demo_dup2(void)
{
    int fd = open(g_path, O_RDWR | O_TRUNC | O_CLOEXEC);
    CHECK(fd >= 0);
    fflush(stdout); /* stdio buffer must be empty before fd 1 moves */
    int saved = dup(STDOUT_FILENO);
    CHECK(saved >= 0);
    CHECK(dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);
    printf("this line went into the file\n");
    fflush(stdout);
    CHECK(dup2(saved, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved);

    char buf[64] = {0};
    CHECK(pread(fd, buf, sizeof buf - 1, 0) > 0);
    printf("3) dup2 redirect: the file contains \"%.*s\"\n",
           (int)strcspn(buf, "\n"), buf);
    CHECK(strcmp(buf, "this line went into the file\n") == 0);
    close(fd);
}

/* ---- 4. pread/pwrite: positional I/O ---- */
static void demo_pread(void)
{
    int fd = open(g_path, O_RDWR | O_TRUNC | O_CLOEXEC);
    CHECK(fd >= 0);
    /* A table of fixed-size records: record i lives at i * 16. */
    for (int i = 0; i < 4; i++) {
        char rec[16];
        snprintf(rec, sizeof rec, "record-%02d......", i);
        CHECK(pwrite(fd, rec, 16, (off_t)i * 16) == 16);
    }
    char rec[17] = {0};
    CHECK(pread(fd, rec, 16, 2 * 16) == 16);
    printf("4) pwrite 4 records, pread(record 2) = \"%s\"; shared offset "
           "still %ld\n", rec, (long)pos(fd));
    CHECK(strncmp(rec, "record-02", 9) == 0);
    CHECK(pos(fd) == 0); /* lseek+read from two threads would race here */
    close(fd);
}

/* ---- 5. writev/readv: scatter-gather ----
 *
 *   iov[0] -> [ header: magic, len ]  \
 *   iov[1] -> [ payload bytes ...  ]   >- one writev() -> one contiguous
 *                                     /   run of bytes in the file/socket
 */
struct hdr {
    uint32_t magic;
    uint32_t len;
};

static void demo_vectored(void)
{
    int fd = open(g_path, O_RDWR | O_TRUNC | O_CLOEXEC);
    CHECK(fd >= 0);
    const char payload[] = "sensor frame 42";
    struct hdr h = {0xC0DEF00Du, (uint32_t)sizeof payload};
    struct iovec out[2] = {
        {.iov_base = &h, .iov_len = sizeof h},
        {.iov_base = (void *)payload, .iov_len = sizeof payload},
    };
    ssize_t w = writev(fd, out, 2); /* no memcpy into a temp buffer */
    CHECK(w == (ssize_t)(sizeof h + sizeof payload));

    struct hdr h2;
    char back[sizeof payload];
    struct iovec in[2] = {
        {.iov_base = &h2, .iov_len = sizeof h2},
        {.iov_base = back, .iov_len = sizeof back},
    };
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(readv(fd, in, 2) == w);
    printf("5) writev(header+payload) = %zd bytes in one syscall; readv "
           "split it back: magic=0x%X len=%u \"%s\"\n", w, h2.magic, h2.len,
           back);
    CHECK(h2.magic == h.magic && h2.len == h.len &&
          strcmp(back, payload) == 0);
    close(fd);
}

int main(void)
{
    alarm(10);
    snprintf(g_path, sizeof g_path, "/tmp/dupdemo_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(g_path); /* unique name, created 0600 */
    CHECK(fd >= 0);
    close(fd);

    demo_dup();
    demo_fork_offset();
    demo_dup2();
    demo_pread();
    demo_vectored();

    CHECK(unlink(g_path) == 0);
    printf("03_dup_fcntl_vectored: all checks passed\n");
    return 0;
}
