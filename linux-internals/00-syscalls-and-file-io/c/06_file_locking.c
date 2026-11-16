/*
 * 06_file_locking.c — advisory file locks between processes: flock(),
 *                     POSIX record locks with fcntl(F_SETLK/F_GETLK), the
 *                     "close any fd drops the lock" trap, and OFD locks
 *
 * Concept : Linux file locks are ADVISORY: they stop only processes that
 *           also ask for the lock; read()/write() ignore them. Three APIs:
 *             flock(fd, LOCK_EX)   whole file, owned by the open file
 *                                  description (BSD style)
 *             fcntl(F_SETLK)       byte ranges, owned by the PROCESS (POSIX)
 *                                  -> closing ANY fd of that file in the
 *                                  process drops ALL its locks on it
 *             fcntl(F_OFD_SETLK)   byte ranges, owned by the open file
 *                                  description: the sane one (Linux 3.15+)
 *           Typical uses: a pid file so a daemon runs once, serialising
 *           access to a shared config/database file (SQLite uses fcntl
 *           locks), and `flock(1)` in shell scripts.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/06_file_locking.c
 * Read    : ../README.md  (section "3.11 File locking")
 */
#define _GNU_SOURCE /* F_OFD_SETLK, mkstemp */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
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

enum kind { K_FLOCK, K_POSIX, K_OFD };
enum { GOT_IT = 0, BUSY = 1 };

/* Try a NON-blocking exclusive lock with the given API. Returns GOT_IT or
 * BUSY; anything else is a test failure. Non-blocking keeps the test from
 * ever hanging: a blocking F_SETLKW/LOCK_EX would wait forever on a bug. */
static int try_lock(int fd, enum kind k, off_t start, off_t len)
{
    int r;
    if (k == K_FLOCK) {
        r = flock(fd, LOCK_EX | LOCK_NB);
    } else {
        struct flock fl;
        memset(&fl, 0, sizeof fl); /* l_pid MUST be 0 for OFD locks */
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = start;
        fl.l_len = len; /* 0 would mean "to end of file, and beyond" */
        r = fcntl(fd, k == K_OFD ? F_OFD_SETLK : F_SETLK, &fl);
    }
    if (r == 0)
        return GOT_IT;
    CHECK(errno == EAGAIN || errno == EACCES || errno == EWOULDBLOCK);
    return BUSY;
}

/* Another PROCESS opens the file itself and tries the lock. */
static int child_try(enum kind k, off_t start, off_t len)
{
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        int fd = open(g_path, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            _exit(2);
        _exit(try_lock(fd, k, start, len)); /* exit: locks die with us */
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st));
    CHECK(WEXITSTATUS(st) == GOT_IT || WEXITSTATUS(st) == BUSY);
    return WEXITSTATUS(st);
}

static const char *s(int r) { return r == GOT_IT ? "got it" : "BUSY"; }

/* ---- 1. flock: whole-file lock ---- */
static void demo_flock(void)
{
    int fd = open(g_path, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(flock(fd, LOCK_EX) == 0);
    int r1 = child_try(K_FLOCK, 0, 0);
    CHECK(flock(fd, LOCK_UN) == 0);
    int r2 = child_try(K_FLOCK, 0, 0);
    printf("1) flock: parent holds LOCK_EX -> child %s; after LOCK_UN -> "
           "child %s\n", s(r1), s(r2));
    CHECK(r1 == BUSY && r2 == GOT_IT);

    /* Advisory: a process that does not ask can still write. */
    CHECK(flock(fd, LOCK_EX) == 0);
    int fd2 = open(g_path, O_WRONLY | O_CLOEXEC);
    CHECK(fd2 >= 0 && write(fd2, "ignored the lock", 16) == 16);
    printf("   advisory only: a write() through another fd succeeded while "
           "the lock was held\n");
    close(fd2);
    close(fd); /* last fd of the description -> flock released */
}

/* ---- 2. POSIX byte-range locks ----
 *
 *   offset:   0          50         100        150        200
 *   parent:   [=====WRLCK=====)
 *   child A:             [=====want=====)      -> overlaps -> BUSY
 *   child B:                         [=====want=====)  -> free -> got it
 */
static void demo_posix_ranges(void)
{
    int fd = open(g_path, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(try_lock(fd, K_POSIX, 0, 100) == GOT_IT);
    int a = child_try(K_POSIX, 50, 100);
    int b = child_try(K_POSIX, 100, 100);
    printf("2) POSIX: parent locks [0,100) -> child [50,150) %s, child "
           "[100,200) %s\n", s(a), s(b));
    CHECK(a == BUSY && b == GOT_IT);

    /* F_GETLK asks "who would block me?" (from another process). */
    fflush(stdout);
    pid_t parent = getpid();
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        int cfd = open(g_path, O_RDONLY | O_CLOEXEC);
        struct flock fl;
        memset(&fl, 0, sizeof fl);
        fl.l_type = F_RDLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start = 10;
        fl.l_len = 1;
        if (cfd < 0 || fcntl(cfd, F_GETLK, &fl) != 0)
            _exit(2);
        _exit(fl.l_type == F_WRLCK && fl.l_pid == parent && fl.l_start == 0 &&
                      fl.l_len == 100
                  ? 0
                  : 3);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    printf("   F_GETLK from a child: byte 10 is held by pid %d as F_WRLCK "
           "[0,100)\n", (int)parent);
    close(fd);
}

/* ---- 3. the POSIX trap and the OFD fix ----
 *
 *   POSIX lock owner = (process, inode):
 *     fd1 = open(f); lock(fd1)        <- your code
 *     fd2 = open(f); close(fd2)       <- a library reading the same file
 *     ===> ALL of this process's locks on f are gone. Silently.
 *
 *   OFD lock owner = the open file description of fd1: close(fd2) is
 *   irrelevant. Also, two OFDs in ONE process conflict (POSIX locks in the
 *   same process never conflict with each other — useless between threads).
 */
static void demo_trap_and_ofd(void)
{
    int fd1 = open(g_path, O_RDWR | O_CLOEXEC);
    CHECK(fd1 >= 0);
    CHECK(try_lock(fd1, K_POSIX, 0, 10) == GOT_IT);
    int before = child_try(K_POSIX, 0, 10);
    int fd2 = open(g_path, O_RDONLY | O_CLOEXEC);
    CHECK(fd2 >= 0);
    close(fd2); /* innocent-looking ... */
    int after = child_try(K_POSIX, 0, 10);
    printf("3) POSIX lock: child %s; we open+close ANOTHER fd of the file; "
           "child %s  <- lock silently lost\n", s(before), s(after));
    CHECK(before == BUSY && after == GOT_IT);
    close(fd1);

    fd1 = open(g_path, O_RDWR | O_CLOEXEC);
    CHECK(fd1 >= 0);
    CHECK(try_lock(fd1, K_OFD, 0, 10) == GOT_IT);
    fd2 = open(g_path, O_RDWR | O_CLOEXEC);
    CHECK(fd2 >= 0);
    int same_proc = try_lock(fd2, K_OFD, 0, 10); /* different OFD */
    close(fd2);
    int child = child_try(K_OFD, 0, 10);
    printf("   OFD lock: a 2nd open() in the SAME process -> %s; after "
           "closing it a child -> %s  <- still held\n", s(same_proc),
           s(child));
    CHECK(same_proc == BUSY && child == BUSY);
    close(fd1); /* last reference to the OFD: lock released */
    CHECK(child_try(K_OFD, 0, 10) == GOT_IT);
    printf("   closing fd1 (the owning description) released it\n");
}

int main(void)
{
    alarm(20);
    snprintf(g_path, sizeof g_path, "/tmp/locks_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(g_path);
    CHECK(fd >= 0);
    char zeros[256] = {0};
    CHECK(write(fd, zeros, sizeof zeros) == (ssize_t)sizeof zeros);
    close(fd);

    demo_flock();
    demo_posix_ranges();
    demo_trap_and_ofd();

    CHECK(unlink(g_path) == 0);
    printf("06_file_locking: all checks passed\n");
    return 0;
}
