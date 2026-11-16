/*
 * kmb_params.c — checks kmb_hello's module parameters from user space
 *                through /sys/module/kmb_hello/
 *
 * Concept : Every loaded module gets a directory /sys/module/<name>/ with
 *           initstate ("live"), refcnt, taint, holders/ and parameters/.
 *           Each module_param() with a non-zero permission is a file there:
 *           reading it calls the parameter's get(), writing (root, 0644
 *           only) calls its set() — which is where kmb_hello rejects
 *           out-of-range values with EINVAL. This program reads all of it,
 *           checks the ranges the module promises, and (as root) proves the
 *           run-time range check works. It also looks at kmb_lib/holders/
 *           when the export pair is loaded.
 * Build   : make drivers   (-> build/linux-device-drivers/00-kernel-module-basics/user/kmb_params)
 * Run     : sudo insmod kernel/kmb_hello.ko values=1,2,3 name=AI64
 *           ./kmb_params            (read-only checks)
 *           sudo ./kmb_params       (also the write / EINVAL checks)
 *           Needs the module loaded, so `make test` does NOT run it.
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = no module.
 * Read    : ../README.md  (section 3.4 "Module parameters")
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOD "/sys/module/kmb_hello"
#define PAR MOD "/parameters/"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static int exists(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/* Read a whole sysfs attribute (they are small) and strip the newline. */
static int read_attr(const char *path, char *buf, size_t len)
{
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    if (n > 0 && buf[n - 1] == '\n')
        buf[n - 1] = '\0';
    return 0;
}

static long read_long(const char *path)
{
    char buf[64];
    char *end;
    long v;

    CHECK(read_attr(path, buf, sizeof(buf)) == 0);
    errno = 0;
    v = strtol(buf, &end, 10);
    CHECK(errno == 0 && end != buf);
    return v;
}

/*
 * write(2), not fprintf: sysfs runs the store (our set()) during write(),
 * and only write()'s return value carries its errno back.
 */
static int write_attr(const char *path, const char *val)
{
    ssize_t n;
    int fd = open(path, O_WRONLY), saved;

    if (fd < 0)
        return -1;
    n = write(fd, val, strlen(val));
    saved = errno;
    close(fd);
    errno = saved;
    return n < 0 ? -1 : 0;
}

static void test_sysfs_layout(void)
{
    char buf[256];

    CHECK(read_attr(MOD "/initstate", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "live") == 0);
    printf("PASS initstate: %s\n", buf);

    printf("INFO refcnt   : %ld (users of kmb_hello)\n", read_long(MOD "/refcnt"));

    /* Out-of-tree modules taint the kernel with 'O' (plus 'E' if unsigned
     * on a kernel that checks signatures, e.g. Ubuntu's). */
    CHECK(read_attr(MOD "/taint", buf, sizeof(buf)) == 0);
    CHECK(strchr(buf, 'O') != NULL);
    printf("PASS taint    : \"%s\" (O = out-of-tree module)\n", buf);

    /* perm 0 -> no sysfs file */
    CHECK(!exists(PAR "secret"));
    printf("PASS secret   : perm 0 parameter has no file in parameters/\n");
}

static void test_values(void)
{
    char buf[256], *tok, *save = NULL;
    long count = read_long(PAR "count");
    long level = read_long(PAR "level");
    int n = 0;

    CHECK(count >= 1 && count <= 16);
    CHECK(level >= 0 && level <= 7);
    printf("PASS ranges   : count=%ld in 1..16, level=%ld in 0..7\n", count, level);

    CHECK(read_attr(PAR "name", buf, sizeof(buf)) == 0);
    CHECK(buf[0] != '\0');
    printf("PASS name     : \"%s\" (charp)\n", buf);

    CHECK(read_attr(PAR "tag", buf, sizeof(buf)) == 0);
    CHECK(strlen(buf) <= 15);
    printf("PASS tag      : \"%s\" fits the 16-byte buffer\n", buf);

    CHECK(read_attr(PAR "verbose", buf, sizeof(buf)) == 0);
    CHECK(strcmp(buf, "Y") == 0 || strcmp(buf, "N") == 0);
    printf("PASS verbose  : %s (bool shows as Y/N)\n", buf);

    /* Arrays read back comma-separated, only the nr_values given. */
    CHECK(read_attr(PAR "values", buf, sizeof(buf)) == 0);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save))
        n++;
    CHECK(n <= 8);
    printf("PASS values   : %d element(s) (array of max 8)\n", n);
}

static void test_runtime_write(void)
{
    long old = read_long(PAR "level");
    char val[16];

    if (geteuid() != 0) {
        printf("SKIP write    : run as root to test the run-time setter\n");
        return;
    }

    errno = 0;
    CHECK(write_attr(PAR "level", "9") == -1 && errno == EINVAL);
    CHECK(read_long(PAR "level") == old);
    printf("PASS reject   : echo 9 > level -> EINVAL, value stays %ld\n", old);

    CHECK(write_attr(PAR "level", "6") == 0);
    CHECK(read_long(PAR "level") == 6);
    printf("PASS accept   : echo 6 > level -> reads back 6\n");

    snprintf(val, sizeof(val), "%ld", old);
    CHECK(write_attr(PAR "level", val) == 0); /* restore */

    /* 0444 files are read-only even for root. */
    errno = 0;
    CHECK(write_attr(PAR "count", "5") == -1);
    printf("PASS readonly : count (0444) cannot be written, even by root (%s)\n",
           strerror(errno));
}

static void test_export_pair(void)
{
    if (!exists("/sys/module/kmb_lib")) {
        printf("SKIP holders  : kmb_lib not loaded (insmod kmb_lib.ko kmb_consumer.ko)\n");
        return;
    }
    if (exists("/sys/module/kmb_consumer")) {
        CHECK(exists("/sys/module/kmb_lib/holders/kmb_consumer"));
        CHECK(read_long("/sys/module/kmb_lib/refcnt") >= 1);
        printf("PASS holders  : kmb_lib is held by kmb_consumer (refcnt %ld)\n",
               read_long("/sys/module/kmb_lib/refcnt"));
    } else {
        printf("INFO holders  : kmb_lib loaded, no consumer\n");
    }
}

int main(void)
{
    if (!exists(MOD)) {
        fprintf(stderr, "%s not found\n"
                        "load the module first: sudo insmod kmb_hello.ko\n", MOD);
        return 2;
    }
    test_sysfs_layout();
    test_values();
    test_runtime_write();
    test_export_pair();
    printf("all kmb_hello sysfs checks passed\n");
    return 0;
}
