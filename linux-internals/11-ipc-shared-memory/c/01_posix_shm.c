/*
 * 01_posix_shm.c — POSIX shared memory: shm_open + ftruncate + mmap
 *                  (MAP_SHARED) + munmap + shm_unlink, between two processes
 *
 * Concept : shm_open() creates a named memory object (a file in the tmpfs
 *           /dev/shm). ftruncate() gives it a size, mmap(MAP_SHARED) maps
 *           the SAME physical pages into each process. After that, a plain
 *           store by one process is visible to loads in the other — no
 *           system call, no copy. That is why it is the fastest IPC, and
 *           also why it needs synchronisation: the kernel no longer
 *           mediates anything (here waitpid() is the synchronisation).
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/01_posix_shm.c
 * Read    : ../README.md  (section "3.1 POSIX shm_open + mmap")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* What we share. Fixed-size, no pointers (see 08_offsets_not_pointers.c). */
struct shared {
    uint32_t magic;
    uint32_t count;
    int32_t values[256];
    char note[64];
};

#define MAGIC 0x5348u /* "SH" */

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
    /* Name: "/" + no other slashes; unique per run so parallel runs and
     * leftovers from crashed runs do not collide. */
    char name[64];
    snprintf(name, sizeof name, "/shm_demo_%d", (int)getpid());

    /*
     *   process A                     kernel                    process B
     *   ---------                     ------                    ---------
     *   shm_open("/x") ----> /dev/shm/x (tmpfs inode) <---- shm_open("/x")
     *   ftruncate(4096)         | page frames |
     *   p = mmap(...) ------->  [  phys page  ]  <------- q = mmap(...)
     *   p->count = 5            (same RAM)               q->count == 5
     */
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    /* A new object has size 0: mapping it and touching it -> SIGBUS.
     * ftruncate sets the size (rounded up to pages when mapped); the new
     * bytes read as zero. */
    CHECK(ftruncate(fd, sizeof(struct shared)) == 0);
    struct shared *sh = mmap(NULL, sizeof *sh, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
    CHECK(sh != MAP_FAILED);
    /* The fd is no longer needed once mapped: the mapping keeps the object
     * alive. Closing early avoids leaking it into children/exec. */
    CHECK(close(fd) == 0);
    CHECK(sh->magic == 0 && sh->count == 0); /* zero-filled */

    char path[96];
    snprintf(path, sizeof path, "/dev/shm%s", name);
    struct stat sb;
    CHECK(stat(path, &sb) == 0);
    printf("1) %s exists, size %lld bytes, mapped at %p\n", path,
           (long long)sb.st_size, (void *)sh);
    CHECK(sb.st_size == (off_t)sizeof(struct shared));

    /* ---- 2. child writes via the INHERITED mapping ----
     * A MAP_SHARED mapping survives fork() and stays shared (MAP_PRIVATE
     * would become copy-on-write — see 02_anon_and_file_mmap.c). */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        for (int i = 0; i < 256; i++)
            sh->values[i] = i * i;
        sh->count = 256;
        snprintf(sh->note, sizeof sh->note, "written by child %d",
                 (int)getpid());
        sh->magic = MAGIC;
        _exit(0);
    }
    /* waitpid() is our synchronisation here: once the child has exited,
     * all its stores are complete and visible. Without such a sync point
     * (mutex, semaphore, atomic flag...) reading now would be a data race. */
    wait_ok(pid);
    CHECK(sh->magic == MAGIC && sh->count == 256);
    for (int i = 0; i < 256; i++)
        CHECK(sh->values[i] == i * i);
    printf("2) parent sees the child's data: count=%u, values[255]=%d, "
           "note=\"%s\"\n", sh->count, sh->values[255], sh->note);

    /* ---- 3. a separate open + mmap, as an unrelated process would ----
     * The child opens the object BY NAME and maps it again. The new mapping
     * usually lands at a DIFFERENT virtual address — same physical pages. */
    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    fflush(stdout);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(pipefd[0]);
        int cfd = shm_open(name, O_RDWR, 0);
        CHECK(cfd >= 0);
        struct shared *mine = mmap(NULL, sizeof *mine, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, cfd, 0);
        CHECK(mine != MAP_FAILED);
        close(cfd);
        void *addr = mine;
        CHECK(write(pipefd[1], &addr, sizeof addr) == (ssize_t)sizeof addr);
        mine->values[0] = -1; /* store through the second mapping */
        mine->count = 1000;
        CHECK(munmap(mine, sizeof *mine) == 0);
        _exit(0);
    }
    close(pipefd[1]);
    void *child_addr = NULL;
    CHECK(read(pipefd[0], &child_addr, sizeof child_addr) ==
          (ssize_t)sizeof child_addr);
    close(pipefd[0]);
    wait_ok(pid);
    printf("3) child re-mapped it at %p (parent: %p) -> %s address, same "
           "pages\n", child_addr, (void *)sh,
           child_addr == (void *)sh ? "same" : "different");
    CHECK(sh->values[0] == -1 && sh->count == 1000);
    printf("   parent sees the store made through the other mapping: "
           "values[0]=%d count=%u\n", sh->values[0], sh->count);

    /* ---- 4. lifetime ----
     * shm_unlink removes the NAME (like unlink on a file). Existing
     * mappings keep working; the memory is freed when the last mapping is
     * gone. Forgetting shm_unlink leaks RAM in /dev/shm until reboot —
     * check with `ls -l /dev/shm`. */
    CHECK(shm_unlink(name) == 0);
    CHECK(stat(path, &sb) != 0 && errno == ENOENT);
    sh->count = 7; /* mapping still valid */
    CHECK(sh->count == 7);
    CHECK(munmap(sh, sizeof *sh) == 0);
    printf("4) shm_unlink: name gone from /dev/shm, mapping still worked "
           "until munmap\n");

    printf("01_posix_shm: all checks passed\n");
    return 0;
}
