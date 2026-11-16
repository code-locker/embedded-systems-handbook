/*
 * 02_open_read_write.c — open/read/write/lseek/close, the open() flags,
 *                        sparse files, and short reads/writes handled with
 *                        read_full()/write_full()
 *
 * Concept : A file descriptor is a small int that indexes your process's
 *           fd table. open() picks the LOWEST free number. read()/write()
 *           move the file OFFSET and may transfer FEWER bytes than asked
 *           (pipes, sockets, terminals, signals, non-blocking fds, a full
 *           disk): every robust program loops. The flags decide the rest:
 *           O_CREAT|O_EXCL = "create, fail if it exists" (lock files),
 *           O_TRUNC, O_APPEND (atomic seek-to-end + write), O_CLOEXEC,
 *           O_NONBLOCK (EAGAIN instead of sleeping), O_SYNC, O_DIRECT.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/02_open_read_write.c
 * Read    : ../README.md  (sections "3.4 Everything is a file", "3.5 open
 *           flags" and "3.6 Short reads and writes")
 */
#define _GNU_SOURCE /* O_DIRECT, F_GETPIPE_SZ, mkdtemp */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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

/* ---- the two loops every systems programmer writes once ----
 *
 *  want 10 bytes:   read() -> 4    read() -> 6     done
 *                   [####......]   [##########]
 *
 * read_full: returns the bytes read; < n only at EOF. -1 on error.
 * write_full: returns n, or -1 on error. Both retry on EINTR. */
static ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (r == 0) /* EOF: the writer closed / end of file */
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t write_full(int fd, const void *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, (const char *)buf + done, n - done);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)w; /* w == 0 does not happen for n > 0 on files */
    }
    return (ssize_t)done;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static char g_dir[64];

static void path_in(char *out, size_t n, const char *name)
{
    int len = snprintf(out, n, "%s/%s", g_dir, name);
    CHECK(len > 0 && (size_t)len < n);
}

/* ---- 1. create, write, seek, read, sparse file ---- */
static void demo_basic(void)
{
    char p[128];
    path_in(p, sizeof p, "data.txt");

    /* O_EXCL: create or fail. The check and the create are ONE syscall, so
     * there is no window for another process (unlike access()+open()). */
    int fd = open(p, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    errno = 0;
    CHECK(open(p, O_CREAT | O_EXCL | O_RDWR, 0644) == -1 && errno == EEXIST);
    printf("1) open(O_CREAT|O_EXCL) -> fd %d; second time -> EEXIST\n", fd);

    const char msg[] = "hello, file\n";
    CHECK(write_full(fd, msg, sizeof msg - 1) == (ssize_t)(sizeof msg - 1));
    /* The offset is now at the end; read() here would return 0 (EOF). */
    char buf[64] = {0};
    CHECK(read(fd, buf, sizeof buf) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read_full(fd, buf, sizeof msg - 1) == (ssize_t)(sizeof msg - 1));
    CHECK(strcmp(buf, msg) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == (off_t)(sizeof msg - 1));
    printf("   wrote %zu bytes, read at offset 0 after lseek(SEEK_SET); "
           "SEEK_CUR = %ld\n", sizeof msg - 1, (long)lseek(fd, 0, SEEK_CUR));

    /* Seek 1 MiB past the end and write 1 byte: the gap is a HOLE. It reads
     * as zeros and takes no disk blocks (on ext4, tmpfs, f2fs...). */
    const off_t far = 1 << 20;
    CHECK(lseek(fd, far, SEEK_SET) == far);
    CHECK(write(fd, "X", 1) == 1);
    struct stat sb;
    CHECK(fstat(fd, &sb) == 0);
    CHECK(sb.st_size == far + 1);
    char z;
    CHECK(pread(fd, &z, 1, 4096) == 1 && z == 0);
    printf("   sparse: st_size = %ld bytes but st_blocks*512 = %ld bytes "
           "allocated; the hole reads as 0\n", (long)sb.st_size,
           (long)sb.st_blocks * 512);
    CHECK(close(fd) == 0);

    /* O_TRUNC: open an existing file and cut it to length 0. */
    fd = open(p, O_WRONLY | O_TRUNC | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(fstat(fd, &sb) == 0 && sb.st_size == 0);
    CHECK(close(fd) == 0);
    printf("   O_TRUNC: size is now %ld\n", (long)sb.st_size);
    CHECK(unlink(p) == 0);
}

/* ---- 2. lowest free fd, and O_CLOEXEC ---- */
static void demo_fd_numbers(void)
{
    char p[128];
    path_in(p, sizeof p, "fds.txt");
    int a = open(p, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    int b = open(p, O_RDWR | O_CLOEXEC);
    int c = open(p, O_RDWR); /* no O_CLOEXEC on purpose */
    CHECK(a >= 0 && b > a && c > b); /* not always a+1: an inherited
                                         fd may sit in a gap */
    CHECK(close(b) == 0);
    int d = open(p, O_RDWR | O_CLOEXEC);
    printf("2) opened fds %d %d %d, closed %d, next open() got %d "
           "(lowest free number)\n", a, b, c, b, d);
    CHECK(d == b); /* POSIX guarantees the lowest free slot */

    /* FD_CLOEXEC is a per-FD flag (lives in the fd table, not in the open
     * file description): exec() closes the fd. Without it, every child you
     * exec inherits your files, sockets, device nodes... */
    CHECK((fcntl(a, F_GETFD) & FD_CLOEXEC) != 0);
    CHECK((fcntl(c, F_GETFD) & FD_CLOEXEC) == 0);
    printf("   fd %d has FD_CLOEXEC (O_CLOEXEC), fd %d does not: it would "
           "leak into an exec()'d program\n", a, c);
    close(a);
    close(c);
    close(d);
    CHECK(unlink(p) == 0);
}

/* ---- 3. O_APPEND: atomic "seek to end + write" ----
 *
 *  without O_APPEND: fd1 off=0  fd2 off=0   both write at 0 -> overwrite
 *  with    O_APPEND: every write() goes to the CURRENT end, atomically
 *                    (this is how many processes share one log file)
 */
static void demo_append(void)
{
    char p[128];
    path_in(p, sizeof p, "log.txt");
    char buf[64] = {0};

    int f1 = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    int f2 = open(p, O_WRONLY | O_CLOEXEC);
    CHECK(f1 >= 0 && f2 >= 0);
    CHECK(write(f1, "AAAA", 4) == 4);
    CHECK(write(f2, "BB", 2) == 2); /* f2's offset is still 0 */
    close(f1);
    close(f2);
    int r = open(p, O_RDONLY | O_CLOEXEC);
    CHECK(read_full(r, buf, sizeof buf - 1) == 4);
    close(r);
    printf("3) two fds, no O_APPEND : \"%s\" (BB overwrote AA)\n", buf);
    CHECK(strcmp(buf, "BBAA") == 0);

    f1 = open(p, O_TRUNC | O_WRONLY | O_APPEND | O_CLOEXEC);
    f2 = open(p, O_WRONLY | O_APPEND | O_CLOEXEC);
    CHECK(f1 >= 0 && f2 >= 0);
    CHECK(write(f1, "AAAA", 4) == 4);
    CHECK(write(f2, "BB", 2) == 2);
    CHECK(write(f1, "CC", 2) == 2);
    close(f1);
    close(f2);
    memset(buf, 0, sizeof buf);
    r = open(p, O_RDONLY | O_CLOEXEC);
    CHECK(read_full(r, buf, sizeof buf - 1) == 8);
    close(r);
    printf("   two fds, O_APPEND    : \"%s\" (nothing lost)\n", buf);
    CHECK(strcmp(buf, "AAAABBCC") == 0);
    CHECK(unlink(p) == 0);
}

/* ---- 4. short reads/writes on a pipe, and O_NONBLOCK ---- */
static void demo_short_io(void)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    int cap = fcntl(p[1], F_GETPIPE_SZ); /* usually 65536 */
    CHECK(cap > 0);

    /* Non-blocking empty read: EAGAIN instead of sleeping. */
    CHECK(fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK) == 0);
    char c;
    errno = 0;
    CHECK(read(p[0], &c, 1) == -1 && errno == EAGAIN);
    printf("4) pipe capacity %d bytes; non-blocking read on empty pipe -> "
           "EAGAIN\n", cap);

    /* Non-blocking write bigger than the pipe: a SHORT WRITE. Code that
     * assumes write() == n silently drops the rest. */
    size_t big = (size_t)cap * 4;
    char *src = malloc(big); /* owned here, freed below */
    CHECK(src != NULL);
    for (size_t i = 0; i < big; i++)
        src[i] = (char)(i * 7 + 3);
    CHECK(fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK) == 0);
    ssize_t w = write(p[1], src, big);
    printf("   non-blocking write(%zu) returned %zd: SHORT WRITE\n", big, w);
    CHECK(w > 0 && (size_t)w < big);
    errno = 0;
    CHECK(write(p[1], src, big) == -1 && errno == EAGAIN); /* now full */
    close(p[0]);
    close(p[1]);

    /* Blocking pipe + a child that reads in small pieces: every read() the
     * child makes returns less than it asked for while we are still
     * writing; read_full()/write_full() hide all of that. */
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(p[1]);
        char *dst = malloc(big); /* child's own copy, freed before _exit */
        if (!dst)
            _exit(2);
        size_t got = 0;
        int calls = 0;
        while (got < big) {
            ssize_t r = read(p[0], dst + got, big - got); /* asks for ALL */
            if (r < 0 && errno == EINTR)
                continue;
            if (r <= 0)
                break;
            got += (size_t)r;
            calls++;
        }
        int ok = got == big && memcmp(dst, src, big) == 0;
        free(dst);
        free(src);
        /* report the number of read() calls via the exit status */
        _exit(ok ? (calls > 255 ? 255 : calls) : 0);
    }
    close(p[0]);
    CHECK(write_full(p[1], src, big) == (ssize_t)big);
    close(p[1]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st));
    int calls = WEXITSTATUS(st);
    CHECK(calls >= 1);
    printf("   blocking: write_full(%zu) done; the child needed %d read() "
           "calls asking for everything each time\n", big, calls);
    free(src);
}

/* ---- 5. O_SYNC and O_DIRECT (brief) ---- */
static void demo_sync_direct(void)
{
    char p[128];
    path_in(p, sizeof p, "sync.bin");
    char block[4096];
    memset(block, 'S', sizeof block);
    enum { N = 16 };

    double t0 = now_ms();
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    for (int i = 0; i < N; i++)
        CHECK(write_full(fd, block, sizeof block) == (ssize_t)sizeof block);
    close(fd);
    double t_cache = now_ms() - t0;

    /* O_SYNC: each write() returns only when data AND metadata are on the
     * device — like write() + fsync() every time. Slow, but nothing is
     * lost on a power cut once write() has returned. */
    t0 = now_ms();
    fd = open(p, O_TRUNC | O_WRONLY | O_SYNC | O_CLOEXEC);
    CHECK(fd >= 0);
    for (int i = 0; i < N; i++)
        CHECK(write_full(fd, block, sizeof block) == (ssize_t)sizeof block);
    close(fd);
    double t_sync = now_ms() - t0;
    printf("5) %d x 4 KiB writes: page cache %.2f ms, O_SYNC %.2f ms "
           "(printed, depends on the disk)\n", N, t_cache, t_sync);

    /* O_DIRECT: bypass the page cache (DMA straight from your buffer).
     * Rules: buffer address, length and file offset aligned to the logical
     * block size (512 B or 4 KiB). tmpfs refuses O_DIRECT (EINVAL). */
    fd = open(p, O_WRONLY | O_DIRECT | O_CLOEXEC);
    if (fd < 0) {
        printf("   O_DIRECT not supported on this filesystem (%s): skipped\n",
               strerror(errno));
    } else {
        void *al = NULL;
        CHECK(posix_memalign(&al, 4096, 4096) == 0); /* freed below */
        memset(al, 'D', 4096);
        ssize_t ok = write(fd, al, 4096);
        errno = 0;
        ssize_t bad = write(fd, (char *)al + 1, 100); /* misaligned */
        int e = errno;
        printf("   O_DIRECT: aligned 4 KiB write -> %zd; misaligned 100 B "
               "write -> %zd (%s)\n", ok, bad, bad < 0 ? strerror(e) : "ok");
        free(al);
        close(fd);
    }
    CHECK(unlink(p) == 0);
}

int main(void)
{
    alarm(20);
    snprintf(g_dir, sizeof g_dir, "/tmp/fileio_%d_XXXXXX", (int)getpid());
    CHECK(mkdtemp(g_dir) != NULL); /* private per-run directory */

    demo_basic();
    demo_fd_numbers();
    demo_append();
    demo_short_io();
    demo_sync_direct();

    CHECK(rmdir(g_dir) == 0); /* fails if we left a file behind */
    printf("02_open_read_write: all checks passed\n");
    return 0;
}
