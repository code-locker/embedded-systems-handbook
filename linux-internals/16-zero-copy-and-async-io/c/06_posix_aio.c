/*
 * 06_posix_aio.c — POSIX asynchronous I/O (aio_read / aio_suspend /
 *                  aio_return, lio_listio) and how glibc really does it
 *
 * Concept : POSIX AIO lets you start a read or write and get on with other
 *           work: aio_read() returns at once, aio_error() says EINPROGRESS
 *           until it is done, aio_suspend() waits for any of a list,
 *           aio_return() gives the byte count (exactly once per request).
 *           lio_listio() submits a batch in one call. The catch on Linux:
 *           glibc implements all of it in USER SPACE with a pool of helper
 *           threads doing ordinary blocking pread/pwrite. It is portable
 *           and easy, but it is not kernel async I/O, it costs a thread
 *           per concurrent request, and requests on one fd are serialised.
 *           The program watches /proc/self/task to show the helper threads.
 *           Kernel-native async I/O is io_uring (c/07).
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/06_posix_aio.c
 * Read    : ../README.md  (section 3.7 "Async I/O models")
 */
#define _GNU_SOURCE /* mkostemp */
#include <aio.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define NREQ    8
#define REQ_LEN (128u << 10)          /* 8 x 128 KiB = 1 MiB */
#define FSIZE   (NREQ * REQ_LEN)

static int count_threads(void)
{
    DIR *d = opendir("/proc/self/task");
    CHECK(d);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static unsigned char pattern(size_t i) { return (unsigned char)((i * 31u) ^ (i >> 11)); }

static int tmpfile_fd(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/zc06-%d-XXXXXX", (int)getpid());
    int fd = mkostemp(path, O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(unlink(path) == 0); /* nameless: nothing to clean up on any exit */
    return fd;
}

int main(void)
{
    alarm(20);
    int fd = tmpfile_fd();
    unsigned char *src = malloc(FSIZE);
    CHECK(src);
    for (size_t i = 0; i < FSIZE; i++)
        src[i] = pattern(i);
    CHECK(pwrite(fd, src, FSIZE, 0) == (ssize_t)FSIZE);

    /* ---- 1. eight aio_read requests in flight at once ---- */
    int threads_before = count_threads();
    static struct aiocb cb[NREQ]; /* must stay valid until aio_return() */
    unsigned char *dst = calloc(1, FSIZE);
    CHECK(dst);
    for (int i = 0; i < NREQ; i++) {
        memset(&cb[i], 0, sizeof cb[i]);
        cb[i].aio_fildes = fd;
        cb[i].aio_buf = dst + (size_t)i * REQ_LEN; /* buffer is the kernel's (well, glibc's) until done */
        cb[i].aio_nbytes = REQ_LEN;
        cb[i].aio_offset = (off_t)i * REQ_LEN;
        cb[i].aio_sigevent.sigev_notify = SIGEV_NONE; /* we poll/suspend; no signal, no thread callback */
        CHECK(aio_read(&cb[i]) == 0); /* returns immediately */
    }
    int threads_during = count_threads();
    printf("1) %d aio_read() of %u KiB submitted; threads: %d before, %d right after submit\n",
           NREQ, REQ_LEN >> 10, threads_before, threads_during);

    /* Wait for all of them. aio_suspend returns when at least one in the
     * list is done (or timeout/EINTR); completed entries may be set to NULL
     * so later calls skip them. */
    const struct aiocb *list[NREQ];
    for (int i = 0; i < NREQ; i++)
        list[i] = &cb[i];
    int done = 0, suspends = 0;
    while (done < NREQ) {
        struct timespec to = {2, 0}; /* never hang */
        int r = aio_suspend(list, NREQ, &to);
        CHECK(r == 0 || errno == EINTR);
        suspends++;
        for (int i = 0; i < NREQ; i++) {
            if (!list[i])
                continue;
            int e = aio_error(&cb[i]);
            if (e == EINPROGRESS)
                continue;
            CHECK(e == 0);
            ssize_t n = aio_return(&cb[i]); /* exactly once, frees glibc's state */
            CHECK(n == (ssize_t)REQ_LEN);
            list[i] = NULL;
            done++;
        }
    }
    CHECK(memcmp(dst, src, FSIZE) == 0);
    printf("   all %d completed after %d aio_suspend() calls; data verified\n", NREQ, suspends);

    /* ---- 2. lio_listio: a batch of writes in ONE call, LIO_WAIT ---- */
    int out = tmpfile_fd();
    static struct aiocb wcb[NREQ];
    struct aiocb *wl[NREQ];
    for (int i = 0; i < NREQ; i++) {
        memset(&wcb[i], 0, sizeof wcb[i]);
        wcb[i].aio_fildes = out;
        wcb[i].aio_lio_opcode = LIO_WRITE;
        wcb[i].aio_buf = src + (size_t)i * REQ_LEN;
        wcb[i].aio_nbytes = REQ_LEN;
        wcb[i].aio_offset = (off_t)i * REQ_LEN;
        wl[i] = &wcb[i];
    }
    CHECK(lio_listio(LIO_WAIT, wl, NREQ, NULL) == 0); /* blocks until all are done */
    for (int i = 0; i < NREQ; i++) {
        CHECK(aio_error(&wcb[i]) == 0);
        CHECK(aio_return(&wcb[i]) == (ssize_t)REQ_LEN);
    }
    memset(dst, 0, FSIZE);
    CHECK(pread(out, dst, FSIZE, 0) == (ssize_t)FSIZE);
    CHECK(memcmp(dst, src, FSIZE) == 0);
    printf("2) lio_listio(LIO_WAIT, %d writes): done and verified\n", NREQ);

    /* aio_fsync: an asynchronous fsync, completes like any other request */
    static struct aiocb scb;
    memset(&scb, 0, sizeof scb);
    scb.aio_fildes = out;
    CHECK(aio_fsync(O_SYNC, &scb) == 0);
    const struct aiocb *sl[1] = {&scb};
    while (aio_error(&scb) == EINPROGRESS) {
        struct timespec to = {2, 0};
        CHECK(aio_suspend(sl, 1, &to) == 0 || errno == EINTR);
    }
    CHECK(aio_error(&scb) == 0 && aio_return(&scb) == 0);
    printf("3) aio_fsync(O_SYNC): done\n");
    printf("   glibc ran these on helper threads (idle ones exit after ~1 s; tune with\n"
           "   aio_init()). For real kernel async I/O use io_uring (c/07).\n");

    free(dst);
    free(src);
    close(out);
    close(fd);
    printf("06_posix_aio: all checks passed\n");
    return 0;
}
