/*
 * 07_ioctl_proc_sys.c — ioctl(), the "everything else" system call, and the
 *                       kernel's file-shaped interfaces /proc and /sys
 *
 * Concept : read/write move bytes. Everything a device can do that is not
 *           "bytes" (set the baud rate, ask how many bytes are queued, get
 *           the window size, start a DMA) goes through ioctl(fd, request,
 *           arg). The request number encodes direction, a "magic" type
 *           letter, a command number and the argument size: _IOR('f', 1,
 *           long). Drivers you write in linux-device-drivers/01 implement
 *           .unlocked_ioctl for exactly this.
 *           /proc and /sys are not on any disk: reading a file there calls
 *           a kernel function that formats the answer on the spot. /proc is
 *           per-process info (+ old misc knobs), /sys is the device model
 *           (one value per file).
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/07_ioctl_proc_sys.c
 * Read    : ../README.md  (sections "3.12 ioctl" and "3.13 /proc and /sys")
 */
#define _GNU_SOURCE /* mkstemp, pipe2 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h> /* FS_IOC_GETFLAGS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
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

/* ---- 1. ioctl on a pipe, a terminal, a file ---- */
static void demo_ioctl(void)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    char data[123];
    memset(data, 'q', sizeof data);
    CHECK(write(p[1], data, sizeof data) == (ssize_t)sizeof data);
    int queued = -1;
    CHECK(ioctl(p[0], FIONREAD, &queued) == 0); /* "how much can I read?" */
    printf("1) ioctl(pipe, FIONREAD) = %d bytes waiting\n", queued);
    CHECK(queued == (int)sizeof data);
    close(p[0]);
    close(p[1]);

    /* TIOCGWINSZ only makes sense on a terminal. Under `make test` stdout
     * is a pipe -> ENOTTY ("inappropriate ioctl for device"): the driver
     * behind the fd does not know this request. */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0)
        printf("   ioctl(stdout, TIOCGWINSZ): terminal is %u cols x %u "
               "rows\n", ws.ws_col, ws.ws_row);
    else
        printf("   ioctl(stdout, TIOCGWINSZ) -> %s (stdout is not a "
               "terminal)\n", strerror(errno));

    /* The request number is a packed descriptor:
     *   bits 31-30 dir (R/W)  29-16 size  15-8 type ('f')  7-0 nr */
    unsigned long req = FS_IOC_GETFLAGS;
    printf("   FS_IOC_GETFLAGS = 0x%08lx: dir=%lu(%s) type='%c' nr=%lu "
           "size=%lu\n", req, (unsigned long)_IOC_DIR(req),
           _IOC_DIR(req) == _IOC_READ ? "read" : "?",
           (char)_IOC_TYPE(req), (unsigned long)_IOC_NR(req),
           (unsigned long)_IOC_SIZE(req));
    CHECK(_IOC_TYPE(req) == 'f' && _IOC_DIR(req) == _IOC_READ);

    char path[64];
    snprintf(path, sizeof path, "/tmp/ioctl_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    int flags = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0)
        printf("   ioctl(file, FS_IOC_GETFLAGS) = 0x%x (what lsattr shows)\n",
               flags);
    else
        printf("   ioctl(file, FS_IOC_GETFLAGS) -> %s (this filesystem has "
               "no inode flags)\n", strerror(errno));
    close(fd);
    CHECK(unlink(path) == 0);
}

/* ---- 2. /proc/self/status ---- */
static void demo_proc_status(void)
{
    FILE *f = fopen("/proc/self/status", "r");
    CHECK(f != NULL);
    char line[256];
    long pid = -1;
    printf("2) /proc/self/status (selected lines):\n");
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Name:", 5) || !strncmp(line, "Pid:", 4) ||
            !strncmp(line, "PPid:", 5) || !strncmp(line, "Threads:", 8) ||
            !strncmp(line, "VmRSS:", 6) || !strncmp(line, "FDSize:", 7))
            printf("     %s", line);
        if (!strncmp(line, "Pid:", 4))
            pid = strtol(line + 4, NULL, 10);
    }
    fclose(f);
    CHECK(pid == getpid()); /* the kernel formatted it for THIS process */
}

/* Count entries in /proc/self/fd (minus the fd opendir itself uses). */
static int count_fds(int verbose, const char *want, int *found)
{
    DIR *d = opendir("/proc/self/fd");
    CHECK(d != NULL);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        int fd = atoi(e->d_name);
        if (fd == dirfd(d))
            continue;
        char link[300], target[256]; /* d_name is up to 255 chars */
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t r = readlink(link, target, sizeof target - 1);
        if (r < 0)
            continue;
        target[r] = '\0';
        if (want && strcmp(target, want) == 0)
            (*found)++;
        if (verbose)
            printf("     fd %-3d -> %s\n", fd, target);
        n++;
    }
    closedir(d);
    return n;
}

/* ---- 3. /proc/self/fd: your fd table as a directory ---- */
static void demo_proc_fd(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/procfd_%d_XXXXXX", (int)getpid());
    int base = count_fds(0, NULL, NULL);
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    int d1 = dup(fd);
    int p[2];
    CHECK(d1 >= 0 && pipe2(p, O_CLOEXEC) == 0);
    int found = 0;
    printf("3) /proc/self/fd after mkstemp + dup + pipe:\n");
    int now = count_fds(1, path, &found);
    printf("   %d fds before, %d now; the temp file appears %d times\n",
           base, now, found);
    CHECK(now == base + 4);
    CHECK(found == 2);
    close(fd);
    close(d1);
    close(p[0]);
    close(p[1]);
    CHECK(unlink(path) == 0);
    CHECK(count_fds(0, NULL, NULL) == base);

    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
    printf("   RLIMIT_NOFILE: soft %llu, hard %llu (see /proc/self/limits; "
           "the _bug program hits it)\n", (unsigned long long)rl.rlim_cur,
           (unsigned long long)rl.rlim_max);
}

/* ---- 4. /sys: the device model, one value per file ---- */
static void demo_sys(void)
{
    DIR *d = opendir("/sys/class");
    if (!d) {
        printf("4) /sys/class not available (%s): skipped\n", strerror(errno));
        return;
    }
    int n = 0;
    struct dirent *e;
    printf("4) /sys/class has device classes like:");
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        if (n < 10)
            printf(" %s", e->d_name);
        n++;
    }
    closedir(d);
    printf(" ... (%d total)\n", n);
    CHECK(n > 0);

    /* One value per file: read it like any file. */
    FILE *f = fopen("/sys/class/net/lo/mtu", "r");
    if (f) {
        int mtu = 0;
        CHECK(fscanf(f, "%d", &mtu) == 1);
        fclose(f);
        printf("   /sys/class/net/lo/mtu = %d (the same number `ip link` "
               "shows)\n", mtu);
        CHECK(mtu > 0);
    }
    f = fopen("/proc/sys/kernel/osrelease", "r");
    if (f) {
        char rel[128] = {0};
        CHECK(fgets(rel, sizeof rel, f) != NULL);
        fclose(f);
        printf("   /proc/sys/kernel/osrelease = %s", rel);
    }
}

int main(void)
{
    alarm(10);
    demo_ioctl();
    demo_proc_status();
    demo_proc_fd();
    demo_sys();
    printf("07_ioctl_proc_sys: all checks passed\n");
    return 0;
}
