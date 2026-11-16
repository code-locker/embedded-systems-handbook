/*
 * esh_sysfs_test.c — user-space test for sysfs_demo.ko: read/write/validate
 *                    attributes, poll() on a sysfs_notify()ed attribute,
 *                    device attributes, and the uevent over netlink
 *
 * Concept : sysfs attributes are plain files: write a value, read it back,
 *           and an out-of-range write fails with the errno store() returned
 *           (EINVAL). A sysfs file can also be poll()ed: the kernel side
 *           calls sysfs_notify(), user space waits for POLLPRI|POLLERR, then
 *           must lseek(0) and read() again. The protocol is: read once to
 *           arm, poll, rewind, re-read. udev hears about device changes via
 *           a NETLINK_KOBJECT_UEVENT socket; this program listens on one too.
 * Build   : make drivers   (-> build/linux-device-drivers/05-sysfs-procfs-debugfs/user/esh_sysfs_test)
 * Run     : sudo insmod kernel/sysfs_demo.ko
 *           sudo ./esh_sysfs_test
 *           Needs the module loaded (and root for the writes), so `make test`
 *           does NOT run it. Exit 0 = all CHECKs passed, 1 = a CHECK failed,
 *           2 = module not loaded / not root.
 * Read    : ../README.md  (sections 3.3 to 3.6)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define KDIR "/sys/kernel/esh_sysfs"
#define DDIR "/sys/class/misc/esh_sysfs"
#define DEV "/dev/esh_sysfs"

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* Read a whole attribute into buf (NUL-terminated). Returns length or -1. */
static ssize_t attr_read(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

/*
 * Write a string with ONE write() call, as echo does. Returns 0, or the
 * errno the kernel's store() returned (e.g. EINVAL).
 */
static int attr_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;
    int err = 0;

    if (fd < 0)
        return errno;
    n = write(fd, val, strlen(val));
    if (n < 0)
        err = errno;
    else if ((size_t)n != strlen(val))
        err = EIO;
    close(fd);
    return err;
}

static long attr_long(const char *path)
{
    char buf[64];

    CHECK(attr_read(path, buf, sizeof(buf)) > 0);
    return strtol(buf, NULL, 0);
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---- int attribute: parse, range check, errno propagation -------------- */
static void test_value(void)
{
    char buf[64];

    CHECK(attr_write(KDIR "/value", "123\n") == 0);
    CHECK(attr_long(KDIR "/value") == 123);
    CHECK(attr_write(KDIR "/value", "-1000") == 0); /* no '\n' is fine too */
    CHECK(attr_long(KDIR "/value") == -1000);
    CHECK(attr_write(KDIR "/value", "0x10") == 0);  /* base 0: hex accepted */
    CHECK(attr_long(KDIR "/value") == 16);
    CHECK(attr_write(KDIR "/value", "5000") == EINVAL); /* out of range */
    CHECK(attr_write(KDIR "/value", "12abc") == EINVAL); /* junk */
    CHECK(attr_long(KDIR "/value") == 16);              /* unchanged */
    CHECK(attr_read(KDIR "/value", buf, sizeof(buf)) == 3 &&
          strcmp(buf, "16\n") == 0);                    /* one value + '\n' */
    printf("PASS value: write/read, hex input, EINVAL for 5000 and \"12abc\"\n");
}

/* ---- string and enum attributes ---------------------------------------- */
static void test_name_mode(void)
{
    char buf[128];

    CHECK(attr_write(KDIR "/name", "ai64-board\n") == 0);
    CHECK(attr_read(KDIR "/name", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "ai64-board\n") == 0);
    CHECK(attr_write(KDIR "/name", "this-name-is-far-too-long-for-the-attribute") == EINVAL);
    CHECK(attr_write(KDIR "/name", "\n") == EINVAL);

    CHECK(attr_write(KDIR "/mode", "fast\n") == 0);
    CHECK(attr_read(KDIR "/mode", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "off slow [fast]\n") == 0);
    CHECK(attr_write(KDIR "/mode", "turbo") == EINVAL);
    CHECK(attr_write(KDIR "/mode", "slow") == 0);
    CHECK(attr_read(KDIR "/mode", buf, sizeof(buf)) > 0);
    CHECK(strcmp(buf, "off [slow] fast\n") == 0);
    printf("PASS name/mode: string with length check, enum shown as \"off [slow] fast\"\n");
}

/* ---- read-only / write-only permissions --------------------------------- */
static void test_perms(void)
{
    char buf[16];

    errno = 0;
    CHECK(open(KDIR "/event_count", O_WRONLY) < 0);  /* r--r--r-- */
    CHECK(attr_read(KDIR "/trigger", buf, sizeof(buf)) < 0); /* --w------- */
    printf("PASS perms: event_count is read-only, trigger is write-only\n");
}

/* ---- poll() on event_count, woken by sysfs_notify() --------------------- */
/*
 *   main                                   writer thread
 *   fd = open(event_count); read() -> N    (arms the notification)
 *   poll(POLLPRI|POLLERR) ... sleeping     sleep 200 ms
 *                                          write "1" > trigger
 *                                            -> kernel: sysfs_notify()
 *   wakes: revents = POLLPRI|POLLERR
 *   lseek(fd, 0); read() -> N+1            (must rewind: sysfs files are
 *                                           read from offset 0 each time)
 */
static void *delayed_trigger(void *arg)
{
    struct timespec d = { .tv_sec = 0, .tv_nsec = 200 * 1000 * 1000 };

    (void)arg;
    nanosleep(&d, NULL);
    CHECK(attr_write(KDIR "/trigger", "1\n") == 0);
    return NULL;
}

static void test_poll(void)
{
    struct pollfd p;
    pthread_t th;
    char buf[32];
    long before, after;
    double t0, waited;
    ssize_t n;
    int fd = open(KDIR "/event_count", O_RDONLY);

    CHECK(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1); /* arm */
    CHECK(n > 0);
    buf[n] = '\0';
    before = strtol(buf, NULL, 10);

    /* Nobody triggers: poll times out. POLLIN alone would return at once
     * (a regular file is always "readable"), so ask for POLLPRI only. */
    p.fd = fd;
    p.events = POLLPRI | POLLERR;
    CHECK(poll(&p, 1, 100) == 0);

    CHECK(pthread_create(&th, NULL, delayed_trigger, NULL) == 0);
    t0 = now_ms();
    CHECK(poll(&p, 1, 3000) == 1);
    waited = now_ms() - t0;
    CHECK(p.revents & (POLLPRI | POLLERR));
    CHECK(pthread_join(th, NULL) == 0);

    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    n = read(fd, buf, sizeof(buf) - 1);
    CHECK(n > 0);
    buf[n] = '\0';
    after = strtol(buf, NULL, 10);
    close(fd);
    CHECK(after == before + 1);
    CHECK(waited >= 150);
    printf("PASS poll: slept %.0f ms until sysfs_notify(), event_count %ld -> %ld\n",
           waited, before, after);
}

/* ---- device attributes on the misc device ------------------------------- */
static void test_device_attrs(void)
{
    char buf[64];
    int fd;
    ssize_t n;

    CHECK(attr_write(DDIR "/counter", "41") == 0);
    CHECK(attr_long(DDIR "/counter") == 41);

    /* read() on /dev/esh_sysfs returns the counter and increments it. */
    fd = open(DEV, O_RDONLY);
    CHECK(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    CHECK(n > 0);
    buf[n] = '\0';
    CHECK(strcmp(buf, "counter 41\n") == 0);
    CHECK(read(fd, buf, sizeof(buf)) == 0); /* then EOF */
    close(fd);
    CHECK(attr_long(DDIR "/counter") == 42);

    CHECK(attr_write(DDIR "/reset", "1") == 0);
    CHECK(attr_long(DDIR "/counter") == 0);
    CHECK(attr_write(DDIR "/reset", "maybe") == EINVAL);
    CHECK(attr_read(DDIR "/info", buf, sizeof(buf)) > 0);
    CHECK(strncmp(buf, "minor ", 6) == 0);
    printf("PASS device attrs: counter rw, read(/dev) bumps it, reset wo, info: %s", buf);
}

/* ---- uevent: what udev sees --------------------------------------------- */
/*
 * The kernel multicasts uevents on NETLINK_KOBJECT_UEVENT group 1. A message
 * is "ACTION@DEVPATH\0KEY=VALUE\0KEY=VALUE\0...". udevd (systemd-udevd)
 * listens to these and runs rules; `udevadm monitor --kernel --property`
 * prints them.
 */
static int uevent_has(const char *msg, size_t len, const char *needle)
{
    size_t off = 0;

    while (off < len) {
        if (strcmp(msg + off, needle) == 0)
            return 1;
        off += strlen(msg + off) + 1;
    }
    return 0;
}

static void test_uevent(void)
{
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200 * 1000 };
    char msg[4096], want[32];
    long next;
    int s, got = 0, tries;
    ssize_t n;

    s = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        printf("SKIP uevent: cannot listen on NETLINK_KOBJECT_UEVENT (%s)\n",
               strerror(errno));
        if (s >= 0)
            close(s);
        return;
    }
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);

    next = attr_long(KDIR "/event_count") + 1;
    snprintf(want, sizeof(want), "ESH_EVENT=%ld", next);
    CHECK(attr_write(KDIR "/trigger", "1") == 0);

    /* Other devices may send uevents too: look at up to 25 messages / 5 s. */
    for (tries = 0; tries < 25 && !got; tries++) {
        n = recv(s, msg, sizeof(msg) - 1, 0);
        if (n <= 0)
            continue;
        msg[n] = '\0';
        got = uevent_has(msg, (size_t)n, "ACTION=change") &&
              uevent_has(msg, (size_t)n, "SUBSYSTEM=misc") &&
              uevent_has(msg, (size_t)n, want);
    }
    close(s);
    CHECK(got);
    printf("PASS uevent: ACTION=change SUBSYSTEM=misc %s received over netlink\n", want);
}

int main(void)
{
    char buf[64];

    if (access(KDIR, F_OK) != 0 || access(DEV, F_OK) != 0) {
        fprintf(stderr, "%s or %s missing: load the module first:\n"
                        "  sudo insmod sysfs_demo.ko\n", KDIR, DEV);
        return 2;
    }
    if (access(KDIR "/value", W_OK) != 0) {
        fprintf(stderr, "cannot write %s/value (%s): run as root (sudo)\n",
                KDIR, strerror(errno));
        return 2;
    }
    CHECK(attr_read(KDIR "/raw", buf, sizeof(buf)) == 48); /* binary, fixed size */

    test_value();
    test_name_mode();
    test_perms();
    test_poll();
    test_device_attrs();
    test_uevent();
    printf("all sysfs_demo checks passed\n");
    return 0;
}
