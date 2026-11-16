/*
 * 05_fsync_stat_dirs.c — the page cache and durability (fsync, fdatasync,
 *                        the atomic-replace pattern), file metadata with
 *                        stat/fstat/lstat, hard and symbolic links, and
 *                        reading a directory with opendir/readdir
 *
 * Concept : write() returns as soon as the data is in the PAGE CACHE (RAM).
 *           The kernel writes dirty pages to the device later (~5-30 s).
 *           Pull the power on an SD card or eMMC before that and the data
 *           is gone, or worse, the file is half old/half new. fsync(fd)
 *           waits until the file's data and metadata are on the device;
 *           fdatasync skips metadata that is not needed to read the data
 *           back (e.g. mtime). To update a config file safely: write a
 *           temp file, fsync it, rename() over the old one (atomic), then
 *           fsync the DIRECTORY so the rename itself is durable.
 *           A file's metadata lives in its INODE: stat() reads it. A
 *           directory is just a file of (name -> inode number) entries.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/05_fsync_stat_dirs.c
 * Read    : ../README.md  (sections "3.9 The page cache and fsync" and
 *           "3.10 stat and directories")
 */
#define _GNU_SOURCE /* mkdtemp, fdatasync, d_type */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static char g_dir[64];

static void path_in(char *out, size_t n, const char *name)
{
    int len = snprintf(out, n, "%s/%s", g_dir, name);
    CHECK(len > 0 && (size_t)len < n);
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void write_file(const char *p, const char *text)
{
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    size_t n = strlen(text);
    CHECK(write(fd, text, n) == (ssize_t)n);
    CHECK(close(fd) == 0);
}

static long meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[128];
    long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':')
            v = strtol(line + kl + 1, NULL, 10);
    fclose(f);
    return v;
}

/* ---- 1. page cache, fsync, fdatasync ----
 *
 *   write() ---> [ page cache: dirty pages ] --(flusher thread, later)--> SD/eMMC
 *                   ^ returns HERE                fsync() waits for this ^
 */
static void demo_fsync(void)
{
    char p[128];
    path_in(p, sizeof p, "log.bin");
    enum { SZ = 256 * 1024 };
    char *buf = malloc(SZ); /* owned here */
    CHECK(buf != NULL);
    memset(buf, 'L', SZ);

    long dirty0 = meminfo_kb("Dirty");
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    double t0 = now_ms();
    CHECK(write(fd, buf, SZ) == SZ);
    double t_write = now_ms() - t0;
    long dirty1 = meminfo_kb("Dirty");

    t0 = now_ms();
    CHECK(fsync(fd) == 0); /* data + inode (size, mtime) on the device */
    double t_fsync = now_ms() - t0;

    CHECK(pwrite(fd, buf, SZ, 0) == SZ); /* overwrite: size unchanged */
    t0 = now_ms();
    CHECK(fdatasync(fd) == 0); /* data; mtime may be written later */
    double t_fdatasync = now_ms() - t0;
    CHECK(close(fd) == 0);
    free(buf);

    printf("1) write(256 KiB) %.2f ms (into the page cache), fsync %.2f ms, "
           "fdatasync after overwrite %.2f ms\n", t_write, t_fsync,
           t_fdatasync);
    printf("   /proc/meminfo Dirty: %ld kB before, %ld kB after write() "
           "(system-wide, printed only)\n", dirty0, dirty1);
    CHECK(unlink(p) == 0);
}

/* ---- 2. atomic replace: never leave a half-written config ----
 *
 *   write config.tmp -> fsync(tmp) -> rename(tmp, config) -> fsync(dir)
 *   power cut at any point: you see the complete OLD or the complete NEW
 */
static void demo_atomic_replace(void)
{
    char cfg[128], tmp[128];
    path_in(cfg, sizeof cfg, "app.conf");
    path_in(tmp, sizeof tmp, "app.conf.tmp");
    write_file(cfg, "rate=30\n");

    const char next[] = "rate=60\nmode=hdr\n";
    int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
    CHECK(fd >= 0);
    CHECK(write(fd, next, sizeof next - 1) == (ssize_t)(sizeof next - 1));
    CHECK(fsync(fd) == 0);  /* the NEW bytes are on the device ... */
    CHECK(close(fd) == 0);
    CHECK(rename(tmp, cfg) == 0); /* ... then switch names atomically */
    int dfd = open(g_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(dfd >= 0);
    CHECK(fsync(dfd) == 0); /* the directory entry change is durable */
    CHECK(close(dfd) == 0);

    char back[64] = {0};
    fd = open(cfg, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0 && read(fd, back, sizeof back - 1) == (ssize_t)(sizeof next - 1));
    close(fd);
    CHECK(strcmp(back, next) == 0);
    CHECK(access(tmp, F_OK) == -1 && errno == ENOENT);
    printf("2) atomic replace: tmp + fsync + rename + fsync(dir) -> "
           "app.conf = \"rate=60 mode=hdr\"\n");
}

/* ---- 3. stat, hard links, symlinks ----
 *
 *   directory entries          inode 1234
 *   "app.conf"  --------+-->  [ mode 0644, nlink 2, size 17, blocks ... ]
 *   "hard.conf" --------+
 *   "soft.conf" --> inode 5678 [ symlink: "app.conf" ]
 */
static void demo_stat(void)
{
    char cfg[128], hard[128], soft[128];
    path_in(cfg, sizeof cfg, "app.conf");
    path_in(hard, sizeof hard, "hard.conf");
    path_in(soft, sizeof soft, "soft.conf");

    struct stat a, h, s_follow, s_link;
    CHECK(stat(cfg, &a) == 0);
    CHECK(S_ISREG(a.st_mode));
    printf("3) stat(app.conf): inode %lu, mode %o, nlink %lu, size %ld, "
           "blocks %ld, uid %u\n", (unsigned long)a.st_ino,
           (unsigned)(a.st_mode & 07777), (unsigned long)a.st_nlink,
           (long)a.st_size, (long)a.st_blocks, (unsigned)a.st_uid);
    CHECK(a.st_nlink == 1 && a.st_size == 17);

    CHECK(link(cfg, hard) == 0); /* second NAME for the same inode */
    CHECK(stat(hard, &h) == 0 && stat(cfg, &a) == 0);
    CHECK(h.st_ino == a.st_ino && a.st_nlink == 2);
    printf("   link() -> hard.conf has the SAME inode %lu, nlink now %lu\n",
           (unsigned long)h.st_ino, (unsigned long)a.st_nlink);

    CHECK(symlink("app.conf", soft) == 0); /* a small file holding a path */
    CHECK(stat(soft, &s_follow) == 0);     /* follows the link */
    CHECK(lstat(soft, &s_link) == 0);      /* the link itself */
    CHECK(s_follow.st_ino == a.st_ino && S_ISLNK(s_link.st_mode));
    char target[64] = {0};
    CHECK(readlink(soft, target, sizeof target - 1) == 8);
    printf("   symlink: stat() follows it (inode %lu), lstat() sees a link "
           "(inode %lu) -> \"%s\"\n", (unsigned long)s_follow.st_ino,
           (unsigned long)s_link.st_ino, target);

    /* unlink removes a NAME. The inode lives on while another name or an
     * open fd refers to it. */
    int fd = open(cfg, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(unlink(cfg) == 0 && unlink(hard) == 0);
    struct stat f;
    CHECK(fstat(fd, &f) == 0);
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == 'r');
    printf("   both names unlinked, but the open fd still reads it: nlink=%lu "
           "(freed at close)\n", (unsigned long)f.st_nlink);
    CHECK(f.st_nlink == 0);
    close(fd);
    errno = 0;
    CHECK(stat(soft, &s_follow) == -1 && errno == ENOENT); /* dangling */
    CHECK(unlink(soft) == 0);
}

/* ---- 4. directories ---- */
static void demo_readdir(void)
{
    const char *names[] = {"a.txt", "b.txt", "c.txt"};
    for (int i = 0; i < 3; i++) {
        char p[128];
        path_in(p, sizeof p, names[i]);
        write_file(p, "x");
    }
    char sub[128];
    path_in(sub, sizeof sub, "subdir");
    CHECK(mkdir(sub, 0755) == 0);

    DIR *d = opendir(g_dir); /* owned here, closedir below */
    CHECK(d != NULL);
    int files = 0, dirs = 0, dots = 0;
    struct dirent *e;
    printf("4) readdir(%s):", g_dir);
    errno = 0;
    while ((e = readdir(d)) != NULL) { /* order is NOT sorted */
        printf(" %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            dots++;
        else if (e->d_type == DT_DIR)
            dirs++;
        else if (e->d_type == DT_REG)
            files++;
        /* DT_UNKNOWN is legal on some filesystems: then call lstat(). */
    }
    CHECK(errno == 0); /* NULL + errno set = error, not end of directory */
    printf("\n");
    closedir(d);
    CHECK(dots == 2 && files == 3 && dirs == 1);
    printf("   %d regular files, %d directory, plus \".\" and \"..\"\n",
           files, dirs);

    for (int i = 0; i < 3; i++) {
        char p[128];
        path_in(p, sizeof p, names[i]);
        CHECK(unlink(p) == 0);
    }
    CHECK(rmdir(sub) == 0);
}

int main(void)
{
    alarm(20);
    snprintf(g_dir, sizeof g_dir, "/tmp/fsync_%d_XXXXXX", (int)getpid());
    CHECK(mkdtemp(g_dir) != NULL);

    demo_fsync();
    demo_atomic_replace();
    demo_stat();
    demo_readdir();

    CHECK(rmdir(g_dir) == 0);
    printf("05_fsync_stat_dirs: all checks passed\n");
    return 0;
}
