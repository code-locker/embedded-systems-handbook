/*
 * 04_mmap_vs_read.c — reading files with mmap vs read(): page faults,
 *                     MAP_POPULATE, madvise, and why mmap loses on many
 *                     small files
 *
 * Concept : mmap() maps the page cache pages of a file into your address
 *           space: no copy into a user buffer, the data is simply there.
 *           But "there" is lazy: the first touch of each page is a page
 *           fault (the kernel maps up to 16 already-cached neighbours at
 *           once — "fault-around"), and setting up/tearing down a mapping
 *           costs mmap + munmap + a TLB flush. For one big file read many
 *           times or randomly, mmap wins. For many small files read once,
 *           read() into one reused buffer wins: one syscall, one copy of
 *           a few KiB, no VMA, no faults. Minor-fault counts from
 *           getrusage() make the difference visible; timings are printed,
 *           checksums asserted.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/04_mmap_vs_read.c
 * Read    : ../README.md  (section 3.5 "mmap for file I/O, and when it loses")
 */
#define _GNU_SOURCE /* MAP_POPULATE, mkdtemp */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
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

#define BIG_SIZE   (8u << 20) /* one 8 MiB file */
#define SMALL_N    300        /* and 300 files ... */
#define SMALL_SIZE 2000       /* ... of 2000 bytes */

static char dir[64];

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static long minflt(void)
{
    struct rusage ru;
    CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
    return ru.ru_minflt;
}

/* Sum of 8-byte words: cheap, order-independent is fine here (we compare
 * the same file read two ways), and it touches every page. */
static uint64_t sum_buf(const unsigned char *p, size_t n)
{
    uint64_t s = 0;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        s += w;
    }
    for (; i < n; i++)
        s += p[i];
    return s;
}

static void write_file(const char *path, size_t size, unsigned seed)
{
    unsigned char *b = malloc(size);
    CHECK(b);
    for (size_t i = 0; i < size; i++)
        b[i] = (unsigned char)(i * 7u + seed);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(write(fd, b, size) == (ssize_t)size);
    CHECK(close(fd) == 0);
    free(b);
}

struct result {
    double ms;
    long faults;
    uint64_t sum;
};

static struct result big_read(const char *path)
{
    static unsigned char buf[64 << 10]; /* one reused buffer: stays in L2 */
    struct result r = {0};
    long f0 = minflt();
    double t0 = now_ms();
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        r.sum += sum_buf(buf, (size_t)n); /* 8 KiB-aligned chunks: word sums add up */
    CHECK(n == 0);
    close(fd);
    r.ms = now_ms() - t0;
    r.faults = minflt() - f0;
    return r;
}

static struct result big_mmap(const char *path, int flags, int advice)
{
    struct result r = {0};
    long f0 = minflt();
    double t0 = now_ms();
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    unsigned char *p = mmap(NULL, BIG_SIZE, PROT_READ, MAP_PRIVATE | flags, fd, 0);
    CHECK(p != MAP_FAILED);
    close(fd); /* the mapping keeps the file referenced */
    if (advice >= 0)
        CHECK(madvise(p, BIG_SIZE, advice) == 0);
    r.sum = sum_buf(p, BIG_SIZE);
    CHECK(munmap(p, BIG_SIZE) == 0);
    r.ms = now_ms() - t0;
    r.faults = minflt() - f0;
    return r;
}

int main(void)
{
    alarm(30);
    snprintf(dir, sizeof dir, "/tmp/zc04-%d-XXXXXX", (int)getpid());
    CHECK(mkdtemp(dir) != NULL);
    char big[128];
    snprintf(big, sizeof big, "%s/big.bin", dir);
    write_file(big, BIG_SIZE, 3);
    long pg = sysconf(_SC_PAGESIZE);

    /* ---- 1. one big file, page cache warm ---- */
    printf("1) one %u MiB file (%ld pages of %ld B), page cache warm:\n", BIG_SIZE >> 20,
           (long)BIG_SIZE / pg, pg);
    struct result a = big_read(big);
    struct result b = big_mmap(big, 0, -1);
    struct result c = big_mmap(big, MAP_POPULATE, -1);
    struct result d = big_mmap(big, 0, MADV_SEQUENTIAL);
    printf("   %-28s %8s %13s\n", "method", "ms", "minor faults");
    printf("   %-28s %8.2f %13ld\n", "read() 64 KiB buffer", a.ms, a.faults);
    printf("   %-28s %8.2f %13ld   (fault-around maps ~16 pages per fault)\n", "mmap, lazy", b.ms,
           b.faults);
    printf("   %-28s %8.2f %13ld   (same faults, taken INSIDE mmap(), before\n"
           "%59s your loop: good for RT)\n", "mmap + MAP_POPULATE", c.ms, c.faults, "");
    printf("   %-28s %8.2f %13ld\n", "mmap + MADV_SEQUENTIAL", d.ms, d.faults);
    printf("   (under ASan/TSan the counts include shadow-memory faults; build with\n"
           "    SAN=0 for the clean numbers: ~16 / 128 / 128 / 128 on x86-64)\n");
    CHECK(a.sum == b.sum && b.sum == c.sum && c.sum == d.sum);
    /* Not asserted as a ratio, but always true: touching an unpopulated
     * mapping faults at least once, and MAP_POPULATE pre-maps instead. */
    CHECK(b.faults >= 1);

    /* ---- 2. many small files: per-file setup cost dominates ---- */
    char path[160];
    for (int i = 0; i < SMALL_N; i++) {
        snprintf(path, sizeof path, "%s/s%03d", dir, i);
        write_file(path, SMALL_SIZE, (unsigned)i);
    }
    static unsigned char sbuf[4096];
    uint64_t s_read = 0, s_map = 0;
    long f0 = minflt();
    double t0 = now_ms();
    for (int i = 0; i < SMALL_N; i++) {
        snprintf(path, sizeof path, "%s/s%03d", dir, i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        ssize_t n = read(fd, sbuf, sizeof sbuf); /* 1 syscall, ~2 KB copied */
        CHECK(n == SMALL_SIZE);
        s_read += sum_buf(sbuf, (size_t)n);
        close(fd);
    }
    double t_read = now_ms() - t0;
    long f_read = minflt() - f0;

    f0 = minflt();
    t0 = now_ms();
    for (int i = 0; i < SMALL_N; i++) {
        snprintf(path, sizeof path, "%s/s%03d", dir, i);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        struct stat st;
        CHECK(fstat(fd, &st) == 0); /* mmap needs the size first: +1 syscall */
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        CHECK(p != MAP_FAILED);
        s_map += sum_buf(p, (size_t)st.st_size); /* page fault on first touch */
        CHECK(munmap(p, (size_t)st.st_size) == 0); /* VMA teardown + TLB flush */
        close(fd);
    }
    double t_map = now_ms() - t0;
    long f_map = minflt() - f0;
    printf("2) %d files of %d B each:\n", SMALL_N, SMALL_SIZE);
    printf("   open+read+close             %8.2f ms %8ld faults  (3 syscalls/file)\n", t_read, f_read);
    printf("   open+fstat+mmap+munmap+close %7.2f ms %8ld faults  (5 syscalls + 1 fault/file)\n",
           t_map, f_map);
    CHECK(s_read == s_map);

    /* ---- cleanup ---- */
    for (int i = 0; i < SMALL_N; i++) {
        snprintf(path, sizeof path, "%s/s%03d", dir, i);
        CHECK(unlink(path) == 0);
    }
    CHECK(unlink(big) == 0);
    CHECK(rmdir(dir) == 0);
    printf("   Rule of thumb: mmap for big files accessed randomly or repeatedly\n"
           "   (and shared between processes); read() for streaming and small files.\n"
           "   And beware: if another process truncates a mapped file, touching the\n"
           "   lost pages raises SIGBUS.\n");
    printf("04_mmap_vs_read: all checks passed\n");
    return 0;
}
