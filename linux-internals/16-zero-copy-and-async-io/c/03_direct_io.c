/*
 * 03_direct_io.c — buffered vs direct I/O: O_DIRECT alignment rules (asked
 *                  with statx STATX_DIOALIGN), posix_memalign, page-cache
 *                  residency with mincore, and the EINVAL fallback
 *
 * Concept : Normal ("buffered") I/O goes through the PAGE CACHE: read()
 *           copies from cached pages, write() copies into them and returns;
 *           the kernel writes back later. O_DIRECT bypasses the cache: the
 *           device DMAs straight to/from YOUR buffer. That avoids one copy
 *           and does not evict other data from the cache, but every request
 *           now hits the device, and the buffer address, file offset and
 *           length must be aligned (usually to the logical block size,
 *           512 or 4096). Misaligned -> EINVAL. Some filesystems (tmpfs
 *           before 6.6, some FUSE) reject O_DIRECT at open() — the program
 *           then explains and skips. Databases (PostgreSQL optional, MySQL
 *           InnoDB, ScyllaDB) and video recorders writing huge streams use
 *           O_DIRECT so their own cache is the only one.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/03_direct_io.c
 * Read    : ../README.md  (section 3.4 "Buffered vs direct I/O")
 */
#define _GNU_SOURCE /* O_DIRECT, statx, mkdtemp */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define FILE_SIZE (4u << 20)
#ifndef STATX_DIOALIGN
#define STATX_DIOALIGN 0x00002000U /* Linux 6.1; not in glibc 2.35 headers */
#endif

static char dir[64], path_b[128], path_d[128];

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

static void cleanup(void)
{
    unlink(path_b);
    unlink(path_d);
    rmdir(dir);
}

/* Ask the kernel for the O_DIRECT alignment rules of this file (6.1+).
 * Ubuntu 22.04's headers (glibc 2.35, linux-libc-dev 5.15) have no named
 * fields for them yet. The kernel ABI puts them right after stx_mnt_id:
 *   0x90 u64 stx_mnt_id | 0x98 u32 stx_dio_mem_align | 0x9c u32 stx_dio_offset_align
 * struct statx is 256 bytes in every version, so reading by offset is safe. */
static int dio_align(const char *path, uint32_t *mem_align, uint32_t *off_align)
{
    struct statx stx;
    memset(&stx, 0, sizeof stx);
    if (statx(AT_FDCWD, path, 0, STATX_DIOALIGN, &stx) != 0 || !(stx.stx_mask & STATX_DIOALIGN))
        return -1;
    _Static_assert(sizeof stx == 256, "struct statx ABI size");
    memcpy(mem_align, (const char *)&stx + 0x98, 4);
    memcpy(off_align, (const char *)&stx + 0x9c, 4);
    return 0;
}

/* How many pages of the file are in the page cache right now? mincore()
 * on a mapping answers per page (bit 0 = resident). Mapping does not fault
 * pages in; only touching would. */
static size_t resident_pages(const char *path, size_t *total)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    long pg = sysconf(_SC_PAGESIZE);
    *total = (FILE_SIZE + (size_t)pg - 1) / (size_t)pg;
    void *p = mmap(NULL, FILE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    unsigned char *vec = malloc(*total);
    CHECK(vec);
    CHECK(mincore(p, FILE_SIZE, vec) == 0);
    size_t n = 0;
    for (size_t i = 0; i < *total; i++)
        n += vec[i] & 1;
    free(vec);
    CHECK(munmap(p, FILE_SIZE) == 0);
    close(fd);
    return n;
}

static void fill(unsigned char *b, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        b[i] = (unsigned char)((i * 131u + seed) >> 3);
}

int main(void)
{
    alarm(30);
    snprintf(dir, sizeof dir, "/tmp/zc03-%d-XXXXXX", (int)getpid());
    CHECK(mkdtemp(dir) != NULL);
    snprintf(path_b, sizeof path_b, "%s/buffered.bin", dir);
    snprintf(path_d, sizeof path_d, "%s/direct.bin", dir);
    atexit(cleanup);

    /* O_DIRECT buffer: aligned to 4096 (covers 512-byte and 4K devices).
     * malloc gives 16-byte alignment only. posix_memalign / aligned_alloc
     * are the portable ways; free() releases it normally. */
    unsigned char *buf;
    CHECK(posix_memalign((void **)&buf, 4096, FILE_SIZE + 4096) == 0);
    fill(buf, FILE_SIZE, 7);

    /* ---- 1. a buffered write: returns as soon as the page cache has it ---- */
    int fd = open(path_b, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    double t0 = now_ms();
    CHECK(write(fd, buf, FILE_SIZE) == (ssize_t)FILE_SIZE);
    double t_wr = now_ms() - t0;
    t0 = now_ms();
    CHECK(fsync(fd) == 0); /* NOW it is on the device */
    double t_sync = now_ms() - t0;
    CHECK(close(fd) == 0);
    size_t total, res = resident_pages(path_b, &total);
    printf("1) buffered write of %u MiB: write() %.2f ms, fsync() %.2f ms;\n"
           "   %zu of %zu pages are in the page cache afterwards\n",
           FILE_SIZE >> 20, t_wr, t_sync, res, total);
    CHECK(res > 0); /* just written: cached (unless under heavy memory pressure) */

    /* Drop them (clean pages only: that is why we fsync'ed first). */
    fd = open(path_b, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED) == 0);
    close(fd);
    printf("   after posix_fadvise(DONTNEED): %zu pages resident\n", resident_pages(path_b, &total));

    /* ---- 2. the alignment rules for THIS file ---- */
    uint32_t mem_al = 0, off_al = 0;
    if (dio_align(path_b, &mem_al, &off_al) == 0)
        printf("2) statx(STATX_DIOALIGN): buffer alignment %u, offset/length alignment %u%s\n",
               mem_al, off_al, mem_al == 0 ? " (0 = O_DIRECT not supported here)" : "");
    else
        printf("2) statx(STATX_DIOALIGN) unsupported (kernel < 6.1): assume 4096 to be safe\n");

    /* ---- 3. open with O_DIRECT; some filesystems refuse ---- */
    int dfd = open(path_b, O_RDONLY | O_DIRECT | O_CLOEXEC);
    if (dfd < 0) {
        CHECK(errno == EINVAL);
        printf("3) open(O_DIRECT) -> EINVAL: this filesystem does not support direct I/O\n"
               "   (tmpfs before 6.6, some FUSE/overlay setups). Falling back to buffered\n"
               "   I/O; on the board's ext4 SD card/eMMC it works.\n");
        free(buf);
        printf("03_direct_io: all checks passed\n");
        return 0;
    }

    /* misaligned BUFFER: EINVAL (when the fs reports a real alignment) */
    errno = 0;
    ssize_t r = pread(dfd, buf + 1, 4096, 0);
    printf("3) O_DIRECT pread, buffer at +1 byte  -> %s\n", r < 0 ? strerror(errno) : "ok?!");
    if (mem_al > 1)
        CHECK(r < 0 && errno == EINVAL);

    /* misaligned OFFSET */
    errno = 0;
    r = pread(dfd, buf, 4096, 100);
    printf("   O_DIRECT pread, file offset 100      -> %s\n", r < 0 ? strerror(errno) : "ok?!");
    if (off_al > 1)
        CHECK(r < 0 && errno == EINVAL);

    /* aligned everything: works, and the data is right */
    unsigned char *chk = malloc(FILE_SIZE);
    CHECK(chk);
    fill(chk, FILE_SIZE, 7);
    memset(buf, 0, FILE_SIZE);
    t0 = now_ms();
    size_t got = 0;
    while (got < FILE_SIZE) {
        r = pread(dfd, buf + got, 1u << 20, (off_t)got); /* 1 MiB requests */
        CHECK(r > 0);
        got += (size_t)r;
    }
    double t_dio = now_ms() - t0;
    CHECK(memcmp(buf, chk, FILE_SIZE) == 0);
    close(dfd);
    printf("   aligned O_DIRECT read of %u MiB: %.2f ms (straight from the device)\n",
           FILE_SIZE >> 20, t_dio);
    printf("   pages resident after the O_DIRECT read: %zu (the cache was bypassed)\n",
           resident_pages(path_b, &total));

    /* ---- 4. buffered reads: cold (from device into cache) then warm ---- */
    for (int pass = 0; pass < 2; pass++) {
        fd = open(path_b, O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        t0 = now_ms();
        got = 0;
        while ((r = read(fd, buf, 1u << 20)) > 0)
            got += (size_t)r;
        double t = now_ms() - t0;
        close(fd);
        CHECK(got == FILE_SIZE);
        printf("4) buffered read pass %d: %.2f ms (%s)\n", pass + 1, t,
               pass == 0 ? "cold: device -> page cache -> buffer" : "warm: page cache -> buffer, no I/O");
    }

    /* ---- 5. O_DIRECT write: aligned buffer, length multiple of 4096 ---- */
    dfd = open(path_d, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT | O_CLOEXEC, 0600);
    CHECK(dfd >= 0);
    fill(buf, FILE_SIZE, 9);
    t0 = now_ms();
    CHECK(write(dfd, buf, FILE_SIZE) == (ssize_t)FILE_SIZE);
    CHECK(fdatasync(dfd) == 0); /* still needed: device cache + metadata */
    double t_dw = now_ms() - t0;
    close(dfd);
    printf("5) O_DIRECT write + fdatasync: %.2f ms; %zu pages of it in the page cache\n", t_dw,
           resident_pages(path_d, &total));

    free(chk);
    free(buf);
    printf("03_direct_io: all checks passed\n");
    return 0;
}
