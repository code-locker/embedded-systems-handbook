/*
 * 08_splice_tee_vmsplice.c — zero-copy plumbing with pipes: splice() moves
 *                            data between a pipe and a file/socket inside
 *                            the kernel, tee() duplicates a pipe without
 *                            consuming it, vmsplice() puts user pages into
 *                            a pipe
 *
 * Concept : read()+write() copies every byte twice: kernel -> user buffer
 *           -> kernel. A pipe's buffer is really a list of PAGE REFERENCES,
 *           so the kernel can move page references instead of bytes:
 *
 *   read/write copy:   file --copy--> user buf --copy--> file2
 *   splice:            file --page refs--> pipe --page refs--> file2
 *                                     (user space never touches the data)
 *
 *           One end of splice() must be a pipe; that is why the pipe is
 *           the "zero-copy glue" in Linux (sendfile() is built on it).
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/08_splice_tee_vmsplice.c
 * Read    : ../README.md  (section "3.8 Zero-copy: splice, tee, vmsplice")
 */
#define _GNU_SOURCE /* splice, tee, vmsplice */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
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

#define FILE_SIZE (200 * 1024) /* > pipe capacity: forces several rounds */

static int make_temp(char *tmpl)
{
    int fd = mkstemp(tmpl);
    CHECK(fd >= 0);
    return fd;
}

static void fill_pattern(unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(i * 31 + 7);
}

static void read_exact(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n > 0) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        CHECK(r > 0);
        p += r;
        n -= (size_t)r;
    }
}

int main(void)
{
    alarm(10);
    unsigned char *src = malloc(FILE_SIZE);
    unsigned char *chk = malloc(FILE_SIZE);
    CHECK(src && chk);
    fill_pattern(src, FILE_SIZE);

    char in_path[] = "/tmp/splice_in_XXXXXX";
    char out_path[] = "/tmp/splice_out_XXXXXX";
    int in = make_temp(in_path);
    int out = make_temp(out_path);
    CHECK(write(in, src, FILE_SIZE) == FILE_SIZE);
    CHECK(lseek(in, 0, SEEK_SET) == 0);

    /* ---- 1. file -> pipe -> file with splice ----
     * Loop: each splice moves at most what the pipe can hold (64 KiB). */
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    size_t moved = 0;
    int rounds = 0;
    while (moved < FILE_SIZE) {
        ssize_t n = splice(in, NULL, p[1], NULL, FILE_SIZE - moved,
                           SPLICE_F_MOVE);
        CHECK(n > 0);
        /* Drain exactly what we put in, into the output file. */
        ssize_t left = n;
        while (left > 0) {
            ssize_t m = splice(p[0], NULL, out, NULL, (size_t)left,
                               SPLICE_F_MOVE);
            CHECK(m > 0);
            left -= m;
        }
        moved += (size_t)n;
        rounds++;
    }
    CHECK(lseek(out, 0, SEEK_SET) == 0);
    read_exact(out, chk, FILE_SIZE);
    CHECK(memcmp(src, chk, FILE_SIZE) == 0);
    printf("1) splice: copied %d KiB file->pipe->file in %d rounds, no user "
           "buffer; contents identical\n", FILE_SIZE / 1024, rounds);

    /* ---- 2. tee: duplicate a pipe's contents into another pipe ----
     *
     *     p  [ "hello tee" ] --tee--> q [ "hello tee" ]   (p still full)
     *
     * Like the `tee` command, but nothing is copied: both pipes reference
     * the same pages. GStreamer's `tee` element is the same idea for
     * buffers (refcounted, not copied). */
    int q[2];
    CHECK(pipe2(q, O_CLOEXEC) == 0);
    const char msg[] = "hello tee";
    CHECK(write(p[1], msg, sizeof msg) == (ssize_t)sizeof msg);
    ssize_t t = tee(p[0], q[1], sizeof msg, 0);
    CHECK(t == (ssize_t)sizeof msg);
    char a[sizeof msg], b[sizeof msg];
    read_exact(p[0], a, sizeof a); /* original still there */
    read_exact(q[0], b, sizeof b); /* and the duplicate */
    CHECK(strcmp(a, msg) == 0 && strcmp(b, msg) == 0);
    printf("2) tee: both pipes delivered \"%s\"\n", a);

    /* ---- 3. vmsplice: user memory -> pipe by reference ----
     * The pipe now points at OUR pages. Rule: do not modify the buffer until
     * the reader has consumed the data (with SPLICE_F_GIFT you give the
     * pages away completely). Here we read it back right away. */
    struct iovec iov = {.iov_base = src, .iov_len = 4096};
    ssize_t v = vmsplice(p[1], &iov, 1, 0);
    CHECK(v == 4096);
    read_exact(p[0], chk, 4096);
    CHECK(memcmp(src, chk, 4096) == 0);
    printf("3) vmsplice: 4096 user bytes went through the pipe by page "
           "reference\n");

    close(p[0]); close(p[1]); close(q[0]); close(q[1]);
    close(in);
    close(out);
    CHECK(unlink(in_path) == 0 && unlink(out_path) == 0);
    free(src);
    free(chk);
    printf("08_splice_tee_vmsplice: all checks passed\n");
    return 0;
}
