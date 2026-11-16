/*
 * 04_stdio_buffering.c — stdio (FILE*) is a user-space buffer on top of
 *                        write(2): count the syscalls it makes for full,
 *                        line and no buffering; see why output is printed
 *                        twice after fork() and lost after a crash
 *
 * Concept : printf/fputs/fwrite copy into a buffer inside your process.
 *           The buffer is handed to write(2) only when it is full (fully
 *           buffered: files, pipes), at '\n' (line buffered: a terminal),
 *           immediately (unbuffered: stderr), or on fflush/fclose/exit().
 *           fork() copies the unflushed buffer into the child, so both
 *           flush it: duplicated output. _exit(), a crash or a kill -9
 *           never flushes it: lost output. To COUNT the write() calls
 *           without strace we use fopencookie(): a FILE whose "write(2)"
 *           is our own counting function.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/04_stdio_buffering.c
 *           strace -c -e trace=write ./build/linux-internals/00-syscalls-and-file-io/c/04_stdio_buffering
 * Read    : ../README.md  (section "3.8 stdio buffering vs raw syscalls")
 */
#define _GNU_SOURCE /* fopencookie */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
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

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* ---- a FILE that counts its "write(2)" calls ---- */
struct counter {
    int calls;
    size_t bytes;
};

static ssize_t count_write(void *cookie, const char *buf, size_t n)
{
    (void)buf;
    struct counter *c = cookie;
    c->calls++; /* exactly where a real FILE would call write(2) */
    c->bytes += n;
    return (ssize_t)n;
}

static int count_close(void *cookie)
{
    (void)cookie;
    return 0;
}

/* Write `lines` lines of 9 bytes through a stream in `mode`. */
static struct counter run_mode(int mode, int lines)
{
    struct counter c = {0, 0};
    cookie_io_functions_t io = {.read = NULL, .write = count_write,
                                .seek = NULL, .close = count_close};
    FILE *f = fopencookie(&c, "w", io); /* owned here, fclose below */
    CHECK(f != NULL);
    /* setvbuf must come before the first I/O on the stream. NULL buffer:
     * stdio allocates it (and frees it at fclose). */
    CHECK(setvbuf(f, NULL, mode, 4096) == 0);
    for (int i = 0; i < lines; i++)
        fprintf(f, "line %03d\n", i % 1000);
    CHECK(fclose(f) == 0); /* final flush happens here */
    CHECK(c.bytes == (size_t)lines * 9);
    return c;
}

static void demo_count(void)
{
    enum { LINES = 1000 };
    struct counter full = run_mode(_IOFBF, LINES);
    struct counter line = run_mode(_IOLBF, LINES);
    struct counter none = run_mode(_IONBF, LINES);
    printf("1) %d fprintf() calls, %d bytes, 4 KiB buffer -> write() calls:\n",
           LINES, LINES * 9);
    printf("     _IOFBF fully buffered (files, pipes)   %5d\n", full.calls);
    printf("     _IOLBF line buffered  (a terminal)      %5d\n", line.calls);
    printf("     _IONBF unbuffered     (stderr)          %5d\n", none.calls);
    /* 9000 bytes / 4096 -> 2 full buffers + the rest at fclose. */
    CHECK(full.calls >= 2 && full.calls <= 4);
    CHECK(line.calls == LINES); /* one per '\n' */
    CHECK(none.calls >= LINES); /* at least one per fprintf */

    printf("   this program's stdout is %s -> %s\n",
           isatty(STDOUT_FILENO) ? "a terminal" : "NOT a terminal",
           isatty(STDOUT_FILENO) ? "line buffered" : "FULLY buffered");
}

/* Read everything from fd until EOF into buf (NUL-terminated). */
static size_t slurp(int fd, char *buf, size_t cap)
{
    size_t got = 0;
    for (;;) {
        ssize_t r = read(fd, buf + got, cap - 1 - got);
        if (r < 0 && errno == EINTR)
            continue;
        CHECK(r >= 0);
        if (r == 0)
            break;
        got += (size_t)r;
    }
    buf[got] = '\0';
    return got;
}

static int count_substr(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

/* ---- 2. fork() with unflushed data: printed twice ----
 *
 *   fprintf(f, "BUFFERED ")    buffer: [BUFFERED ]       (nothing written)
 *   fork()                     parent buffer copy  +  child buffer copy
 *   both fclose()              write("BUFFERED ") x 2   -> duplicated
 */
static int fork_and_count(int flush_first)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    FILE *f = fdopen(p[1], "w"); /* a pipe: fully buffered */
    CHECK(f != NULL);
    fprintf(f, "BUFFERED ");
    if (flush_first)
        fflush(f);
    fflush(stdout); /* the same bug would duplicate OUR stdout too */
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(p[0]);
        fclose(f); /* flushes the child's copy of the buffer */
        _exit(0);
    }
    fclose(f); /* flushes the parent's copy, closes p[1] */
    char buf[256];
    slurp(p[0], buf, sizeof buf);
    close(p[0]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    return count_substr(buf, "BUFFERED");
}

static void demo_fork_duplicate(void)
{
    int bad = fork_and_count(0);
    int good = fork_and_count(1);
    printf("2) data in a FILE buffer at fork(): appears %d times; with "
           "fflush() before fork(): %d time\n", bad, good);
    CHECK(bad == 2 && good == 1);
}

/* ---- 3. _exit()/crash loses the buffer, exit() flushes it ---- */
static size_t child_output(int use_exit)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(p[0]);
        FILE *f = fdopen(p[1], "w");
        if (!f)
            _exit(2);
        fprintf(f, "last words before dying\n");
        if (use_exit) {
            /* exit() runs atexit handlers and flushes every FILE. (stdout
             * was flushed before fork, so nothing is printed twice.) */
            exit(0);
        }
        _exit(0); /* like a crash/SIGKILL: the buffer simply vanishes */
    }
    close(p[1]);
    char buf[256];
    size_t n = slurp(p[0], buf, sizeof buf);
    close(p[0]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    return n;
}

static void demo_crash_loss(void)
{
    size_t lost = child_output(0);
    size_t kept = child_output(1);
    printf("3) child printed 24 bytes then _exit(): reader got %zu bytes; "
           "then exit(): %zu bytes\n", lost, kept);
    CHECK(lost == 0 && kept == 24);
    printf("   -> on a crash the LAST log lines are the ones you lose: "
           "log to stderr, or fflush/setvbuf(_IOLBF)\n");
}

/* ---- 4. why buffering exists: cost of 1-byte write() vs fputc() ---- */
static void demo_speed(void)
{
    enum { N = 20000 };
    char path[64];
    snprintf(path, sizeof path, "/tmp/stdio_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(path);
    CHECK(fd >= 0);

    double t0 = now_ms();
    for (int i = 0; i < N; i++)
        CHECK(write(fd, "x", 1) == 1); /* N syscalls */
    double t_raw = now_ms() - t0;

    FILE *f = fdopen(dup(fd), "w"); /* owned here, fclose below */
    CHECK(f != NULL);
    t0 = now_ms();
    for (int i = 0; i < N; i++)
        fputc('x', f); /* N/4096 syscalls */
    CHECK(fclose(f) == 0);
    double t_stdio = now_ms() - t0;

    CHECK(lseek(fd, 0, SEEK_END) == 2 * N);
    close(fd);
    CHECK(unlink(path) == 0);
    printf("4) %d single bytes: write() each %.2f ms, fputc() %.2f ms "
           "(printed, not asserted)\n", N, t_raw, t_stdio);
}

int main(void)
{
    alarm(20);
    demo_count();
    demo_fork_duplicate();
    demo_crash_loss();
    demo_speed();
    printf("04_stdio_buffering: all checks passed\n");
    return 0;
}
