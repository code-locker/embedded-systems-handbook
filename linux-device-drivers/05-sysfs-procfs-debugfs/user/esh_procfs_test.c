/*
 * esh_procfs_test.c — user-space test for procfs_demo.ko: seq_file output
 *                     read in tiny chunks vs one big read, the stats file,
 *                     commands written to ctl, and /proc/self as an example
 *                     of the kernel exporting per-process data
 *
 * Concept : A seq_file must give the same bytes whether you read it with
 *           7-byte reads or one 1 MiB read: each read() restarts the
 *           iterator at the saved position. This program checks that, counts
 *           the lines against "stats", then adds and deletes an item through
 *           "ctl" (needs root) and checks the list follows.
 * Build   : make drivers   (-> build/linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_procfs_test)
 * Run     : sudo insmod kernel/procfs_demo.ko nitems=200
 *           sudo ./esh_procfs_test        (without sudo the ctl tests are skipped)
 *           Needs the module loaded, so `make test` does NOT run it.
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = module not loaded.
 * Read    : ../README.md  (section 3.7 "procfs and seq_file")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PDIR "/proc/esh_procfs"
#define BIG (1024 * 1024)

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/*
 * Read a whole file with read() calls of `chunk` bytes. Returns a malloc'd
 * NUL-terminated buffer (caller frees) and stores the length and number of
 * read() calls that returned data.
 */
static char *slurp(const char *path, size_t chunk, size_t *len, int *calls)
{
    char *buf = malloc(BIG);
    size_t total = 0;
    ssize_t n;
    int fd = open(path, O_RDONLY);

    CHECK(buf != NULL);
    CHECK(fd >= 0);
    *calls = 0;
    while (total + chunk < BIG && (n = read(fd, buf + total, chunk)) > 0) {
        total += (size_t)n;
        (*calls)++;
    }
    close(fd);
    CHECK(total + chunk < BIG); /* file not unexpectedly huge */
    buf[total] = '\0';
    *len = total;
    return buf;
}

/* "key: value" lookup in the stats file. */
static long stat_value(const char *key)
{
    size_t len;
    int calls;
    char *s = slurp(PDIR "/stats", 4096, &len, &calls);
    char *p = strstr(s, key);
    long v;

    CHECK(p != NULL);
    v = strtol(p + strlen(key), NULL, 10);
    free(s);
    return v;
}

static int count_lines(const char *s)
{
    int n = 0;

    for (; *s; s++)
        n += *s == '\n';
    return n;
}

/* ---- seq_file: small reads == big read ---------------------------------- */
static void test_items(void)
{
    size_t len_small, len_big;
    int calls_small, calls_big, lines;
    long items = stat_value("items:"), starts0, starts1;
    char *small, *big, *line;
    unsigned int prev = 0, id;
    int ordered = 1;

    starts0 = stat_value("seq_starts:");
    small = slurp(PDIR "/items", 7, &len_small, &calls_small);
    big = slurp(PDIR "/items", BIG / 2, &len_big, &calls_big);
    starts1 = stat_value("seq_starts:");

    CHECK(len_small == len_big && memcmp(small, big, len_big) == 0);
    lines = count_lines(big);
    CHECK(lines == items + 1); /* + header line */
    CHECK(strncmp(big, "  id name", 9) == 0);

    /* ids strictly increase: the list is kept in creation order */
    line = strchr(big, '\n');
    while (line && line[1]) {
        CHECK(sscanf(line + 1, "%u", &id) == 1);
        ordered &= id > prev;
        prev = id;
        line = strchr(line + 1, '\n');
    }
    CHECK(ordered);
    printf("PASS items: %ld items, %zu bytes, identical with %d x 7-byte reads and %d big read(s); "
           "seq start() ran %ld times\n",
           items, len_big, calls_small, calls_big, starts1 - starts0);
    if (len_big > 4096)
        printf("     (output > one 4 KiB page: seq_file paged it for us)\n");
    free(small);
    free(big);
}

/* ---- ctl: add, find, delete, bad input ----------------------------------- */
static int ctl(const char *cmd)
{
    int fd = open(PDIR "/ctl", O_WRONLY);
    int err = 0;

    if (fd < 0)
        return errno;
    if (write(fd, cmd, strlen(cmd)) < 0)
        err = errno;
    close(fd);
    return err;
}

static void test_ctl(void)
{
    size_t len;
    int calls;
    long items0 = stat_value("items:"), id;
    char *s, *p, cmd[64];

    if (access(PDIR "/ctl", W_OK) != 0) {
        printf("SKIP ctl: %s/ctl not writable (%s); run as root\n", PDIR,
               strerror(errno));
        return;
    }
    CHECK(ctl("add esh-test 4242\n") == 0);
    CHECK(stat_value("items:") == items0 + 1);
    s = slurp(PDIR "/items", 4096, &len, &calls);
    p = strstr(s, "esh-test");
    CHECK(p != NULL);
    while (p > s && p[-1] != '\n') /* back to the start of the line */
        p--;
    id = strtol(p, NULL, 10);
    CHECK(strstr(p, "4242") != NULL);
    free(s);

    snprintf(cmd, sizeof(cmd), "del %ld\n", id);
    CHECK(ctl(cmd) == 0);
    CHECK(stat_value("items:") == items0);
    CHECK(ctl(cmd) == ENOENT);          /* already gone */
    CHECK(ctl("explode\n") == EINVAL);  /* unknown command */
    printf("PASS ctl: add -> item %ld listed, del -> gone, ENOENT twice, EINVAL for junk\n", id);
}

/* ---- /proc/self: the kernel exporting process data ----------------------- */
static void test_proc_self(void)
{
    size_t len;
    int calls;
    char *s = slurp("/proc/self/status", 4096, &len, &calls);
    char want[32];

    snprintf(want, sizeof(want), "\nPid:\t%d\n", (int)getpid());
    CHECK(strstr(s, want) != NULL);
    free(s);
    printf("PASS /proc/self/status: shows our pid %d (generated on read by the kernel)\n",
           (int)getpid());
}

int main(void)
{
    if (access(PDIR "/items", R_OK) != 0) {
        fprintf(stderr, "%s missing (%s): load the module first:\n"
                        "  sudo insmod procfs_demo.ko\n", PDIR, strerror(errno));
        return 2;
    }
    test_items();
    test_ctl();
    test_proc_self();
    printf("all procfs_demo checks passed\n");
    return 0;
}
