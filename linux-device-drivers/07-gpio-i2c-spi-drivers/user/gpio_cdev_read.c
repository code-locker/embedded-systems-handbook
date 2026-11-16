/*
 * gpio_cdev_read.c — list GPIO chips and read a line with the GPIO v2
 *                    character-device uAPI (raw ioctls, no libgpiod)
 *
 * Concept : Since Linux 4.8 user space talks to GPIOs through
 *           /dev/gpiochipN; the old /sys/class/gpio interface is deprecated
 *           (global numbers, no ownership, no cleanup when a program dies).
 *           This program does by hand what gpioinfo / gpioget / gpiomon do:
 *             GPIO_GET_CHIPINFO_IOCTL       name, label, number of lines
 *             GPIO_V2_GET_LINEINFO_IOCTL    per line: name, consumer, flags
 *             GPIO_V2_GET_LINE_IOCTL        request lines -> a NEW fd
 *             GPIO_V2_LINE_GET_VALUES_IOCTL read them through that fd
 *             read(fd) of gpio_v2_line_event   edge events with timestamps
 *           Released lines go back automatically when the request fd closes
 *           (also when the process crashes).
 *           Safety: on real chips it only LISTS lines unless you name one,
 *           because even requesting an input can reconfigure a pad. On a
 *           gpio-sim chip (VM) it also reads a line and checks it against
 *           the simulated pull, and checks a rising-edge event.
 * Build   : make drivers  (-> build/linux-device-drivers/07-gpio-i2c-spi-drivers/user/gpio_cdev_read)
 * Run     : sudo ./gpio_cdev_read                   list all chips (+ gpio-sim self-test)
 *           sudo ./gpio_cdev_read /dev/gpiochip1 4  also read line 4 of gpiochip1
 *           Exit 0 = OK, 1 = a CHECK failed, 2 = no accessible /dev/gpiochip*.
 *           Not run by `make test` (needs hardware or gpio-sim, usually root).
 * Read    : ../README.md  (section 3.1 "GPIO", user-space part)
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define MAX_LIST 12 /* lines printed per chip; real SoC chips have 100+ */

/* A gpio-sim chip's sysfs dir lives under .../platform/gpio-sim.N/ */
static int is_gpio_sim(const char *chip)
{
    char path[320], link[512];
    ssize_t n;

    snprintf(path, sizeof(path), "/sys/bus/gpio/devices/%s", chip);
    n = readlink(path, link, sizeof(link) - 1);
    if (n < 0)
        return 0;
    link[n] = '\0';
    return strstr(link, "gpio-sim") != NULL;
}

/* gpio-sim lets us set the "outside world" level of a line: its pull. */
static int sim_set_pull(const char *chip, unsigned int off, const char *pull)
{
    char path[320];
    FILE *f;
    int ok;

    snprintf(path, sizeof(path), "/sys/bus/gpio/devices/%s/sim_gpio%u/pull",
             chip, off);
    f = fopen(path, "w");
    if (!f)
        return -1;
    ok = fputs(pull, f) >= 0;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static void print_flags(__u64 f)
{
    printf("%s%s%s%s%s%s",
           (f & GPIO_V2_LINE_FLAG_USED) ? " used" : "",
           (f & GPIO_V2_LINE_FLAG_OUTPUT) ? " output" : "",
           (f & GPIO_V2_LINE_FLAG_INPUT) ? " input" : "",
           (f & GPIO_V2_LINE_FLAG_ACTIVE_LOW) ? " active-low" : "",
           (f & GPIO_V2_LINE_FLAG_BIAS_PULL_UP) ? " pull-up" : "",
           (f & GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN) ? " pull-down" : "");
}

/* List one chip; returns number of lines or -1. */
static int list_chip(int fd, const char *devname)
{
    struct gpiochip_info ci;
    unsigned int i;

    memset(&ci, 0, sizeof(ci));
    CHECK(ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &ci) == 0);
    printf("%s: label \"%s\", %u lines\n", ci.name, ci.label, ci.lines);
    CHECK(ci.lines > 0);
    CHECK(strcmp(ci.name, devname) == 0); /* name == /dev node name */

    for (i = 0; i < ci.lines && i < MAX_LIST; i++) {
        struct gpio_v2_line_info li;

        memset(&li, 0, sizeof(li)); /* uAPI rule: padding must be zero */
        li.offset = i;
        CHECK(ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &li) == 0);
        CHECK(li.offset == i);
        printf("  line %3u: %-20s %-16s", i, li.name[0] ? li.name : "unnamed",
               li.consumer[0] ? li.consumer : "-");
        print_flags(li.flags);
        printf("\n");
    }
    if (ci.lines > MAX_LIST)
        printf("  ... %u more lines (gpioinfo %s shows all)\n",
               ci.lines - MAX_LIST, devname);
    return (int)ci.lines;
}

/* Request one line with the given flags; returns the request fd. */
static int request_line(int chip_fd, unsigned int off, __u64 flags)
{
    struct gpio_v2_line_request req;

    memset(&req, 0, sizeof(req));
    req.offsets[0] = off;
    req.num_lines = 1;
    req.config.flags = flags;
    snprintf(req.consumer, sizeof(req.consumer), "gpio_cdev_read");
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &req) < 0)
        return -1;
    return req.fd; /* the kernel filled in a new anonymous fd */
}

static int read_value(int req_fd)
{
    struct gpio_v2_line_values v;

    memset(&v, 0, sizeof(v));
    v.mask = 1; /* bit i = i-th line of THIS request, not the offset */
    CHECK(ioctl(req_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v) == 0);
    return (int)(v.bits & 1);
}

/* gpio-sim self-test: pull decides the input level; edges become events. */
static void sim_selftest(int chip_fd, const char *chip, unsigned int nlines)
{
    unsigned int off = nlines > 1 ? 1 : 0;
    struct gpio_v2_line_event ev;
    struct pollfd pfd;
    int rfd, v;

    if (sim_set_pull(chip, off, "pull-down") < 0) {
        printf("%s: gpio-sim, but sim_gpio%u/pull not writable (run as root)\n",
               chip, off);
        return;
    }
    rfd = request_line(chip_fd, off, GPIO_V2_LINE_FLAG_INPUT);
    if (rfd < 0 && errno == EBUSY) {
        printf("%s line %u is used by a driver (esh_gpio_demo?): skipped\n",
               chip, off);
        return;
    }
    CHECK(rfd >= 0);
    v = read_value(rfd);
    CHECK(v == 0);
    CHECK(sim_set_pull(chip, off, "pull-up") == 0);
    v = read_value(rfd);
    CHECK(v == 1);
    close(rfd); /* line released here */
    printf("PASS %s line %u: pull-down -> 0, pull-up -> 1\n", chip, off);

    /* Edge events: request with EDGE_RISING, then make an edge. */
    CHECK(sim_set_pull(chip, off, "pull-down") == 0);
    rfd = request_line(chip_fd, off,
                       GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING);
    CHECK(rfd >= 0);
    CHECK(sim_set_pull(chip, off, "pull-up") == 0);
    pfd.fd = rfd;
    pfd.events = POLLIN;
    CHECK(poll(&pfd, 1, 1000) == 1); /* 1 s timeout: never hang */
    CHECK(read(rfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev));
    CHECK(ev.id == GPIO_V2_LINE_EVENT_RISING_EDGE);
    CHECK(ev.offset == off);
    printf("PASS %s line %u: rising-edge event, timestamp %llu ns, seqno %u\n",
           chip, off, (unsigned long long)ev.timestamp_ns, ev.seqno);
    close(rfd);
    sim_set_pull(chip, off, "pull-down");
}

static int cmp_names(const struct dirent **a, const struct dirent **b)
{
    return strcmp((*a)->d_name, (*b)->d_name);
}

static int is_chip(const struct dirent *d)
{
    return strncmp(d->d_name, "gpiochip", 8) == 0;
}

int main(int argc, char **argv)
{
    struct dirent **names;
    int n, i, opened = 0, denied = 0;

    if (argc == 3) { /* explicit: read exactly this line, input mode */
        unsigned int off = (unsigned int)strtoul(argv[2], NULL, 0);
        int fd = open(argv[1], O_RDONLY | O_CLOEXEC), rfd;

        if (fd < 0) {
            fprintf(stderr, "%s: %s (try sudo)\n", argv[1], strerror(errno));
            return 2;
        }
        rfd = request_line(fd, off, GPIO_V2_LINE_FLAG_INPUT);
        if (rfd < 0) {
            fprintf(stderr, "request line %u: %s%s\n", off, strerror(errno),
                    errno == EBUSY ? " (a driver or another process owns it)" : "");
            return 1;
        }
        printf("%s line %u = %d\n", argv[1], off, read_value(rfd));
        close(rfd);
        close(fd);
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: %s [/dev/gpiochipN OFFSET]\n", argv[0]);
        return 2;
    }

    n = scandir("/dev", &names, is_chip, cmp_names);
    for (i = 0; i < n; i++) {
        char path[300];
        int fd, lines;

        snprintf(path, sizeof(path), "/dev/%s", names[i]->d_name);
        fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES)
                denied++;
            free(names[i]);
            continue;
        }
        opened++;
        lines = list_chip(fd, names[i]->d_name);
        if (is_gpio_sim(names[i]->d_name))
            sim_selftest(fd, names[i]->d_name, (unsigned int)lines);
        close(fd);
        free(names[i]);
    }
    if (n >= 0)
        free(names);

    if (!opened) {
        fprintf(stderr,
                "no accessible /dev/gpiochip* (%d found, %d permission denied).\n"
                "  board: run with sudo (or a udev rule giving group 'gpio' access)\n"
                "  VM:    sudo modprobe gpio-sim and create a bank via configfs,\n"
                "         see ../README.md section 8\n",
                n < 0 ? 0 : n, denied);
        return 2;
    }
    printf("done: %d chip(s) listed. Real chips are only listed; read a line with\n"
           "      %s /dev/gpiochipN OFFSET\n", opened, argv[0]);
    return 0;
}
