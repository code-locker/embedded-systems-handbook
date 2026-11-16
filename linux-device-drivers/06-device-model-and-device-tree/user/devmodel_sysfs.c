/*
 * devmodel_sysfs.c — the device model seen from user space: /sys/bus,
 *                    /sys/devices, driver and firmware-node links, modalias,
 *                    driver attributes, and the live device tree
 *
 * Concept : Every struct device is a directory in sysfs. Its "driver" link
 *           says who bound it, "modalias" is the string udev uses to load
 *           the right module, "of_node" (DT) or "software_node" points at the
 *           firmware description it was created from, and a driver's
 *           dev_groups attributes appear next to them. This program checks
 *           all of that for platdev_host_demo.ko (platdev-demo.0) and/or
 *           esh_demo_driver.ko (esh-demo + esh-demo-consumer), and on the
 *           board reads the overlay's node from /proc/device-tree.
 * Build   : make drivers   (-> build/linux-device-drivers/06-device-model-and-device-tree/user/devmodel_sysfs)
 * Run     : sudo insmod kernel/platdev_host_demo.ko          (VM / board)
 *           sudo insmod kernel/esh_demo_driver.ko [fake_devices=1]
 *           ./devmodel_sysfs        (as root it also writes ctrl_mode)
 *           Needs a module loaded, so `make test` does NOT run it.
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = nothing loaded.
 * Read    : ../README.md  (sections 3.1 "Bus, device, driver" and 3.9
 *           "Inspecting the live device tree")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PLAT "/sys/bus/platform/devices/"

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

/* Read a whole small sysfs file, strip the trailing newline. 0 on success. */
static int read_str(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    size_t n;

    if (!f)
        return -1;
    n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\0'))
        buf[--n] = '\0';
    return 0;
}

static int write_str(const char *path, const char *s)
{
    FILE *f = fopen(path, "w");
    int ok;

    if (!f)
        return -1;
    ok = fputs(s, f) >= 0;
    ok = (fclose(f) == 0) && ok;
    return ok ? 0 : -1;
}

/* basename of a symlink target, e.g. driver -> ../../bus/platform/drivers/esh-demo */
static int link_name(const char *path, char *buf, size_t len)
{
    char tgt[PATH_MAX];
    ssize_t n = readlink(path, tgt, sizeof(tgt) - 1);
    const char *b;

    if (n < 0)
        return -1;
    tgt[n] = '\0';
    b = strrchr(tgt, '/');
    snprintf(buf, len, "%s", b ? b + 1 : tgt);
    return 0;
}

static void show_device(const char *name, const char *want_driver, const char *want_alias)
{
    char path[PATH_MAX], buf[256], real[PATH_MAX];

    snprintf(path, sizeof(path), PLAT "%s", name);
    CHECK(realpath(path, real) != NULL);
    printf("\n%s\n  sysfs path:   %s\n", name, real);   /* /sys/devices/platform/... */

    snprintf(path, sizeof(path), PLAT "%s/driver", name);
    CHECK(link_name(path, buf, sizeof(buf)) == 0);  /* not bound -> no link */
    printf("  driver:       %s\n", buf);
    CHECK(strcmp(buf, want_driver) == 0);

    snprintf(path, sizeof(path), PLAT "%s/modalias", name);
    CHECK(read_str(path, buf, sizeof(buf)) == 0);
    printf("  modalias:     %s\n", buf);
    CHECK(strncmp(buf, want_alias, strlen(want_alias)) == 0);

    snprintf(path, sizeof(path), PLAT "%s/of_node", name);
    if (exists(path)) {
        CHECK(realpath(path, real) != NULL);
        printf("  of_node:      %s  (created from the device tree)\n", real);
    }
    snprintf(path, sizeof(path), PLAT "%s/software_node", name);
    if (exists(path)) {
        CHECK(link_name(path, buf, sizeof(buf)) == 0);
        printf("  software_node: %s  (created by a module, no DT)\n", buf);
    }
}

static void test_platdev(void)
{
    char buf[64];
    const char *dir = PLAT "platdev-demo.0/";

    show_device("platdev-demo.0", "platdev-demo", "platform:platdev-demo");
    CHECK(read_str(PLAT "platdev-demo.0/label", buf, sizeof(buf)) == 0);
    printf("  label:        %s\n", buf);
    CHECK(strcmp(buf, "host-demo") == 0);
    CHECK(read_str(PLAT "platdev-demo.0/ctrl_mode", buf, sizeof(buf)) == 0);
    printf("  ctrl_mode:    %s\n", buf);

    if (geteuid() == 0) {
        /* sysfs store -> ctrl_mode_store() -> read-modify-write of CTRL.MODE */
        CHECK(write_str(PLAT "platdev-demo.0/ctrl_mode", "5") == 0);
        CHECK(read_str(PLAT "platdev-demo.0/ctrl_mode", buf, sizeof(buf)) == 0);
        CHECK(strcmp(buf, "5") == 0);
        errno = 0;
        CHECK(write_str(PLAT "platdev-demo.0/ctrl_mode", "99") != 0);  /* > 15: -ERANGE */
        CHECK(write_str(PLAT "platdev-demo.0/ctrl_mode", "3") == 0);
        printf("PASS %s ctrl_mode write 5 / read 5 / 99 rejected\n", dir);
    } else {
        printf("INFO not root: skipping the ctrl_mode write test\n");
    }
    printf("PASS platdev-demo.0 bound, modalias and attributes ok\n");
}

/* alias: "of:" when created from DT (board), "platform:" for fake_devices=1 */
static void test_esh_demo(const char *alias)
{
    char buf[256];

    show_device("esh-demo", "esh-demo", alias);
    CHECK(read_str(PLAT "esh-demo/label", buf, sizeof(buf)) == 0);
    printf("  label:        %s\n", buf);
    CHECK(read_str(PLAT "esh-demo/frequency_hz", buf, sizeof(buf)) == 0);
    printf("  frequency_hz: %s\n", buf);
    CHECK(strcmp(buf, "1000000") == 0);
    CHECK(read_str(PLAT "esh-demo/thresholds", buf, sizeof(buf)) == 0);
    printf("  thresholds:   %s\n", buf);
    CHECK(strcmp(buf, "100 200 300") == 0);

    if (exists(PLAT "esh-demo-consumer")) {
        show_device("esh-demo-consumer", "esh-demo-consumer", alias);
        CHECK(read_str(PLAT "esh-demo-consumer/provider", buf, sizeof(buf)) == 0);
        printf("  provider:     %s\n", buf);
        CHECK(strncmp(buf, "esh-demo ", 9) == 0);
        /* the device link created in probe: consumer -> supplier */
        printf("  device link:  %s\n",
               exists(PLAT "esh-demo-consumer/supplier:platform:esh-demo")
                   ? "supplier:platform:esh-demo present" : "(not listed)");
    }

    /* Board: the overlay's node in the live DT (also under /sys/firmware/devicetree/base) */
    if (exists("/proc/device-tree/esh-demo/label")) {
        CHECK(read_str("/proc/device-tree/esh-demo/label", buf, sizeof(buf)) == 0);
        printf("  /proc/device-tree/esh-demo/label = \"%s\"\n", buf);
        printf("  esh-demo-off (status=disabled) has a DT node: %s, a device: %s\n",
               exists("/proc/device-tree/esh-demo-off") ? "yes" : "no",
               exists(PLAT "esh-demo-off") ? "yes (unexpected)" : "no");
        CHECK(!exists(PLAT "esh-demo-off"));
    }
    printf("PASS esh-demo bound, properties visible through sysfs\n");
}

int main(void)
{
    int have_plat = exists(PLAT "platdev-demo.0");
    int have_esh = exists(PLAT "esh-demo");

    if (!have_plat && !have_esh) {
        fprintf(stderr,
                "neither %splatdev-demo.0 nor %sesh-demo exists.\n"
                "Load a module first (VM or board, never your desktop):\n"
                "  sudo insmod platdev_host_demo.ko\n"
                "  sudo insmod esh_demo_driver.ko fake_devices=1   (board: apply the overlay instead)\n",
                PLAT, PLAT);
        return 2;
    }
    if (have_plat)
        test_platdev();
    if (have_esh)
        test_esh_demo(exists(PLAT "esh-demo/of_node") ? "of:" : "platform:");
    return 0;
}
