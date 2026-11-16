/*
 * 07_named_fifo.c — named pipes (FIFOs): mkfifo, blocking open() semantics,
 *                   two "unrelated" processes that share only a path name,
 *                   cleanup with unlink
 *
 * Concept : An anonymous pipe can only be shared by inheritance (fork). A
 *           FIFO is the same kernel pipe, but it has a NAME in the
 *           filesystem, so any process with permission can open it. The
 *           data never touches the disk: the inode is only a rendezvous.
 *           open() blocks until the other side opens too:
 *              open(O_RDONLY) waits for a writer, open(O_WRONLY) waits for
 *              a reader; with O_NONBLOCK, read-open succeeds at once and
 *              write-open fails with ENXIO if there is no reader.
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/07_named_fifo.c
 * Read    : ../README.md  (section "3.7 Named pipes (FIFOs)")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

#define N_MSGS 10

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

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/*
 * The "other program". It gets NOTHING from the parent except the path
 * string — exactly the situation of two unrelated programs started from
 * different shells (`./producer /tmp/x/fifo` and `./consumer /tmp/x/fifo`).
 * To make that honest, it closes every inherited fd above stderr first.
 */
static int producer_main(const char *path)
{
    for (int fd = 3; fd < 64; fd++)
        close(fd); /* forget everything inherited (EBADF is fine) */

    /* Let the reader block in open() for a while, to SEE the rendezvous. */
    struct timespec d = {0, 200 * 1000 * 1000};
    nanosleep(&d, NULL);

    int fd = open(path, O_WRONLY | O_CLOEXEC); /* reader exists -> no wait */
    if (fd < 0)
        return 2;
    for (int i = 0; i < N_MSGS; i++) {
        char msg[32]; /* fixed-size records: atomic, easy to frame */
        memset(msg, 0, sizeof msg);
        snprintf(msg, sizeof msg, "reading #%d = %d", i, i * i);
        if (write(fd, msg, sizeof msg) != (ssize_t)sizeof msg)
            return 3;
    }
    close(fd); /* last writer closes -> reader gets EOF */
    return 0;
}

int main(void)
{
    alarm(10);

    /* A private directory from mkdtemp: unique per run, and nobody else can
     * plant a file with our FIFO name (mkfifo fails with EEXIST if a name is
     * taken, which is also a safety feature). The pid in the name is for
     * humans looking at /tmp. */
    char dir[] = "/tmp/fifo_demo_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[96];
    snprintf(path, sizeof path, "%s/sensor-%d.fifo", dir, (int)getpid());

    CHECK(mkfifo(path, 0600) == 0);
    struct stat sb;
    CHECK(stat(path, &sb) == 0);
    CHECK(S_ISFIFO(sb.st_mode));
    printf("mkfifo %s  (ls -l shows type 'p', size %lld)\n", path,
           (long long)sb.st_size);

    /* ---- 1. O_NONBLOCK open rules (no peer exists yet) ---- */
    errno = 0;
    CHECK(open(path, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO);
    printf("1) open(O_WRONLY|O_NONBLOCK) with no reader -> ENXIO\n");
    int rfd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    CHECK(rfd >= 0);
    char tmp[8];
    CHECK(read(rfd, tmp, sizeof tmp) == 0); /* no writer -> EOF, not EAGAIN */
    close(rfd);
    printf("   open(O_RDONLY|O_NONBLOCK) succeeds at once; read() -> 0 "
           "because there is no writer\n");

    /* ---- 2. blocking open: the rendezvous ---- */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        _exit(producer_main(path));
    }

    double t0 = now_ms();
    rfd = open(path, O_RDONLY | O_CLOEXEC); /* BLOCKS until the writer opens */
    CHECK(rfd >= 0);
    double waited = now_ms() - t0;
    printf("2) open(O_RDONLY) blocked %.0f ms until the producer opened the "
           "write side (timing-dependent)\n", waited);

    int got = 0;
    char msg[32];
    for (;;) {
        /* Records are 32 bytes (<= PIPE_BUF) so each arrives whole, but a
         * single read() could still return several; we ask for exactly
         * one record at a time to keep framing simple. */
        ssize_t n = read(rfd, msg, sizeof msg);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (n == 0)
            break;
        CHECK(n == (ssize_t)sizeof msg);
        char want[32];
        snprintf(want, sizeof want, "reading #%d = %d", got, got * got);
        CHECK(strcmp(msg, want) == 0);
        got++;
    }
    close(rfd);
    wait_ok(pid);
    printf("   received %d/%d records in order, then EOF\n", got, N_MSGS);
    CHECK(got == N_MSGS);

    /* ---- 3. O_RDWR trick (Linux-specific, not POSIX) ----
     * A daemon that wants to keep a FIFO open across many writers opens it
     * O_RDWR: open() never blocks (it is its own writer) and read() never
     * sees EOF when a client disconnects. Downside: you can never detect
     * "no writers left". */
    int rw = open(path, O_RDWR | O_CLOEXEC);
    CHECK(rw >= 0);
    CHECK(write(rw, "ping", 4) == 4);
    char p4[4];
    CHECK(read(rw, p4, 4) == 4 && memcmp(p4, "ping", 4) == 0);
    close(rw);
    printf("3) open(O_RDWR) on a FIFO does not block (Linux); used by "
           "daemons that must not see EOF\n");

    /* ---- 4. cleanup ----
     * The FIFO inode stays in the filesystem until unlinked, like any file.
     * Forgetting it leaves stale FIFOs in /tmp; the next run's mkfifo then
     * fails with EEXIST. Data, however, is gone the moment both sides
     * close: a FIFO never stores anything persistently. */
    CHECK(unlink(path) == 0);
    CHECK(rmdir(dir) == 0);
    CHECK(access(path, F_OK) != 0);
    printf("4) unlink + rmdir: FIFO removed\n");

    printf("07_named_fifo: all checks passed\n");
    return 0;
}
