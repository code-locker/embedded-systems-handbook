/*
 * 01_copy_file_to_file.c — five ways to copy a file: read/write, mmap+write,
 *                          sendfile, copy_file_range, splice through a pipe;
 *                          timings printed, content checksums asserted
 *
 * Concept : read()+write() moves every byte across the user/kernel
 *           boundary TWICE (page cache -> user buffer -> page cache) and
 *           costs two syscalls per chunk. The "zero-copy" calls keep the
 *           data inside the kernel: sendfile() and splice() hand page
 *           references from one file to another, copy_file_range() lets the
 *           filesystem do it (on btrfs/XFS it can even share the blocks:
 *           a reflink, no data copied at all). mmap()+write() removes one
 *           copy but adds page faults. The numbers depend on the machine
 *           and page-cache state, so they are printed; what is asserted is
 *           that every method produced byte-identical output.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/01_copy_file_to_file.c
 * Read    : ../README.md  (section 3.2 "Copying a file: five ways")
 */
#define _GNU_SOURCE /* copy_file_range, splice, mkdtemp */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
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

#define FILE_SIZE (8u << 20) /* 8 MiB: big enough to measure, fast under TSan */
#define CHUNK     (64u << 10)

static char dir[64];

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* 64-bit FNV-1a over 8-byte words: fast enough at -O0 under sanitizers,
 * and any changed, missing or reordered word changes the result. */
static uint64_t checksum_buf(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 1099511628211ULL;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

static uint64_t checksum_file(const char *path, size_t *size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    *size = (size_t)st.st_size;
    uint64_t h = 0;
    if (st.st_size > 0) {
        void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        CHECK(p != MAP_FAILED);
        h = checksum_buf(p, (size_t)st.st_size);
        CHECK(munmap(p, (size_t)st.st_size) == 0);
    }
    close(fd);
    return h;
}

static void make_source(const char *path)
{
    /* xorshift pseudo-random content: incompressible, deterministic */
    unsigned char *buf = malloc(CHUNK);
    CHECK(buf);
    uint64_t x = 0x9E3779B97F4A7C15ULL;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    for (size_t off = 0; off < FILE_SIZE; off += CHUNK) {
        for (size_t i = 0; i < CHUNK; i += 8) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            memcpy(buf + i, &x, 8);
        }
        CHECK(write(fd, buf, CHUNK) == (ssize_t)CHUNK);
    }
    CHECK(close(fd) == 0);
    free(buf);
}

/* ---- the five methods; each copies `in` (at offset 0) to `out` ---- */

/* 1. read/write:  disk/page cache --copy--> user buf --copy--> page cache */
static int copy_read_write(int in, int out)
{
    static unsigned char buf[CHUNK];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t done = 0;
        while (done < n) { /* write may be short: loop until all written */
            ssize_t w = write(out, buf + done, (size_t)(n - done));
            if (w < 0)
                return -1;
            done += w;
        }
    }
    return n < 0 ? -1 : 0;
}

/* 2. mmap+write: the source pages are mapped (no copy into a buffer), but
 * write() still copies them into the destination's page cache, and every
 * first touch of a source page is a page fault. */
static int copy_mmap_write(int in, int out)
{
    struct stat st;
    CHECK(fstat(in, &st) == 0);
    unsigned char *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, in, 0);
    if (p == MAP_FAILED)
        return -1;
    madvise(p, (size_t)st.st_size, MADV_SEQUENTIAL); /* read-ahead hint */
    size_t done = 0;
    while (done < (size_t)st.st_size) {
        ssize_t w = write(out, p + done, (size_t)st.st_size - done);
        if (w < 0) {
            munmap(p, (size_t)st.st_size);
            return -1;
        }
        done += (size_t)w;
    }
    return munmap(p, (size_t)st.st_size);
}

/* 3. sendfile: in-kernel, page cache -> page cache. out_fd may be any file
 * since 2.6.33 (it was socket-only before). Moves at most ~2 GiB per call. */
static int copy_sendfile(int in, int out)
{
    off_t off = 0;
    struct stat st;
    CHECK(fstat(in, &st) == 0);
    while (off < st.st_size) {
        ssize_t n = sendfile(out, in, &off, (size_t)(st.st_size - off));
        if (n <= 0)
            return -1;
    }
    return 0;
}

/* 4. copy_file_range: "filesystem, copy this range". Same-fs on btrfs/XFS
 * (reflink) or NFS 4.2 (server-side copy): no data moves at all. On ext4 it
 * is an in-kernel splice. Falls back with EXDEV/EOPNOTSUPP on old kernels
 * across filesystems — then use sendfile. */
static int copy_cfr(int in, int out)
{
    struct stat st;
    CHECK(fstat(in, &st) == 0);
    off_t remaining = st.st_size;
    while (remaining > 0) {
        ssize_t n = copy_file_range(in, NULL, out, NULL, (size_t)remaining, 0);
        if (n <= 0)
            return -1;
        remaining -= n;
    }
    return 0;
}

/* 5. splice: file -> pipe -> file. One end of every splice must be a pipe;
 * the pipe holds page REFERENCES, not copies (lesson 09 section 3.8). */
static int copy_splice(int in, int out)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    fcntl(p[1], F_SETPIPE_SZ, 1 << 20); /* bigger pipe = fewer syscalls; may be capped */
    int rc = 0;
    for (;;) {
        ssize_t n = splice(in, NULL, p[1], NULL, 1 << 20, SPLICE_F_MOVE | SPLICE_F_MORE);
        if (n == 0)
            break; /* EOF */
        if (n < 0) {
            rc = -1;
            break;
        }
        while (n > 0) { /* drain everything we put in */
            ssize_t m = splice(p[0], NULL, out, NULL, (size_t)n, SPLICE_F_MOVE | SPLICE_F_MORE);
            if (m <= 0) {
                rc = -1;
                goto out;
            }
            n -= m;
        }
    }
out:
    close(p[0]);
    close(p[1]);
    return rc;
}

struct method {
    const char *name;
    int (*fn)(int, int);
    const char *copies;
};

int main(void)
{
    alarm(30);
    snprintf(dir, sizeof dir, "/tmp/zc01-%d-XXXXXX", (int)getpid());
    CHECK(mkdtemp(dir) != NULL);
    char src[128], dst[128];
    snprintf(src, sizeof src, "%s/src.bin", dir);
    snprintf(dst, sizeof dst, "%s/dst.bin", dir);

    make_source(src);
    size_t sz;
    uint64_t want = checksum_file(src, &sz);
    CHECK(sz == FILE_SIZE);
    printf("source: %u MiB of pseudo-random data in %s (page cache warm), checksum %016llx\n",
           FILE_SIZE >> 20, dir, (unsigned long long)want);

    const struct method m[] = {
        {"read + write", copy_read_write, "2 CPU copies, 2 syscalls per 64 KiB"},
        {"mmap + write", copy_mmap_write, "1 CPU copy + page faults"},
        {"sendfile", copy_sendfile, "in kernel"},
        {"copy_file_range", copy_cfr, "in kernel / fs (reflink on btrfs, XFS)"},
        {"splice via pipe", copy_splice, "in kernel, page refs through a pipe"},
    };
    printf("   %-16s %9s %10s   %s\n", "method", "ms", "MiB/s", "data path");
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int in = open(src, O_RDONLY | O_CLOEXEC);
        int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        CHECK(in >= 0 && out >= 0);
        double t0 = now_ms();
        int rc = m[i].fn(in, out);
        double dt = now_ms() - t0;
        close(in);
        CHECK(close(out) == 0);
        if (rc != 0) {
            /* copy_file_range across filesystems on old kernels, or a
             * filesystem without splice support: explain and move on. */
            printf("   %-16s  unsupported here (%s) -> fall back to sendfile\n", m[i].name,
                   strerror(errno));
            continue;
        }
        uint64_t got = checksum_file(dst, &sz);
        printf("   %-16s %9.2f %10.0f   %s\n", m[i].name, dt, (FILE_SIZE >> 20) / (dt / 1e3),
               m[i].copies);
        CHECK(sz == FILE_SIZE && got == want); /* identical bytes, every method */
        CHECK(unlink(dst) == 0);
    }
    printf("   (writes go to the page cache; none of these timings include the disk.\n"
           "    Add fsync() to measure the device, see c/03.)\n");

    CHECK(unlink(src) == 0);
    CHECK(rmdir(dir) == 0);
    printf("01_copy_file_to_file: all checks passed\n");
    return 0;
}
