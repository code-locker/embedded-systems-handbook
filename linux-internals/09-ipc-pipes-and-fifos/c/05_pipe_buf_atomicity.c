/*
 * 05_pipe_buf_atomicity.c — many writers, one pipe: writes of <= PIPE_BUF
 *                           bytes are atomic (never interleaved); bigger
 *                           writes may be split and mixed
 *
 * Concept : POSIX guarantees that a write() of at most PIPE_BUF bytes
 *           (4096 on Linux) to a pipe/FIFO lands as ONE contiguous block,
 *           even with several writers at once. That is what makes
 *           "many logger processes -> one FIFO" work with fixed records.
 *           Above PIPE_BUF the kernel may copy part of your data, sleep
 *           because the pipe is full, and let another writer in.
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/05_pipe_buf_atomicity.c
 * Read    : ../README.md  (section "3.5 PIPE_BUF atomicity")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h> /* PIPE_BUF */
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

#define N_WRITERS 4
#define N_RECORDS 200
#define REC_SIZE 512 /* <= PIPE_BUF: each write() is atomic */

#define BIG_WRITERS 2
#define BIG_WRITES 2
#define BIG_SIZE (128 * 1024) /* > PIPE_BUF: may be split and interleaved */

_Static_assert(REC_SIZE <= PIPE_BUF, "records must fit in PIPE_BUF");

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

/* Read until EOF into a malloc'd buffer of known expected size.
 * Ownership: caller frees. */
static unsigned char *read_exact_stream(int fd, size_t expect)
{
    unsigned char *buf = malloc(expect + 1);
    CHECK(buf != NULL);
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, expect + 1 - total);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (n == 0)
            break;
        total += (size_t)n;
        CHECK(total <= expect); /* more bytes than written = corruption */
    }
    CHECK(total == expect);
    return buf;
}

/*
 * Record layout (REC_SIZE bytes, every byte checkable):
 *
 *   +--------+--------+--------+-----------------------------------+
 *   | 'W'    | writer | seq hi | seq lo |  fill = 'a' + writer ... |
 *   +--------+--------+--------+-----------------------------------+
 *
 * If two writes interleaved, a record would contain fill bytes of two
 * different writers, or a header in the middle.
 */
static void make_record(unsigned char *rec, int writer, int seq)
{
    rec[0] = 'W';
    rec[1] = (unsigned char)writer;
    rec[2] = (unsigned char)(seq >> 8);
    rec[3] = (unsigned char)(seq & 0xff);
    memset(rec + 4, 'a' + writer, REC_SIZE - 4);
}

int main(void)
{
    alarm(10);
    printf("PIPE_BUF = %d bytes (atomic-write limit for pipes/FIFOs)\n",
           PIPE_BUF);

    /* ---------- part 1: small records, N writers ---------- */
    int fd[2];
    CHECK(pipe(fd) == 0);
    pid_t pids[N_WRITERS];
    fflush(stdout);
    for (int w = 0; w < N_WRITERS; w++) {
        pids[w] = fork();
        CHECK(pids[w] >= 0);
        if (pids[w] == 0) {
            alarm(5);
            close(fd[0]);
            unsigned char rec[REC_SIZE];
            for (int s = 0; s < N_RECORDS; s++) {
                make_record(rec, w, s);
                /* ONE write() per record: atomic because <= PIPE_BUF.
                 * (A blocking pipe write of <= PIPE_BUF also never returns
                 * a short count.) Two write() calls per record (header,
                 * then body) would NOT be atomic as a pair. */
                ssize_t n = write(fd[1], rec, REC_SIZE);
                CHECK(n == REC_SIZE);
            }
            close(fd[1]);
            _exit(0);
        }
    }
    close(fd[1]); /* parent only reads: EOF when all children are done */

    size_t expect = (size_t)N_WRITERS * N_RECORDS * REC_SIZE;
    unsigned char *s = read_exact_stream(fd[0], expect);
    close(fd[0]);
    for (int w = 0; w < N_WRITERS; w++)
        wait_ok(pids[w]);

    int next_seq[N_WRITERS] = {0};
    int switches = 0, prev = -1;
    for (size_t off = 0; off < expect; off += REC_SIZE) {
        const unsigned char *rec = s + off;
        CHECK(rec[0] == 'W');
        int w = rec[1];
        CHECK(w < N_WRITERS);
        int seq = (rec[2] << 8) | rec[3];
        /* Per writer, records arrive in the order that writer wrote them
         * (the pipe is FIFO); between writers, any order is possible. */
        CHECK(seq == next_seq[w]);
        next_seq[w]++;
        for (size_t i = 4; i < REC_SIZE; i++)
            CHECK(rec[i] == 'a' + w); /* no foreign bytes = not interleaved */
        if (w != prev)
            switches++;
        prev = w;
    }
    for (int w = 0; w < N_WRITERS; w++)
        CHECK(next_seq[w] == N_RECORDS);
    free(s);
    printf("part 1: %d writers x %d records of %d B: every record intact, "
           "per-writer order kept\n", N_WRITERS, N_RECORDS, REC_SIZE);
    printf("        writer switched %d times in the stream "
           "(timing-dependent, shows they really ran concurrently)\n",
           switches);

    /* ---------- part 2: big writes (> PIPE_BUF) ----------
     * Each writer does write(fd, 128 KiB). The pipe holds only 64 KiB, so
     * the kernel copies what fits, sleeps the writer, and another writer
     * may get in. We only REPORT what happens: it is allowed, not required. */
    CHECK(pipe(fd) == 0);
    pid_t bpids[BIG_WRITERS];
    fflush(stdout);
    for (int w = 0; w < BIG_WRITERS; w++) {
        bpids[w] = fork();
        CHECK(bpids[w] >= 0);
        if (bpids[w] == 0) {
            alarm(5);
            close(fd[0]);
            unsigned char *big = malloc(BIG_SIZE);
            CHECK(big != NULL);
            memset(big, 'A' + w, BIG_SIZE);
            for (int k = 0; k < BIG_WRITES; k++) {
                /* Blocking write: returns only when all bytes are in
                 * (unless a signal interrupts it). */
                CHECK(write(fd[1], big, BIG_SIZE) == BIG_SIZE);
            }
            free(big);
            close(fd[1]);
            _exit(0);
        }
    }
    close(fd[1]);
    expect = (size_t)BIG_WRITERS * BIG_WRITES * BIG_SIZE;
    s = read_exact_stream(fd[0], expect);
    close(fd[0]);
    for (int w = 0; w < BIG_WRITERS; w++)
        wait_ok(bpids[w]);

    size_t count[BIG_WRITERS] = {0};
    int runs = 0;
    for (size_t i = 0; i < expect; i++) {
        int w = s[i] - 'A';
        CHECK(w >= 0 && w < BIG_WRITERS);
        count[w]++;
        if (i == 0 || s[i] != s[i - 1])
            runs++;
    }
    free(s);
    for (int w = 0; w < BIG_WRITERS; w++)
        CHECK(count[w] == (size_t)BIG_WRITES * BIG_SIZE); /* no bytes lost */
    /* If every 128 KiB write were atomic there would be at most
     * BIG_WRITERS*BIG_WRITES runs. More runs = writes were split. */
    printf("part 2: %d writers x %d writes of %d KiB: %d byte runs in the "
           "stream (<= %d would mean no interleaving; timing-dependent)\n",
           BIG_WRITERS, BIG_WRITES, BIG_SIZE / 1024, runs,
           BIG_WRITERS * BIG_WRITES);
    printf("        -> %s\n", runs > BIG_WRITERS * BIG_WRITES
                                  ? "big writes WERE interleaved"
                                  : "not interleaved this time (not guaranteed!)");

    printf("05_pipe_buf_atomicity: all checks passed\n");
    return 0;
}
