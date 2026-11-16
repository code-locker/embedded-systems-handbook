/*
 * 01_pipe_fork.c — pipe() + fork(): one-way byte stream from child to parent,
 *                  closing unused ends, EOF semantics, pipe2(O_CLOEXEC)
 *
 * Concept : A pipe is a kernel byte buffer with two file descriptors: fd[0]
 *           to read, fd[1] to write. fork() duplicates BOTH ends into the
 *           child, so each side must close the end it does not use. The
 *           reader sees EOF (read() == 0) only when EVERY write end in EVERY
 *           process is closed — that is why closing unused ends matters
 *           (02_pipe_eof_bug.c shows what happens if you forget).
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/01_pipe_fork.c
 * Read    : ../README.md  (section "3.1 pipe() + fork()")
 */
#define _GNU_SOURCE /* pipe2() */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* _exit(), not exit(): in a forked child exit() would flush stdio buffers
 * inherited from the parent (printing things twice) and run atexit hooks
 * that belong to the parent. We fflush(stdout) first so our own text is
 * not lost. */
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define N_LINES 5

/* Wait for one child and insist that it exited 0: a failing CHECK in the
 * child must fail the whole test, not vanish silently. */
static void wait_ok(pid_t pid)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* write() may write less than asked (signals, non-blocking fds), so robust
 * code loops. For a blocking pipe and len <= PIPE_BUF it is all-or-nothing,
 * but writing the loop costs nothing. */
static void write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n > 0);
        p += n;
        len -= (size_t)n;
    }
}

int main(void)
{
    /*
     *   pipe(fd) creates:
     *
     *        fd[1] (write end)  -->  [ kernel pipe buffer, 64 KiB ]  -->  fd[0] (read end)
     *
     *   after fork() BOTH processes hold BOTH ends:
     *
     *     parent: fd[0] fd[1]          child: fd[0] fd[1]
     *               |    X  close        X  close  |
     *               v                               \
     *            reads  <----------  pipe  <-------- writes
     *
     *   Rule: close what you do not use, immediately after fork().
     */
    int fd[2];
    CHECK(pipe(fd) == 0);
    printf("pipe created: read end fd %d, write end fd %d\n", fd[0], fd[1]);

    fflush(stdout); /* empty the buffer so the child does not inherit it */
    pid_t pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        /* ---- child: the WRITER ---- */
        alarm(5); /* watchdog: a bug must never hang the test */
        CHECK(close(fd[0]) == 0); /* child does not read */
        for (int i = 0; i < N_LINES; i++) {
            char line[64];
            int len = snprintf(line, sizeof line, "line %d from child %d\n",
                               i, (int)getpid());
            write_all(fd[1], line, (size_t)len);
        }
        /* Closing the last write end is what produces EOF for the reader.
         * _exit() would close it too; being explicit documents intent. */
        CHECK(close(fd[1]) == 0);
        _exit(0);
    }

    /* ---- parent: the READER ---- */
    alarm(5);
    CHECK(close(fd[1]) == 0); /* MUST close: else read() never returns 0 */

    /* Read until EOF. A pipe is a BYTE STREAM: one read() may return part of
     * a line, or several lines glued together. Message boundaries are not
     * kept — framing (here '\n') is the application's job. */
    char all[1024];
    size_t total = 0;
    int reads = 0;
    for (;;) {
        ssize_t n = read(fd[0], all + total, sizeof all - 1 - total);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (n == 0)
            break; /* EOF: every write end is closed */
        total += (size_t)n;
        reads++;
        CHECK(total < sizeof all - 1);
    }
    all[total] = '\0';
    CHECK(close(fd[0]) == 0);
    wait_ok(pid);

    printf("parent read %zu bytes in %d read() call(s) (timing-dependent):\n%s",
           total, reads, all);

    /* Verify: exactly N_LINES lines, in order (a pipe is FIFO). */
    int lines = 0;
    char *save = NULL;
    for (char *tok = strtok_r(all, "\n", &save); tok;
         tok = strtok_r(NULL, "\n", &save)) {
        char want[32];
        snprintf(want, sizeof want, "line %d ", lines);
        CHECK(strncmp(tok, want, strlen(want)) == 0);
        lines++;
    }
    CHECK(lines == N_LINES);

    /* ---- reading an empty pipe whose writers are all gone -> EOF ---- */
    CHECK(pipe(fd) == 0);
    CHECK(close(fd[1]) == 0);
    char c;
    CHECK(read(fd[0], &c, 1) == 0); /* immediately 0 = EOF, no blocking */
    CHECK(close(fd[0]) == 0);
    printf("empty pipe with no writers: read() returned 0 (EOF) at once\n");

    /* ---- pipe2(O_CLOEXEC): close-on-exec set atomically ----
     * pipe() + fcntl(FD_CLOEXEC) has a race in threaded programs: another
     * thread may fork+exec between the two calls and leak the fd into the
     * new program. pipe2() sets the flag in the same system call. */
    CHECK(pipe2(fd, O_CLOEXEC) == 0);
    CHECK(fcntl(fd[0], F_GETFD) & FD_CLOEXEC);
    CHECK(fcntl(fd[1], F_GETFD) & FD_CLOEXEC);
    printf("pipe2(O_CLOEXEC): both ends have FD_CLOEXEC set\n");
    CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);

    printf("01_pipe_fork: all checks passed\n");
    return 0;
}
