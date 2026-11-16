/*
 * 04_madvise_mlock_overcommit.c — telling the kernel about your memory:
 *                                 madvise (DONTNEED, WILLNEED), mincore,
 *                                 mlock/mlockall for real-time code, and
 *                                 overcommit + the OOM killer's view of you
 *
 * Concept : madvise() is a hint (or an order) about a range: DONTNEED
 *           throws the pages away now (private anonymous memory reads back
 *           as ZEROS; file pages are just re-read from the file), WILLNEED
 *           starts read-ahead. mlock() pins pages in RAM so a real-time
 *           thread never takes a page fault (a major fault can cost
 *           milliseconds); it is limited by RLIMIT_MEMLOCK for normal users.
 *           Linux OVERCOMMITS: mmap of more memory than exists succeeds,
 *           because most of it is never touched. If too much is touched,
 *           the OOM killer picks a victim by oom_score.
 *           No real OOM happens here: we only read the knobs.
 * Build   : make run P=linux-internals/14-virtual-memory/c/04_madvise_mlock_overcommit.c
 * Read    : ../README.md  (sections "3.8 madvise", "3.9 mlock and real
 *           time" and "3.10 Overcommit and the OOM killer")
 */
#define _GNU_SOURCE /* MAP_ANONYMOUS, MAP_NORESERVE, madvise, mincore */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
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

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define UNDER_SANITIZER 1
#else
#define UNDER_SANITIZER 0
#endif

static long g_page;

/* ASan and TSan INTERCEPT mlock()/mlockall() and turn them into no-ops
 * that return 0 (locking would pin their huge shadow mappings). Calling the
 * system call directly bypasses the interceptor, so the demo really locks
 * under every build. In normal code just call mlock(). */
static int do_mlock(void *p, size_t len)
{
#if UNDER_SANITIZER
    return (int)syscall(SYS_mlock, p, len);
#else
    return mlock(p, len);
#endif
}

static int do_munlock(void *p, size_t len)
{
#if UNDER_SANITIZER
    return (int)syscall(SYS_munlock, p, len);
#else
    return munlock(p, len);
#endif
}

static long rss_kib(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    CHECK(f != NULL);
    long size = 0, res = 0;
    CHECK(fscanf(f, "%ld %ld", &size, &res) == 2);
    fclose(f);
    return res * (g_page / 1024);
}

/* Read "Key:   123 kB" from a /proc file; -1 if missing. */
static long proc_kv(const char *file, const char *key)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    char line[256];
    long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':')
            v = strtol(line + kl + 1, NULL, 10);
    fclose(f);
    return v;
}

static long read_long_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

/* How many pages of [p, p+len) are resident (mincore: page cache / RAM)? */
static int resident_pages(void *p, size_t len)
{
    size_t n = (len + (size_t)g_page - 1) / (size_t)g_page;
    unsigned char *vec = malloc(n); /* owned here */
    CHECK(vec != NULL);
    CHECK(mincore(p, len, vec) == 0);
    int r = 0;
    for (size_t i = 0; i < n; i++)
        r += vec[i] & 1;
    free(vec);
    return r;
}

/* ---- 1. MADV_DONTNEED: anonymous -> zeros, file-backed -> re-read ---- */
static void demo_dontneed(void)
{
    const size_t len = 16u << 20;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    memset(p, 0x77, len);
    long rss1 = rss_kib();
    CHECK(madvise(p, len, MADV_DONTNEED) == 0);
    long rss2 = rss_kib();
    printf("1) 16 MiB private anon filled with 0x77, MADV_DONTNEED: RSS "
           "%+ld KiB, p[0] now 0x%02x (zero-fill on next touch)\n",
           rss2 - rss1, (unsigned char)p[0]);
    CHECK(p[0] == 0 && p[len - 1] == 0); /* the DATA is gone, not just RAM */
    CHECK(rss1 - rss2 >= (long)(len / 1024) / 2);
    CHECK(munmap(p, len) == 0);

    /* Same call on a shared FILE mapping: the pages are only unmapped;
     * the data lives in the page cache/file and comes back on access. */
    char path[64];
    snprintf(path, sizeof path, "/tmp/vm_madv_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(path);
    CHECK(fd >= 0 && ftruncate(fd, g_page) == 0);
    char *f = mmap(NULL, (size_t)g_page, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0);
    CHECK(f != MAP_FAILED);
    memcpy(f, "keep me", 8);
    CHECK(madvise(f, (size_t)g_page, MADV_DONTNEED) == 0);
    printf("   MAP_SHARED file page after MADV_DONTNEED still reads \"%s\"\n",
           f);
    CHECK(strcmp(f, "keep me") == 0);

    /* ---- 2. WILLNEED + mincore on the same file ---- */
    const size_t flen = 2u << 20;
    CHECK(ftruncate(fd, (off_t)flen) == 0);
    char *buf = malloc(flen); /* owned here */
    CHECK(buf != NULL);
    memset(buf, 'W', flen);
    CHECK(pwrite(fd, buf, flen, 0) == (ssize_t)flen);
    free(buf);
    CHECK(fsync(fd) == 0);
    CHECK(posix_fadvise(fd, 0, (off_t)flen, POSIX_FADV_DONTNEED) == 0);
    char *m = mmap(NULL, flen, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(m != MAP_FAILED);
    int before = resident_pages(m, flen);
    CHECK(madvise(m, flen, MADV_WILLNEED) == 0); /* start read-ahead */
    int after = resident_pages(m, flen);
    printf("2) 2 MiB file: resident pages after FADV_DONTNEED = %d, right "
           "after MADV_WILLNEED = %d of %zu (printed: async)\n", before,
           after, flen / (size_t)g_page);
    munmap(m, flen);
    munmap(f, (size_t)g_page);
    close(fd);
    CHECK(unlink(path) == 0);
}

/* ---- 3. mlock / mlockall ---- */
static void demo_mlock(void)
{
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_MEMLOCK, &rl) == 0);
    printf("3) RLIMIT_MEMLOCK soft = %llu KiB (root or CAP_IPC_LOCK: "
           "unlimited)\n", (unsigned long long)rl.rlim_cur / 1024);

    const size_t len = 1u << 20;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (do_mlock(p, len) != 0) {
        printf("   mlock(1 MiB) failed: %s -> skipped (raise the limit: "
               "ulimit -l, or LimitMEMLOCK= in a systemd unit)\n",
               strerror(errno));
    } else {
        /* mlock faults every page in NOW and keeps it: no fault later. */
        int res = resident_pages(p, len);
        long vmlck = proc_kv("/proc/self/status", "VmLck");
        printf("   mlock(1 MiB): %d/%zu pages resident immediately, VmLck = "
               "%ld kB\n", res, len / (size_t)g_page, vmlck);
        CHECK(res == (int)(len / (size_t)g_page));
        CHECK(vmlck >= 1024);
        CHECK(do_munlock(p, len) == 0);
    }
    CHECK(munmap(p, len) == 0);

    /* mlockall(MCL_CURRENT | MCL_FUTURE) locks EVERYTHING: code, libs,
     * heap, stacks, and every future mapping. Under ASan/TSan "everything"
     * includes terabytes of reserved shadow memory (which is why their
     * interceptor ignores the call), so skip it there. */
    if (UNDER_SANITIZER) {
        printf("   mlockall: skipped under a sanitizer (intercepted as a "
               "no-op; really locking would pin the shadow memory)\n");
        return;
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        printf("   mlockall: %s -> skipped (needs RLIMIT_MEMLOCK >= the "
               "whole process)\n", strerror(errno));
        return;
    }
    printf("   mlockall(MCL_CURRENT|MCL_FUTURE) ok: VmLck = %ld kB\n",
           proc_kv("/proc/self/status", "VmLck"));
    CHECK(munlockall() == 0);
}

/* ---- 4. overcommit ---- */
static void demo_overcommit(void)
{
    long mode = read_long_file("/proc/sys/vm/overcommit_memory");
    long ratio = read_long_file("/proc/sys/vm/overcommit_ratio");
    long total = proc_kv("/proc/meminfo", "MemTotal");
    long swap = proc_kv("/proc/meminfo", "SwapTotal");
    long limit = proc_kv("/proc/meminfo", "CommitLimit");
    long committed = proc_kv("/proc/meminfo", "Committed_AS");
    const char *names[] = {"0 = heuristic (default)", "1 = always",
                           "2 = strict (never overcommit)"};
    printf("4) vm.overcommit_memory = %s, overcommit_ratio = %ld%%\n",
           mode >= 0 && mode <= 2 ? names[mode] : "?", ratio);
    printf("   MemTotal %ld MiB, SwapTotal %ld MiB, CommitLimit %ld MiB, "
           "Committed_AS %ld MiB (promised to all processes)\n",
           total / 1024, swap / 1024, limit / 1024, committed / 1024);

    /* 16 GiB of address space, MAP_NORESERVE: no commit charge at all. */
    size_t big = 16ull << 30;
    long rss0 = rss_kib();
    void *p = mmap(NULL, big, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        printf("   mmap(16 GiB, MAP_NORESERVE) failed: %s (strict mode or "
               "a 32-bit address space)\n", strerror(errno));
    } else {
        ((char *)p)[0] = 1;               /* touch 1 page out of 4 million */
        ((char *)p)[big - 1] = 1;
        long rss1 = rss_kib();
        printf("   mmap(16 GiB, MAP_NORESERVE) ok, touched 2 pages: RSS "
               "%+ld KiB\n", rss1 - rss0);
        CHECK(rss1 - rss0 < 1024);
        CHECK(munmap(p, big) == 0);
    }

    /* Twice RAM+swap WITHOUT MAP_NORESERVE: the heuristic (mode 0)
     * refuses a single request that could never be satisfied. */
    size_t huge = (size_t)(total + swap) * 1024 * 2;
    errno = 0;
    p = mmap(NULL, huge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
             -1, 0);
    int e = errno;
    printf("   mmap(%zu GiB = 2 x (RAM+swap)) without NORESERVE: %s\n",
           huge >> 30, p == MAP_FAILED ? strerror(e) : "succeeded");
    if (p != MAP_FAILED)
        CHECK(munmap(p, huge) == 0);
    if (mode == 0 || mode == 2)
        CHECK(p == MAP_FAILED && e == ENOMEM);
}

/* ---- 5. the OOM killer's view: oom_score and oom_score_adj ---- */
static void demo_oom_score(void)
{
    long score = read_long_file("/proc/self/oom_score");
    long adj = read_long_file("/proc/self/oom_score_adj");
    printf("5) /proc/self/oom_score = %ld (0..1000, higher dies first), "
           "oom_score_adj = %ld (-1000..1000)\n", score, adj);

    /* Do it in a child so we do not change our own (or our parent's) fate.
     * RAISING your adj is always allowed; LOWERING it below the value you
     * started with needs CAP_SYS_RESOURCE. */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            _exit(2);
        int up_ok = write(fd, "500", 3) == 3;
        long s2 = read_long_file("/proc/self/oom_score");
        int down = write(fd, "-500", 4);
        int down_errno = errno;
        close(fd);
        printf("   child: write 500 -> %s, oom_score now %ld; write -500 -> "
               "%s\n", up_ok ? "ok" : "failed", s2,
               down == 4 ? "ok (privileged)" : strerror(down_errno));
        fflush(stdout);
        _exit(up_ok && s2 >= 500 ? 0 : 3);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    printf("   (systemd: OOMScoreAdjust=-900 for the camera daemon, +500 for "
           "a log uploader)\n");
}

int main(void)
{
    alarm(20);
    g_page = sysconf(_SC_PAGESIZE);
    demo_dontneed();
    demo_mlock();
    demo_overcommit();
    demo_oom_score();
    printf("04_madvise_mlock_overcommit: all checks passed\n");
    return 0;
}
