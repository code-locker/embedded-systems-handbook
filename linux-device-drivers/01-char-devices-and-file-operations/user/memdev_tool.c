/*
 * memdev_tool.c — tiny command-line front end for /dev/memdevN:
 *                 the ioctls that `cat` and `echo` cannot send
 *
 * Concept : read()/write() are reachable from the shell (cat, echo, dd);
 *           ioctl() is not — every driver with ioctls ships a small tool
 *           like this (think `i2cset`, `v4l2-ctl`, `ethtool`). It includes
 *           the driver's ABI header, so the command numbers and struct
 *           layout can never drift from the driver's.
 * Build   : make drivers   (-> build/linux-device-drivers/01-char-devices-and-file-operations/user/memdev_tool)
 * Run     : memdev_tool [DEVICE] [COMMAND [ARG]]      DEVICE defaults to /dev/memdev0
 *             info            sizes and counters (MEMDEV_IOC_GET_STATS)   [default]
 *             clear           MEMDEV_IOC_CLEAR
 *             size N          MEMDEV_IOC_SET_SIZE to N bytes
 *             write TEXT      write TEXT at offset 0 (like echo -n TEXT >)
 *             read            print the contents (like cat)
 *             efault          read() into a PROT_NONE page: a correct driver
 *                             returns EFAULT; memdev_bug.ko oopses
 *           Exit 0 = ok, 1 = the operation failed, 2 = no device / bad usage.
 * Read    : ../README.md  (section 8 "Files in this lesson")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../kernel/memdev_ioctl.h"

static int usage(void)
{
    fprintf(stderr, "usage: memdev_tool [DEVICE] [info | clear | size N | write TEXT | read | efault]\n");
    return 2;
}

static int fail(const char *what)
{
    fprintf(stderr, "%s: %s\n", what, strerror(errno));
    return 1;
}

static int cmd_info(int fd, const char *dev)
{
    struct memdev_stats st;

    if (ioctl(fd, MEMDEV_IOC_GET_STATS, &st) < 0)
        return fail("ioctl(MEMDEV_IOC_GET_STATS)"); /* ENOTTY on other devices */
    printf("%s: size %" PRIu64 " B, len %" PRIu64 " B, reads %" PRIu64
           ", writes %" PRIu64 ", openers %u%s\n",
           dev, (uint64_t)st.size, (uint64_t)st.len, (uint64_t)st.dev_reads,
           (uint64_t)st.dev_writes, st.openers, st.exclusive ? ", exclusive" : "");
    return 0;
}

static int cmd_read(int fd)
{
    char buf[4096];
    ssize_t n;

    while ((n = read(fd, buf, sizeof(buf))) > 0) /* until read() returns 0 = EOF */
        if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n)
            return fail("stdout");
    return n < 0 ? fail("read") : 0;
}

static int cmd_efault(int fd)
{
    long pg = sysconf(_SC_PAGESIZE);
    void *bad = mmap(NULL, (size_t)pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ssize_t n;

    if (bad == MAP_FAILED)
        return fail("mmap");
    n = read(fd, bad, 16);
    if (n < 0 && errno == EFAULT)
        printf("read into a PROT_NONE page -> EFAULT: the driver uses copy_to_user()\n");
    else if (n < 0)
        printf("read -> %s (device empty or no read support?)\n", strerror(errno));
    else
        printf("read returned %zd: nothing faulted (empty device?)\n", n);
    munmap(bad, (size_t)pg);
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/memdev0", *cmd = "info", *arg = NULL;
    int i = 1, fd, ret = 2;

    if (i < argc && argv[i][0] == '/')
        dev = argv[i++];
    if (i < argc)
        cmd = argv[i++];
    if (i < argc)
        arg = argv[i++];
    if (i < argc)
        return usage();

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\nload the module first: sudo insmod memdev.ko\n",
                dev, strerror(errno));
        return 2;
    }

    if (!strcmp(cmd, "info") && !arg) {
        ret = cmd_info(fd, dev);
    } else if (!strcmp(cmd, "clear") && !arg) {
        ret = ioctl(fd, MEMDEV_IOC_CLEAR) < 0 ? fail("ioctl(MEMDEV_IOC_CLEAR)") : 0;
    } else if (!strcmp(cmd, "size") && arg) {
        char *end;
        __u64 v;

        errno = 0;
        v = strtoull(arg, &end, 0);
        if (errno || *end || end == arg)
            ret = usage();
        else
            ret = ioctl(fd, MEMDEV_IOC_SET_SIZE, &v) < 0 ? fail("ioctl(MEMDEV_IOC_SET_SIZE)")
                                                         : cmd_info(fd, dev);
    } else if (!strcmp(cmd, "write") && arg) {
        ssize_t n = pwrite(fd, arg, strlen(arg), 0);

        if (n < 0)
            ret = fail("write");
        else
            ret = printf("wrote %zd of %zu bytes\n", n, strlen(arg)) < 0;
    } else if (!strcmp(cmd, "read") && !arg) {
        ret = cmd_read(fd);
    } else if (!strcmp(cmd, "efault") && !arg) {
        ret = cmd_efault(fd);
    } else {
        ret = usage();
    }
    close(fd);
    return ret;
}
