/*
 * 03_memfd_create.c — memfd_create(): anonymous shared memory as a file
 *                     descriptor, shared with a child, and SEALED so the
 *                     receiver can trust it
 *
 * Concept : memfd_create() returns an fd to an anonymous RAM file: no name
 *           in /dev/shm to clean up or collide with, freed automatically
 *           when the last fd and mapping are gone. You share it by giving
 *           the FD to the other process: by fork() inheritance (here), or
 *           to an unrelated process over a Unix socket with SCM_RIGHTS
 *           (lesson 12). Seals (F_ADD_SEALS) make it immutable in size or
 *           content, so a receiver does not have to fear the sender
 *           shrinking it (SIGBUS) or changing it while being read.
 *           This is how Wayland clients and Android (ashmem's successor)
 *           share buffers.
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/03_memfd_create.c
 * Read    : ../README.md  (section "3.3 memfd_create and sealing")
 */
#define _GNU_SOURCE /* memfd_create, F_ADD_SEALS */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
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

#define W 64
#define H 48 /* a tiny 8-bit grey "camera frame" */

static void wait_ok(pid_t pid)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main(void)
{
    alarm(10);
    const size_t size = W * H;

    /* MFD_CLOEXEC: not leaked into exec'd programs (fork still inherits).
     * MFD_ALLOW_SEALING: without it, F_ADD_SEALS fails with EPERM. */
    int fd = memfd_create("camera-frame", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, (off_t)size) == 0);

    char link[64], target[128];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t tl = readlink(link, target, sizeof target - 1);
    CHECK(tl > 0);
    target[tl] = '\0';
    printf("1) memfd_create -> fd %d, /proc/self/fd/%d -> \"%s\"\n", fd, fd,
           target);
    printf("   (no name in /dev/shm: nothing to unlink, nothing to collide)\n");

    /* Producer fills a "frame". */
    unsigned char *px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED,
                             fd, 0);
    CHECK(px != MAP_FAILED);
    for (size_t y = 0; y < H; y++)
        for (size_t x = 0; x < W; x++)
            px[y * W + x] = (unsigned char)(x * 4 + y);

    /* ---- 2. seal it: no more shrinking, growing or writing ----
     * F_SEAL_WRITE is refused while a WRITABLE shared mapping exists
     * (EBUSY), so the producer unmaps first. */
    CHECK(munmap(px, size) == 0);
    CHECK(fcntl(fd, F_ADD_SEALS,
                F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL) == 0);
    int seals = fcntl(fd, F_GET_SEALS);
    CHECK(seals & F_SEAL_WRITE);
    printf("2) sealed: F_GET_SEALS = 0x%x (SHRINK|GROW|WRITE|SEAL)\n", seals);

    errno = 0;
    CHECK(ftruncate(fd, 0) == -1 && errno == EPERM);
    errno = 0;
    CHECK(write(fd, "x", 1) == -1 && errno == EPERM);
    errno = 0;
    CHECK(mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) ==
              MAP_FAILED && errno == EPERM);
    printf("   ftruncate / write / writable mmap -> EPERM\n");

    /* ---- 3. consumer: a child that only has the inherited fd ----
     * It checks the seals BEFORE trusting the memory: that is the point of
     * sealing — the receiver verifies, it does not rely on promises. */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        int s = fcntl(fd, F_GET_SEALS);
        CHECK((s & (F_SEAL_SHRINK | F_SEAL_WRITE)) ==
              (F_SEAL_SHRINK | F_SEAL_WRITE));
        const unsigned char *ro = mmap(NULL, size, PROT_READ, MAP_SHARED,
                                       fd, 0);
        CHECK(ro != MAP_FAILED);
        unsigned long sum = 0;
        for (size_t i = 0; i < size; i++)
            sum += ro[i];
        unsigned long want = 0;
        for (size_t y = 0; y < H; y++)
            for (size_t x = 0; x < W; x++)
                want += (unsigned char)(x * 4 + y);
        CHECK(sum == want);
        printf("3) child mapped the sealed frame read-only, pixel sum %lu "
               "matches\n", sum);
        fflush(stdout);
        CHECK(munmap((void *)ro, size) == 0);
        _exit(0);
    }
    wait_ok(pid);

    /* The memory is freed when the last fd is closed and the last mapping
     * is gone — no shm_unlink step exists or is needed. */
    CHECK(close(fd) == 0);
    printf("4) close(fd): the frame is freed automatically\n");
    printf("   To give such an fd to an UNRELATED process, send it over a "
           "Unix socket with SCM_RIGHTS (lesson 12).\n");

    printf("03_memfd_create: all checks passed\n");
    return 0;
}
