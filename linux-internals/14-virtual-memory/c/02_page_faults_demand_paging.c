/*
 * 02_page_faults_demand_paging.c — demand paging and lazy allocation
 *                                  measured: minor/major page faults with
 *                                  getrusage, RSS from /proc/self/statm,
 *                                  MAP_POPULATE, and copy-on-write after fork
 *
 * Concept : mmap() and a large malloc() only reserve ADDRESS SPACE. No RAM
 *           is used until the first access to each page: the MMU finds no
 *           translation, raises a PAGE FAULT, and the kernel allocates a
 *           zeroed frame and maps it. That is a MINOR fault (no disk I/O).
 *           A MAJOR fault has to read the page from storage (a file mapping
 *           not in the page cache, or swap). After fork(), parent and child
 *           share every page read-only; the first WRITE to a page in either
 *           process faults and copies just that page (copy-on-write).
 *           RSS (resident set size) counts the pages actually in RAM.
 * Build   : make run P=linux-internals/14-virtual-memory/c/02_page_faults_demand_paging.c
 * Read    : ../README.md  (sections "3.4 Page faults" and "3.5 Demand
 *           paging, lazy allocation, copy-on-write")
 */
#define _GNU_SOURCE /* MAP_ANONYMOUS, MAP_POPULATE, MADV_NOHUGEPAGE */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
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

static long g_page;

struct faults {
    long minflt, majflt;
};

static struct faults faults_now(void)
{
    struct rusage ru;
    CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
    return (struct faults){ru.ru_minflt, ru.ru_majflt};
}

/* RSS in KiB from /proc/self/statm (fields are in pages:
 * size resident shared text lib data dt). */
static long rss_kib(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    CHECK(f != NULL);
    long size = 0, res = 0;
    CHECK(fscanf(f, "%ld %ld", &size, &res) == 2);
    fclose(f);
    return res * (g_page / 1024);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* Write one byte per page: the cheapest way to fault every page in. */
static void touch(char *p, size_t len)
{
    for (size_t off = 0; off < len; off += (size_t)g_page)
        p[off] = 1;
}

/* Fresh anonymous mapping on 4 KiB pages only: with transparent huge pages
 * one fault could map 512 pages and the counts below would be different
 * (still correct, just not 1 fault per page). */
static char *map_anon(size_t len, int extra_flags)
{
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | extra_flags, -1, 0);
    CHECK(p != MAP_FAILED);
    CHECK(madvise(p, len, MADV_NOHUGEPAGE) == 0);
    return p;
}

/* ---- 1. mmap reserves, touching allocates ----
 *
 *   after mmap:     VMA [==================== 32 MiB ====================]
 *                   page table: (nothing)          RSS +0
 *   after touching: every access -> #PF -> alloc zeroed frame -> map
 *                   page table: [x][x][x][x]...    RSS +32 MiB
 */
static void demo_demand_paging(void)
{
    const size_t len = 32u << 20;
    const long npages = (long)(len / (size_t)g_page);

    long rss0 = rss_kib();
    char *p = map_anon(len, 0);
    long rss1 = rss_kib();
    struct faults f0 = faults_now();
    double t0 = now_ms();
    touch(p, len);
    double t_touch = now_ms() - t0;
    struct faults f1 = faults_now();
    long rss2 = rss_kib();

    long minflt = f1.minflt - f0.minflt;
    printf("1) mmap 32 MiB: RSS %+ld KiB; touching %ld pages: %ld minor "
           "faults, %ld major, RSS %+ld KiB, %.2f ms (%.0f ns/fault)\n",
           rss1 - rss0, npages, minflt, f1.majflt - f0.majflt, rss2 - rss1,
           t_touch, t_touch * 1e6 / (double)npages);
    CHECK(rss1 - rss0 < 1024);          /* mmap alone: (almost) no RAM */
    CHECK(minflt >= npages);            /* >= : sanitizers add their own */
    CHECK(rss2 - rss1 >= (long)(len / 1024) * 9 / 10);

    /* Touching again costs nothing: the translations exist now. */
    f0 = faults_now();
    touch(p, len);
    f1 = faults_now();
    printf("   second pass over the same pages: %ld minor faults\n",
           f1.minflt - f0.minflt);
    CHECK(f1.minflt - f0.minflt < npages / 10);
    CHECK(munmap(p, len) == 0);
}

/* ---- 2. a big malloc() is lazy too ---- */
static void demo_lazy_malloc(void)
{
    const size_t len = 32u << 20;
    long rss0 = rss_kib();
    /* glibc serves requests >= 128 KiB (M_MMAP_THRESHOLD) with a private
     * mmap; ASan/TSan use their own allocators, which also mmap. Either
     * way: address space now, RAM later. */
    char *p = malloc(len); /* owned here, freed below */
    CHECK(p != NULL);
    long rss1 = rss_kib();
    memset(p, 0x5A, len / 2); /* touch half */
    long rss2 = rss_kib();
    printf("2) malloc(32 MiB): RSS %+ld KiB; after memset of half: RSS "
           "%+ld KiB more\n", rss1 - rss0, rss2 - rss1);
    CHECK(rss1 - rss0 < 4096);
    CHECK(rss2 - rss1 >= (long)(len / 2 / 1024) * 9 / 10);
    free(p);
    printf("   (calloc of a fresh mmap'd block skips the memset: the kernel "
           "already gives zeroed pages)\n");
}

/* ---- 3. MAP_POPULATE: pay the faults up front ---- */
static void demo_populate(void)
{
    const size_t len = 16u << 20;
    const long npages = (long)(len / (size_t)g_page);

    char *lazy = map_anon(len, 0);
    struct faults a0 = faults_now();
    double t0 = now_ms();
    touch(lazy, len);
    double t_lazy = now_ms() - t0;
    struct faults a1 = faults_now();

    t0 = now_ms();
    struct faults m0 = faults_now();
    char *pop = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    CHECK(pop != MAP_FAILED);
    struct faults m1 = faults_now();
    double t_map = now_ms() - t0;
    struct faults b0 = faults_now();
    t0 = now_ms();
    touch(pop, len);
    double t_pop = now_ms() - t0;
    struct faults b1 = faults_now();

    long lazy_f = a1.minflt - a0.minflt, pop_f = b1.minflt - b0.minflt;
    printf("3) 16 MiB lazy  : touch = %ld faults, %.2f ms\n", lazy_f, t_lazy);
    printf("   MAP_POPULATE : mmap() %.2f ms (%ld faults counted), then "
           "touch = %ld faults, %.2f ms\n", t_map, m1.minflt - m0.minflt,
           pop_f, t_pop);
    /* The populated mapping must take at least npages/2 fewer faults on
     * first touch (sanitizer shadow faults hit both sides equally). */
    CHECK(pop_f + npages / 2 <= lazy_f);
    printf("   -> real-time code: populate (or mlock, see 04) BEFORE the "
           "control loop starts\n");
    CHECK(munmap(lazy, len) == 0);
    CHECK(munmap(pop, len) == 0);
}

/* ---- 4. major faults: file pages not in the page cache ---- */
static void demo_major(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/vm_major_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    const size_t len = 4u << 20;
    char *buf = malloc(len); /* owned here */
    CHECK(buf != NULL);
    memset(buf, 'M', len);
    CHECK(write(fd, buf, len) == (ssize_t)len);
    free(buf);
    CHECK(fsync(fd) == 0); /* clean pages can be dropped ... */
    /* ... and this asks the kernel to drop them from the page cache. */
    CHECK(posix_fadvise(fd, 0, (off_t)len, POSIX_FADV_DONTNEED) == 0);

    char *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(m != MAP_FAILED);
    struct faults f0 = faults_now();
    unsigned sum = 0;
    for (size_t off = 0; off < len; off += (size_t)g_page)
        sum += (unsigned char)m[off];
    struct faults f1 = faults_now();
    CHECK(sum == (len / (size_t)g_page) * 'M');
    printf("4) read a 4 MiB file mapping after FADV_DONTNEED: %ld major, "
           "%ld minor faults (printed: tmpfs or readahead change this)\n",
           f1.majflt - f0.majflt, f1.minflt - f0.minflt);
    CHECK(munmap(m, len) == 0);
    close(fd);
    CHECK(unlink(path) == 0);
}

/* ---- 5. copy-on-write after fork(), measured in the child ----
 *
 *  after fork:    parent PTE -> [frame] <- child PTE   (both read-only)
 *  child writes:  child PTE -> [copy]   parent PTE -> [frame] (writable)
 *                 = one minor fault + one 4 KiB copy per page written
 */
static void demo_cow(void)
{
    const size_t len = 8u << 20;
    const long npages = (long)(len / (size_t)g_page);
    char *p = map_anon(len, 0);
    memset(p, 'P', len); /* parent owns every page */

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(pipefd[0]);
        long r[3];
        struct faults c0 = faults_now();
        unsigned sum = 0;
        for (size_t off = 0; off < len; off += (size_t)g_page)
            sum += (unsigned char)p[off]; /* READ: shared, no copy */
        struct faults c1 = faults_now();
        touch(p, len);                    /* WRITE: copy each page */
        struct faults c2 = faults_now();
        r[0] = c1.minflt - c0.minflt;
        r[1] = c2.minflt - c1.minflt;
        r[2] = sum == (unsigned)npages * 'P';
        ssize_t w = write(pipefd[1], r, sizeof r);
        _exit(w == (ssize_t)sizeof r ? 0 : 1);
    }
    close(pipefd[1]);
    long r[3] = {0, 0, 0};
    CHECK(read(pipefd[0], r, sizeof r) == (ssize_t)sizeof r);
    close(pipefd[0]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    printf("5) child after fork, %ld pages: reading them = %ld faults, "
           "writing them = %ld faults (copy-on-write)\n", npages, r[0], r[1]);
    if (r[0] > npages / 2)
        printf("   (reads faulted too: TSan WRITES its shadow memory "
               "on every read, and those shadow pages are COW)\n");
    CHECK(r[2] == 1);
    CHECK(r[1] >= npages); /* one COW fault per written page */
    CHECK(p[0] == 'P' && p[len - 1] == 'P'); /* parent's copy unchanged */
    printf("   parent still sees its own data: p[0] = '%c'\n", p[0]);
    CHECK(munmap(p, len) == 0);
}

int main(void)
{
    alarm(30);
    g_page = sysconf(_SC_PAGESIZE);
    demo_demand_paging();
    demo_lazy_malloc();
    demo_populate();
    demo_major();
    demo_cow();
    printf("02_page_faults_demand_paging: all checks passed\n");
    return 0;
}
