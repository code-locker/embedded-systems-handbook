/*
 * 08_dma_heap.c — DMA-BUF from user space: allocate a buffer from a DMA heap
 *                 (/dev/dma_heap/system) or udmabuf, map it, bracket CPU
 *                 access with DMA_BUF_IOCTL_SYNC, and share the fd with a
 *                 child process
 *
 * Concept : A DMA-BUF is a buffer represented by a FILE DESCRIPTOR, so it
 *           can be handed between drivers and processes without copying:
 *           the camera (V4L2 VIDIOC_EXPBUF) exports a frame as a dmabuf fd,
 *           the GPU or the encoder imports the same fd, and the pixels never
 *           pass through the CPU. User space can also allocate one itself
 *           from a DMA heap: open /dev/dma_heap/<name>, ioctl
 *           DMA_HEAP_IOCTL_ALLOC, get an fd. Mapping it gives CPU access;
 *           DMA_BUF_IOCTL_SYNC START/END around that access lets the
 *           exporter flush/invalidate caches (on the A72, devices are not
 *           always cache-coherent). On a desktop the heap nodes are usually
 *           root-only: then the program lists what exists, explains, and
 *           skips. On the board (root) it runs fully.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/08_dma_heap.c
 * Read    : ../README.md  (section 3.9 "DMA-BUF and DMA heaps")
 */
#define _GNU_SOURCE /* memfd_create, F_ADD_SEALS */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/udmabuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define BUF_SIZE (1u << 20) /* 1 MiB, page-aligned as both allocators require */

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do
        r = ioctl(fd, req, arg);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static void list_heaps(void)
{
    DIR *d = opendir("/dev/dma_heap");
    if (!d) {
        printf("   /dev/dma_heap: absent (kernel without CONFIG_DMABUF_HEAPS)\n");
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[300];
        struct stat st;
        snprintf(p, sizeof p, "/dev/dma_heap/%s", e->d_name);
        if (stat(p, &st) == 0)
            printf("   %-28s mode %04o uid %u gid %u  %s\n", p, (unsigned)(st.st_mode & 07777),
                   (unsigned)st.st_uid, (unsigned)st.st_gid,
                   access(p, R_OK) == 0 ? "(accessible)" : "(no access for this user)");
    }
    closedir(d);
}

/* Path A: a DMA heap. "system" = ordinary pages (scattered); a CMA heap
 * ("linux,cma" / "reserved") = physically contiguous, for devices without
 * an IOMMU. Returns the dmabuf fd or -errno. */
static int alloc_from_heap(const char *heap)
{
    int h = open(heap, O_RDONLY | O_CLOEXEC);
    if (h < 0)
        return -errno;
    struct dma_heap_allocation_data a = {
        .len = BUF_SIZE,
        .fd_flags = O_RDWR | O_CLOEXEC, /* flags of the NEW dmabuf fd */
        .heap_flags = 0,
    };
    int r = xioctl(h, DMA_HEAP_IOCTL_ALLOC, &a);
    int e = errno;
    close(h);
    return r < 0 ? -e : (int)a.fd;
}

/* Path B: udmabuf turns a sealed memfd into a dmabuf (used by QEMU and for
 * testing importers). The memfd must be sealed against shrinking. */
static int alloc_from_udmabuf(void)
{
    int dev = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (dev < 0)
        return -errno;
    int mfd = memfd_create("udmabuf-backing", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(mfd >= 0);
    CHECK(ftruncate(mfd, BUF_SIZE) == 0);
    CHECK(fcntl(mfd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
    struct udmabuf_create c = {.memfd = (uint32_t)mfd, .flags = UDMABUF_FLAGS_CLOEXEC,
                               .offset = 0, .size = BUF_SIZE};
    int buf = xioctl(dev, UDMABUF_CREATE, &c); /* returns the new dmabuf fd */
    int e = errno;
    close(mfd); /* the dmabuf holds its own reference to the pages */
    close(dev);
    return buf < 0 ? -e : buf;
}

static void buf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync s = {.flags = flags};
    CHECK(xioctl(fd, DMA_BUF_IOCTL_SYNC, &s) == 0);
}

/* What every dmabuf user does: name it, map it, sync around CPU access,
 * pass the fd on. */
static void use_dmabuf(int fd)
{
    /* A name shows up in /proc/<pid>/fdinfo and debugfs: invaluable when
     * hunting leaked buffers in a camera pipeline. */
    if (xioctl(fd, DMA_BUF_SET_NAME_B, (void *)"handbook-demo") != 0)
        printf("   DMA_BUF_SET_NAME: %s (older kernel)\n", strerror(errno));

    char link[64], target[128] = "";
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, target, sizeof target - 1);
    if (n > 0)
        target[n] = '\0';
    printf("   dmabuf fd %d -> \"%s\"\n", fd, target);
    snprintf(link, sizeof link, "/proc/self/fdinfo/%d", fd);
    FILE *fi = fopen(link, "r");
    if (fi) {
        char line[128];
        while (fgets(line, sizeof line, fi))
            if (!strncmp(line, "size", 4) || !strncmp(line, "exp_name", 8) ||
                !strncmp(line, "name", 4))
                printf("     fdinfo %s", line);
        fclose(fi);
    }

    uint32_t *p = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    buf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE); /* CPU is about to write */
    for (size_t i = 0; i < BUF_SIZE / 4; i++)
        p[i] = (uint32_t)(i * 2654435761u);
    buf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);   /* flush for the device */
    printf("   mapped, wrote %u KiB between SYNC_START and SYNC_END\n", BUF_SIZE >> 10);

    /* The "importer": a child that only has the fd (in real life: another
     * process via SCM_RIGHTS, lesson 12, or a driver via an ioctl). */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        const uint32_t *q = mmap(NULL, BUF_SIZE, PROT_READ, MAP_SHARED, fd, 0);
        CHECK(q != MAP_FAILED);
        buf_sync(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        for (size_t i = 0; i < BUF_SIZE / 4; i++)
            CHECK(q[i] == (uint32_t)(i * 2654435761u));
        buf_sync(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        _exit(0);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("   child mapped the same fd and verified every word: shared, not copied\n");
    CHECK(munmap(p, BUF_SIZE) == 0);
    close(fd); /* last reference gone -> the exporter frees the pages */
}

int main(void)
{
    alarm(10);
    printf("1) DMA heaps on this system:\n");
    list_heaps();

    int fd = alloc_from_heap("/dev/dma_heap/system");
    const char *how = "/dev/dma_heap/system";
    if (fd < 0) {
        printf("2) DMA_HEAP_IOCTL_ALLOC on /dev/dma_heap/system: %s\n", strerror(-fd));
        fd = alloc_from_udmabuf();
        how = "/dev/udmabuf (sealed memfd)";
        if (fd < 0)
            printf("   /dev/udmabuf: %s\n", strerror(-fd));
    }
    if (fd < 0) {
        printf("   -> no dmabuf allocator is accessible to this user: skipping the\n"
               "      allocation. On the board run as root (or add a udev rule giving\n"
               "      group 'video' access to /dev/dma_heap/*, as camera stacks do).\n"
               "      Where you meet dmabufs without allocating them yourself:\n"
               "        V4L2:      VIDIOC_EXPBUF exports a capture buffer as an fd,\n"
               "                   V4L2_MEMORY_DMABUF imports one (zero-copy camera->encoder)\n"
               "        DRM/KMS:   PRIME handles <-> fds (GPU, display)\n"
               "        GStreamer: GstDmaBufMemory, io-mode=dmabuf / dmabuf-import\n");
    } else {
        printf("2) allocated %u KiB from %s\n", BUF_SIZE >> 10, how);
        use_dmabuf(fd);
    }
    printf("08_dma_heap: all checks passed\n");
    return 0;
}
