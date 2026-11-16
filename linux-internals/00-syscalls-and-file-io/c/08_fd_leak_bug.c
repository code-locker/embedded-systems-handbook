/*
 * 08_fd_leak_bug.c — INTENTIONAL BUG: a file descriptor leak on an early
 *                    return; after enough calls open() fails with EMFILE
 *
 * Concept : Every open()/socket()/pipe()/accept() takes a slot in the fd
 *           table; only close() gives it back. A function that returns
 *           early on an error path without close() leaks one slot per call.
 *           Nothing crashes at first. Days later, a long-running daemon on
 *           the board can no longer open its config, accept a client or
 *           open /dev/video0: "Too many open files" (EMFILE). To make it
 *           show up in a fraction of a second the program lowers its own
 *           RLIMIT_NOFILE to 32 (no root needed to LOWER a limit).
 *
 *           read_sample():   fd = open(...)
 *                            if (sample looks bad) return -1;   <- BUG:
 *                            ...                                   fd leaks
 *                            close(fd);
 *
 * Tool    : NOT AddressSanitizer: LeakSanitizer tracks malloc, not fds.
 *           Catch it with
 *             - the SELF-CHECK here: it counts /proc/self/fd and exits 1
 *               when open() fails with EMFILE;
 *             - `ls -l /proc/<pid>/fd | wc -l` growing on a live process;
 *             - valgrind --track-fds=yes (build with SAN=0): lists the fds
 *               still open at exit and where they were opened;
 *             - strace -f -e trace=openat,close: opens without closes.
 *           Run it:
 *             make build/linux-internals/00-syscalls-and-file-io/c/08_fd_leak_bug
 *             ./build/linux-internals/00-syscalls-and-file-io/c/08_fd_leak_bug; echo $?
 *             make SAN=0 BUILD=build/nosan build/nosan/linux-internals/00-syscalls-and-file-io/c/08_fd_leak_bug
 *             valgrind --track-fds=yes ./build/nosan/linux-internals/00-syscalls-and-file-io/c/08_fd_leak_bug
 *           Not run by `make test` (it is a _bug file).
 * Read    : ../README.md  (section "7. Common mistakes and pitfalls")
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

static int count_open_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n - 1; /* minus the fd opendir is using */
}

/* Reads one "sample" from the file. Every 3rd sample is "bad". */
static int read_sample(const char *path, int i)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -2; /* errno says why */
    char c;
    if (read(fd, &c, 1) != 1)
        c = 0;
    if (i % 3 == 0)
        return -1; /* BUG: early return without close(fd) */
    close(fd);
    return c;
}

int main(void)
{
    alarm(10);
    struct rlimit rl, orig;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return 2;
    orig = rl;
    rl.rlim_cur = 32; /* lowering the soft limit needs no privilege */
    if (setrlimit(RLIMIT_NOFILE, &rl) != 0)
        return 2;

    char path[64];
    snprintf(path, sizeof path, "/tmp/fdleak_%d_XXXXXX", (int)getpid());
    int tfd = mkstemp(path);
    if (tfd < 0)
        return 2;
    if (write(tfd, "7", 1) != 1)
        return 2;
    close(tfd);

    int start = count_open_fds();
    printf("RLIMIT_NOFILE soft limit set to 32; %d fds open at start\n",
           start);
    int rc = 0;
    for (int i = 0; i < 1000; i++) {
        int r = read_sample(path, i);
        if (r == -2) {
            int e = errno;
            /* Raise the limit again so we can open /proc/self/fd to count
             * (opendir needs an fd too!) and so LeakSanitizer can run its
             * exit check, which also opens files. */
            setrlimit(RLIMIT_NOFILE, &orig);
            printf("call %d: open() failed: %s\n", i, strerror(e));
            int now = count_open_fds();
            printf("open fds now: %d (started with %d) -> %d leaked\n", now,
                   start, now - start);
            if (e == EMFILE) {
                printf("BUG CAUGHT: fd leak on the early-return path of "
                       "read_sample()\n");
                rc = 1;
            } else {
                rc = 2;
            }
            break;
        }
    }
    unlink(path);
    fflush(stdout);
    if (rc == 0)
        printf("no EMFILE?! (should not happen)\n");
    return rc;
}
