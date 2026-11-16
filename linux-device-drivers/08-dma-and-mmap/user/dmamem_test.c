/*
 * dmamem_test.c — user-space test for dmamem_demo.ko (and dmamem_fault.ko):
 *                 mmap a driver's DMA buffer, write a pattern, let the driver
 *                 copy it with a DMA engine (or CPU), verify through the mapping
 *
 * Concept : After mmap() the driver's buffer is ordinary memory to this
 *           process: no read()/write() copies. The program
 *             1. DMAMEM_IOC_INFO   -> buffer size, DMA channel name
 *             2. mmap(MAP_SHARED)  -> pointer to the same pages the device uses
 *             3. writes a pattern into the first half through the pointer
 *             4. DMAMEM_IOC_COPY   -> driver copies first half -> second half
 *                                     (K3 UDMA on the board, memcpy on a PC)
 *             5. checks the second half through the pointer: no read() needed
 *           Then, if /dev/dmamem_fault exists, it checks that the
 *           page-by-page mapping and read() see the same bytes.
 * Build   : make drivers  (-> build/linux-device-drivers/08-dma-and-mmap/user/dmamem_test)
 * Run     : sudo insmod kernel/dmamem_demo.ko ; sudo insmod kernel/dmamem_fault.ko
 *           sudo ./dmamem_test
 *           Exit 0 = all CHECKs passed, 1 = a CHECK failed, 2 = no device.
 *           Needs the module loaded, so `make test` does NOT run it.
 * Read    : ../README.md  (section 3.8 "mmap in drivers")
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/ioctl.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%s\n", #cond,   \
                    __FILE__, __LINE__, strerror(errno));                    \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* ---- ABI copied from kernel/dmamem_demo.c (keep in sync) ---------------- */
struct dmamem_info {
    __u64 size;
    __u32 dmaengine;
    __u32 reserved;
    char chan_name[32];
};

struct dmamem_copy {
    __u64 src_off;
    __u64 dst_off;
    __u64 len;
    __u32 used_dmaengine;
    __u32 reserved;
    __u64 nsec;
};

#define DMAMEM_IOC_MAGIC 'D'
#define DMAMEM_IOC_INFO _IOR(DMAMEM_IOC_MAGIC, 1, struct dmamem_info)
#define DMAMEM_IOC_COPY _IOWR(DMAMEM_IOC_MAGIC, 2, struct dmamem_copy)
/* -------------------------------------------------------------------------- */

static void test_dmamem(int fd)
{
    struct dmamem_info info;
    struct dmamem_copy c;
    uint8_t *p;
    size_t half, i;

    memset(&info, 0, sizeof(info));
    CHECK(ioctl(fd, DMAMEM_IOC_INFO, &info) == 0);
    info.chan_name[sizeof(info.chan_name) - 1] = '\0';
    CHECK(info.size >= 8192 && info.size % 4096 == 0);
    printf("/dev/dmamem: %llu bytes, DMA channel: %s\n",
           (unsigned long long)info.size, info.chan_name);

    /* MAP_PRIVATE is refused by the driver (copy-on-write of device memory) */
    CHECK(mmap(NULL, info.size, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED);
    /* one byte past the buffer must be refused too */
    CHECK(mmap(NULL, info.size + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) ==
          MAP_FAILED);

    p = mmap(NULL, info.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    half = info.size / 2;

    for (i = 0; i < half; i++)
        p[i] = (uint8_t)(i * 13 + 7);  /* pattern, written via the mapping */
    memset(p + half, 0, half);

    /* bad requests: overlap and out of range */
    memset(&c, 0, sizeof(c));
    c.src_off = 0;
    c.dst_off = 16;
    c.len = 64;
    CHECK(ioctl(fd, DMAMEM_IOC_COPY, &c) < 0 && errno == EINVAL);
    c.dst_off = info.size - 32;
    CHECK(ioctl(fd, DMAMEM_IOC_COPY, &c) < 0 && errno == EINVAL);

    c.src_off = 0;
    c.dst_off = half;
    c.len = half;
    CHECK(ioctl(fd, DMAMEM_IOC_COPY, &c) == 0);
    /*
     * No read(), no cache flush call: the driver's coherent buffer is
     * mapped with the right attributes, so the device's writes are
     * visible here directly.
     */
    CHECK(memcmp(p, p + half, half) == 0);
    printf("PASS copy: %zu bytes by %s in %llu us, verified through the mapping\n",
           half, c.used_dmaengine ? "the DMA engine" : "CPU memcpy (no DMA_MEMCPY channel)",
           (unsigned long long)(c.nsec / 1000));
    CHECK(munmap(p, info.size) == 0);
}

static void test_fault(int fd)
{
    const size_t len = 2 * 4096; /* two pages: two separate faults */
    uint8_t buf[64];
    uint8_t *p;

    p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("INFO /dev/dmamem_fault: mmap of 2 pages failed (%s), pages=1?\n",
               strerror(errno));
        return;
    }
    memset(p, 0xa1, 4096);         /* first touch of page 0 -> .fault */
    memcpy(p + 4096, "page-two", 8); /* page 1 */

    CHECK(pread(fd, buf, 8, 4096) == 8); /* same page via copy_to_user */
    CHECK(memcmp(buf, "page-two", 8) == 0);
    CHECK(pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf));
    CHECK(buf[0] == 0xa1 && buf[63] == 0xa1);

    CHECK(pwrite(fd, "via-write", 9, 100) == 9); /* and the other way */
    CHECK(memcmp(p + 100, "via-write", 9) == 0);
    printf("PASS /dev/dmamem_fault: mmap and read()/write() see the same pages\n");
    CHECK(munmap(p, len) == 0);
}

int main(void)
{
    int fd = open("/dev/dmamem", O_RDWR | O_CLOEXEC);
    int ffd;

    if (fd < 0) {
        fprintf(stderr, "/dev/dmamem: %s\n"
                "  load the module first: sudo insmod kernel/dmamem_demo.ko\n"
                "  (and run as root: the node is mode 0600)\n", strerror(errno));
        return 2;
    }
    test_dmamem(fd);
    close(fd);

    ffd = open("/dev/dmamem_fault", O_RDWR | O_CLOEXEC);
    if (ffd >= 0) {
        test_fault(ffd);
        close(ffd);
    } else {
        printf("INFO /dev/dmamem_fault not present (insmod dmamem_fault.ko to test it)\n");
    }
    printf("done: all checks passed\n");
    return 0;
}
