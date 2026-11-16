/*
 * 06_capacity_nonblock_sigpipe.c — pipe capacity (F_GETPIPE_SZ/F_SETPIPE_SZ),
 *                                  O_NONBLOCK + EAGAIN, SIGPIPE / EPIPE
 *
 * Concept : A pipe is a FIXED-size kernel buffer (64 KiB by default on
 *           Linux). When it is full a blocking writer sleeps; a non-blocking
 *           writer gets EAGAIN. When it is empty a non-blocking reader gets
 *           EAGAIN (not 0 — 0 means EOF). Writing to a pipe whose readers
 *           are all gone raises SIGPIPE (default action: kill the process)
 *           or, if SIGPIPE is ignored, write() fails with EPIPE.
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/06_capacity_nonblock_sigpipe.c
 * Read    : ../README.md  (section "3.6 Capacity, non-blocking I/O, SIGPIPE")
 */
#define _GNU_SOURCE /* F_GETPIPE_SZ, F_SETPIPE_SZ, pipe2 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int wait_status(pid_t pid)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    return st;
}

/* Fill a NON-blocking pipe with PIPE_BUF-sized chunks until EAGAIN.
 * Chunks of exactly PIPE_BUF are atomic: either the whole chunk fits or
 * the write fails with EAGAIN, so the total is a clean multiple. */
static long fill_until_eagain(int wfd)
{
    char chunk[PIPE_BUF];
    memset(chunk, 'x', sizeof chunk);
    long total = 0;
    for (;;) {
        ssize_t n = write(wfd, chunk, sizeof chunk);
        if (n < 0) {
            CHECK(errno == EAGAIN); /* == EWOULDBLOCK on Linux */
            return total;
        }
        CHECK(n == (ssize_t)sizeof chunk);
        total += n;
    }
}

static void print_pipe_max_size(void)
{
    FILE *f = fopen("/proc/sys/fs/pipe-max-size", "r");
    long max = -1;
    if (f) {
        if (fscanf(f, "%ld", &max) != 1)
            max = -1;
        fclose(f);
    }
    printf("/proc/sys/fs/pipe-max-size = %ld (largest F_SETPIPE_SZ without "
           "CAP_SYS_RESOURCE)\n", max);
}

int main(void)
{
    alarm(10);

    /* ---------- 1. capacity ---------- */
    int fd[2];
    /* O_NONBLOCK on both ends, set atomically by pipe2(). */
    CHECK(pipe2(fd, O_NONBLOCK | O_CLOEXEC) == 0);
    int cap = fcntl(fd[1], F_GETPIPE_SZ);
    CHECK(cap > 0);
    printf("1) default pipe capacity: %d bytes (%d pages)\n", cap,
           cap / (int)sysconf(_SC_PAGESIZE));

    long filled = fill_until_eagain(fd[1]);
    printf("   filled %ld bytes before write() said EAGAIN\n", filled);
    CHECK(filled == cap);

    /* Drain one chunk -> there is room again. */
    char buf[PIPE_BUF];
    CHECK(read(fd[0], buf, sizeof buf) == (ssize_t)sizeof buf);
    CHECK(write(fd[1], buf, sizeof buf) == (ssize_t)sizeof buf);

    /* Grow the pipe. The kernel rounds up to a power-of-two number of
     * pages. Unprivileged users are limited by pipe-max-size and by
     * pipe-user-pages-soft (if you own too many big pipes, EPERM). Also
     * shrinking below the bytes currently queued fails with EBUSY. */
    print_pipe_max_size();
    int want = 256 * 1024;
    int got = fcntl(fd[1], F_SETPIPE_SZ, want);
    if (got < 0) {
        printf("   F_SETPIPE_SZ(%d) failed: %s (limit reached; skipping)\n",
               want, strerror(errno));
    } else {
        printf("   F_SETPIPE_SZ(%d) -> new capacity %d bytes\n", want, got);
        CHECK(got >= want);
        CHECK(fcntl(fd[0], F_GETPIPE_SZ) == got); /* same pipe, both ends */
        long more = fill_until_eagain(fd[1]);
        printf("   now %ld more bytes fit (total queued %ld)\n", more,
               more + filled);
        CHECK(more + filled == got);
    }
    errno = 0;
    CHECK(fcntl(fd[1], F_SETPIPE_SZ, 4096) < 0 && errno == EBUSY);
    printf("   shrinking below the queued data: EBUSY (data is never "
           "dropped)\n");
    close(fd[0]);
    close(fd[1]);

    /* ---------- 2. non-blocking read of an empty pipe ----------
     *    read() returns  >0 : data
     *                     0 : EOF  (no writers left)
     *                    -1 : errno EAGAIN = "no data YET, writers exist"
     * Confusing EAGAIN with EOF is a classic event-loop bug. */
    CHECK(pipe2(fd, O_NONBLOCK | O_CLOEXEC) == 0);
    errno = 0;
    CHECK(read(fd[0], buf, 1) == -1 && errno == EAGAIN);
    printf("2) empty pipe, writer open, O_NONBLOCK read -> EAGAIN\n");
    close(fd[1]);
    CHECK(read(fd[0], buf, 1) == 0);
    printf("   empty pipe, writer closed           -> 0 (EOF)\n");
    close(fd[0]);

    /* ---------- 3. SIGPIPE: default action kills the writer ----------
     * This is why `yes | head -1` terminates: head exits, yes's next write
     * raises SIGPIPE and yes dies silently. */
    CHECK(pipe(fd) == 0);
    close(fd[0]); /* no reader at all */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        signal(SIGPIPE, SIG_DFL); /* make sure the default is in effect */
        (void)!write(fd[1], "x", 1); /* -> SIGPIPE -> killed here */
        _exit(0);                    /* never reached */
    }
    close(fd[1]);
    int st = wait_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE);
    printf("3) child wrote to a pipe with no reader: killed by signal %d "
           "(%s)\n", WTERMSIG(st), strsignal(WTERMSIG(st)));

    /* ---------- 4. SIGPIPE ignored -> write() returns EPIPE ----------
     * Servers (and anything holding sockets/pipes to other programs)
     * ignore SIGPIPE so that one dead peer does not kill the whole
     * process; they handle EPIPE at the call site instead. For sockets,
     * send(..., MSG_NOSIGNAL) does this per call. */
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    CHECK(sigaction(SIGPIPE, &sa, &old) == 0);
    CHECK(pipe(fd) == 0);
    close(fd[0]);
    errno = 0;
    CHECK(write(fd[1], "x", 1) == -1 && errno == EPIPE);
    printf("4) SIGPIPE ignored: write() -> -1, errno EPIPE (%s)\n",
           strerror(EPIPE));
    close(fd[1]);
    CHECK(sigaction(SIGPIPE, &old, NULL) == 0);

    printf("06_capacity_nonblock_sigpipe: all checks passed\n");
    return 0;
}
