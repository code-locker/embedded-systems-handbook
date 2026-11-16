/*
 * memdev_test.c — self-checking test of /dev/memdevN (memdev.ko) and,
 *                 if loaded, /dev/memdev_misc (memdev_misc.ko)
 *
 * Concept : Everything a char driver promises is visible from user space
 *           as return values and errno: read() returning 0 at EOF, short
 *           writes and ENOSPC, lseek() positions and EINVAL, pread/pwrite
 *           not moving the file offset, ioctl()s with ENOTTY for unknown
 *           commands and EFAULT for bad pointers, per-open state (two
 *           open()s) versus shared state (dup()), O_TRUNC / O_APPEND, an
 *           exclusive minor returning EBUSY, and the driver's mutex keeping
 *           concurrent O_APPEND writers from overlapping.
 * Build   : make drivers   (-> build/linux-device-drivers/01-char-devices-and-file-operations/user/memdev_test)
 * Run     : sudo insmod kernel/memdev.ko [&& sudo insmod kernel/memdev_misc.ko]
 *           ./memdev_test        (the nodes are 0666, no sudo needed)
 *           Needs the module loaded, so `make test` does NOT run it.
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = no device.
 * Read    : ../README.md  (section 8 "Files in this lesson")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../kernel/memdev_ioctl.h" /* the SAME header the driver uses */

#define DEV0 "/dev/memdev0"
#define DEV_MISC "/dev/memdev_misc"
#define PARAMS "/sys/module/memdev/parameters/"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static long param(const char *name)
{
    char path[128];
    long v = -1;
    FILE *f;

    snprintf(path, sizeof(path), PARAMS "%s", name);
    f = fopen(path, "r");
    CHECK(f != NULL);
    CHECK(fscanf(f, "%ld", &v) == 1);
    fclose(f);
    return v;
}

static struct memdev_stats stats(int fd)
{
    struct memdev_stats st;

    CHECK(ioctl(fd, MEMDEV_IOC_GET_STATS, &st) == 0);
    return st;
}

static void set_size(int fd, __u64 size)
{
    CHECK(ioctl(fd, MEMDEV_IOC_SET_SIZE, &size) == 0);
}

/* ---- read / write / EOF ------------------------------------------------ */
static void test_rw(int fd)
{
    char buf[64] = { 0 };

    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(stats(fd).len == 0);

    CHECK(write(fd, "hello world", 11) == 11);
    CHECK(lseek(fd, 0, SEEK_CUR) == 11);  /* write advanced f_pos */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 11); /* asks 64, gets what exists */
    CHECK(memcmp(buf, "hello world", 11) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 0);  /* EOF: read returns 0 */
    printf("PASS rw       : write 11, read back 11, then read() = 0 (EOF)\n");
}

/* ---- lseek ------------------------------------------------------------- */
static void test_seek(int fd)
{
    char buf[8] = { 0 };
    __u64 size;

    CHECK(lseek(fd, -5, SEEK_END) == 6);     /* len 11 - 5 */
    CHECK(read(fd, buf, 5) == 5 && memcmp(buf, "world", 5) == 0);
    CHECK(lseek(fd, -11, SEEK_CUR) == 0);
    errno = 0;
    CHECK(lseek(fd, -1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0);      /* failed seek: no change */

    CHECK(ioctl(fd, MEMDEV_IOC_GET_SIZE, &size) == 0);
    errno = 0;
    CHECK(lseek(fd, (off_t)size + 1, SEEK_SET) == -1 && errno == EINVAL);
    CHECK(lseek(fd, (off_t)size, SEEK_SET) == (off_t)size);
    errno = 0;
    CHECK(write(fd, "x", 1) == -1 && errno == ENOSPC);
    printf("PASS seek     : SEEK_END/CUR/SET, <0 and >size -> EINVAL, write at size -> ENOSPC\n");
}

/* ---- pread / pwrite use their own offset, f_pos stays ------------------ */
static void test_pread(int fd)
{
    char buf[8] = { 0 };

    CHECK(lseek(fd, 3, SEEK_SET) == 3);
    CHECK(pwrite(fd, "HELLO", 5, 0) == 5);
    CHECK(pread(fd, buf, 5, 0) == 5 && memcmp(buf, "HELLO", 5) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 3);
    printf("PASS pread    : pwrite/pread at 0 while f_pos stays 3 (*ppos is a copy)\n");
}

/* ---- ioctl: sizes ------------------------------------------------------ */
static void test_resize(int fd)
{
    char big[32], buf[32];
    struct memdev_stats st;
    __u64 orig, v;
    long max = param("max_size");

    CHECK(ioctl(fd, MEMDEV_IOC_GET_SIZE, &orig) == 0);
    memset(big, 'z', sizeof(big));

    set_size(fd, 16);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(write(fd, big, sizeof(big)) == 16);   /* short write */
    errno = 0;
    CHECK(write(fd, big, 1) == -1 && errno == ENOSPC);
    st = stats(fd);
    CHECK(st.size == 16 && st.len == 16);

    set_size(fd, 8);                            /* shrink keeps the prefix */
    CHECK(pread(fd, buf, sizeof(buf), 0) == 8 && memcmp(buf, big, 8) == 0);

    v = 0;
    errno = 0;
    CHECK(ioctl(fd, MEMDEV_IOC_SET_SIZE, &v) == -1 && errno == EINVAL);
    v = (__u64)max + 1;
    errno = 0;
    CHECK(ioctl(fd, MEMDEV_IOC_SET_SIZE, &v) == -1 && errno == EINVAL);

    set_size(fd, orig);
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    printf("PASS resize   : 16 B -> short write 16 + ENOSPC, shrink keeps prefix, 0 / %ld+1 -> EINVAL\n",
           max);
}

/* ---- errno contract: ENOTTY, EFAULT ------------------------------------ */
/*
 * A PROT_NONE page is a valid user address the kernel cannot read or
 * write: copy_*_user / get_user / put_user fail and the driver returns
 * EFAULT instead of crashing. (ASan does not object: the page is ordinary
 * mapped memory as far as its shadow is concerned.)
 */
static void test_errno(int fd)
{
    long pg = sysconf(_SC_PAGESIZE);
    void *bad = mmap(NULL, (size_t)pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    CHECK(bad != MAP_FAILED);

    errno = 0;
    CHECK(ioctl(fd, _IO('x', 1)) == -1 && errno == ENOTTY);           /* other magic */
    errno = 0;
    CHECK(ioctl(fd, _IO(MEMDEV_IOC_MAGIC, 99)) == -1 && errno == ENOTTY);
    errno = 0;
    CHECK(ioctl(fd, MEMDEV_IOC_GET_SIZE, bad) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(ioctl(fd, MEMDEV_IOC_SET_SIZE, bad) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(ioctl(fd, MEMDEV_IOC_GET_STATS, bad) == -1 && errno == EFAULT);

    CHECK(pwrite(fd, "data", 4, 0) == 4);
    errno = 0;
    CHECK(pread(fd, bad, 4, 0) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(pwrite(fd, bad, 4, 0) == -1 && errno == EFAULT);

    CHECK(munmap(bad, (size_t)pg) == 0);
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    printf("PASS errno    : unknown ioctl -> ENOTTY; bad pointer to ioctl/read/write -> EFAULT\n");
}

/* ---- per-open state vs shared state ------------------------------------ */
/*
 *   fd  --+                       fd2 ---> struct file B (private_data B)
 *         +--> struct file A (private_data A)
 *   fdd --+   (dup shares it)          both -> the same struct memdev
 */
static void test_per_open(int fd)
{
    struct memdev_stats a0 = stats(fd), a1, b;
    int fd2 = open(DEV0, O_RDWR), fdd = dup(fd);

    CHECK(fd2 >= 0 && fdd >= 0);
    CHECK(stats(fd2).openers >= 2);

    CHECK(pwrite(fd2, "abc", 3, 0) == 3);
    a1 = stats(fd);
    b = stats(fd2);
    CHECK(b.fd_write_bytes == 3);                       /* fd2's own */
    CHECK(a1.fd_write_bytes == a0.fd_write_bytes);      /* fd unaffected */
    CHECK(a1.dev_writes >= a0.dev_writes + 1);          /* device-wide */

    CHECK(pwrite(fdd, "de", 2, 3) == 2);                /* through the dup */
    CHECK(stats(fd).fd_write_bytes == a0.fd_write_bytes + 2);
    close(fdd);
    close(fd2);
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    printf("PASS per-open : 2nd open() has its own counters, dup() shares the struct file\n");
}

/* ---- O_TRUNC / O_APPEND, i.e. `echo >` and `echo >>` ------------------- */
static void test_trunc_append(int fd)
{
    char buf[8] = { 0 };
    int w;

    CHECK(pwrite(fd, "old data", 8, 0) == 8);
    w = open(DEV0, O_WRONLY | O_TRUNC);                 /* like: echo ab > dev */
    CHECK(w >= 0);
    CHECK(stats(fd).len == 0);
    CHECK(write(w, "ab", 2) == 2);
    close(w);
    w = open(DEV0, O_WRONLY | O_APPEND);                /* like: echo cd >> dev */
    CHECK(w >= 0);
    CHECK(write(w, "cd", 2) == 2);
    close(w);
    CHECK(pread(fd, buf, sizeof(buf), 0) == 4 && memcmp(buf, "abcd", 4) == 0);
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    printf("PASS trunc    : O_TRUNC empties, O_APPEND writes at the end -> \"abcd\"\n");
}

/* ---- exclusive minor --------------------------------------------------- */
static void test_exclusive(void)
{
    long ex = param("excl_minor");
    char path[32];
    int a, b;

    if (ex < 0) {
        printf("SKIP excl     : module loaded with excl_minor=-1\n");
        return;
    }
    snprintf(path, sizeof(path), "/dev/memdev%ld", ex);
    a = open(path, O_RDWR);
    CHECK(a >= 0);
    CHECK(stats(a).exclusive == 1);
    errno = 0;
    b = open(path, O_RDONLY);
    CHECK(b == -1 && errno == EBUSY);
    close(a);
    b = open(path, O_RDONLY);                           /* free again */
    CHECK(b >= 0);
    close(b);
    printf("PASS excl     : %s second open -> EBUSY, free again after close\n", path);
}

/* ---- concurrent appenders: the driver mutex keeps records whole -------- */
#define REC 8
#define RECS 200

static void *appender(void *arg)
{
    char rec[REC];
    int i, fd = open(DEV0, O_WRONLY | O_APPEND);

    CHECK(fd >= 0);
    memset(rec, *(const char *)arg, sizeof(rec));
    for (i = 0; i < RECS; i++)
        CHECK(write(fd, rec, REC) == REC);
    close(fd);
    return NULL;
}

static void test_concurrent(int fd)
{
    static char buf[2 * REC * RECS];
    pthread_t t1, t2;
    int i, na = 0, nb = 0;
    __u64 orig;

    CHECK(ioctl(fd, MEMDEV_IOC_GET_SIZE, &orig) == 0);
    set_size(fd, sizeof(buf));
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    CHECK(pthread_create(&t1, NULL, appender, "A") == 0);
    CHECK(pthread_create(&t2, NULL, appender, "B") == 0);
    CHECK(pthread_join(t1, NULL) == 0 && pthread_join(t2, NULL) == 0);

    CHECK(pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
    for (i = 0; i < 2 * RECS; i++) {
        const char *r = buf + i * REC;

        CHECK(memcmp(r, r + 1, REC - 1) == 0);          /* record not torn */
        na += r[0] == 'A';
        nb += r[0] == 'B';
    }
    CHECK(na == RECS && nb == RECS);
    set_size(fd, orig);
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == 0);
    printf("PASS threads  : 2 x %d O_APPEND records, none lost or torn (driver mutex)\n",
           RECS);
}

/* ---- misc variant ------------------------------------------------------ */
static void test_misc(void)
{
    char buf[16] = { 0 };
    int fd = open(DEV_MISC, O_RDWR | O_TRUNC);

    if (fd < 0) {
        printf("SKIP misc     : %s not present (insmod memdev_misc.ko)\n", DEV_MISC);
        return;
    }
    CHECK(write(fd, "misc!", 5) == 5);
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 5 && memcmp(buf, "misc!", 5) == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 0);
    CHECK(lseek(fd, -1, SEEK_END) == 4);
    CHECK(lseek(fd, 4096, SEEK_SET) == 4096);
    errno = 0;
    CHECK(write(fd, "x", 1) == -1 && errno == ENOSPC);
    errno = 0;
    CHECK(ioctl(fd, MEMDEV_IOC_CLEAR) == -1 && errno == ENOTTY); /* no .unlocked_ioctl */
    close(fd);
    printf("PASS misc     : %s read/write/llseek via libfs helpers, no ioctl -> ENOTTY\n",
           DEV_MISC);
}

int main(void)
{
    int fd = open(DEV0, O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n"
                        "load the module first: sudo insmod memdev.ko\n",
                DEV0, strerror(errno));
        return 2;
    }
    if (param("excl_minor") == 0) {
        fprintf(stderr, "memdev0 is the exclusive minor; this test needs several opens of it.\n"
                        "reload with: sudo insmod memdev.ko excl_minor=1 (or -1)\n");
        close(fd);
        return 2;
    }
    printf("INFO %s: %ld minors, max_size %ld, exclusive minor %ld\n", DEV0,
           param("nr_devs"), param("max_size"), param("excl_minor"));
    test_rw(fd);
    test_seek(fd);
    test_pread(fd);
    test_resize(fd);
    test_errno(fd);
    test_per_open(fd);
    test_trunc_append(fd);
    test_concurrent(fd);
    close(fd);
    test_exclusive();
    test_misc();
    printf("all memdev checks passed\n");
    return 0;
}
