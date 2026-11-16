/*
 * 02_anon_and_file_mmap.c — anonymous shared mappings before fork(),
 *                           MAP_SHARED vs MAP_PRIVATE (copy-on-write), and
 *                           mmap of a regular file with msync()
 *
 * Concept : If the processes are parent and child, you do not need a name:
 *           mmap(MAP_SHARED | MAP_ANONYMOUS) BEFORE fork() and both have
 *           the same pages. MAP_PRIVATE instead gives copy-on-write: after
 *           fork each process gets its own copy of a page the moment it
 *           writes. Mapping a REGULAR FILE with MAP_SHARED makes the file's
 *           page-cache pages your memory: stores change the file (written
 *           back later, or now with msync), and read()/write() on the same
 *           file see the same pages.
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/02_anon_and_file_mmap.c
 * Read    : ../README.md  (section "3.2 Anonymous shared memory and file mappings")
 */
#define _GNU_SOURCE /* MAP_ANONYMOUS */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
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

/* Fork a child that writes `val` into *p and exits; wait for it. */
static void child_writes(int *p, int val)
{
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        *p = val;
        _exit(*p == val ? 0 : 1); /* the child always sees its own store */
    }
    wait_ok(pid);
}

int main(void)
{
    alarm(10);
    long page = sysconf(_SC_PAGESIZE);

    /* ---- 1. MAP_SHARED | MAP_ANONYMOUS: shared with children ----
     *
     *    parent page table ---\
     *                          >--> [ one physical page ]
     *    child page table ----/      (after fork, both point at it)
     */
    int *shared = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    *shared = 1;
    child_writes(shared, 42);
    printf("1) MAP_SHARED|MAP_ANONYMOUS: child wrote 42, parent reads %d\n",
           *shared);
    CHECK(*shared == 42);

    /* ---- 2. MAP_PRIVATE | MAP_ANONYMOUS: copy-on-write ----
     *
     *    after fork:  parent --> [page] <-- child   (read-only, shared)
     *    child writes:  parent --> [page]   child --> [copy]  (fault + copy)
     *
     * This is how fork() is cheap: nothing is copied until written. And it
     * is why a plain malloc'd (private) buffer can NOT be used for IPC with
     * a child. */
    int *priv = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(priv != MAP_FAILED);
    *priv = 1;
    child_writes(priv, 42);
    printf("2) MAP_PRIVATE|MAP_ANONYMOUS: child wrote 42, parent still "
           "reads %d (copy-on-write)\n", *priv);
    CHECK(*priv == 1);
    CHECK(munmap(priv, (size_t)page) == 0);
    CHECK(munmap(shared, (size_t)page) == 0);

    /* ---- 3. mmap of a regular file ---- */
    char path[] = "/tmp/mmap_file_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    size_t len = 2 * (size_t)page;
    CHECK(ftruncate(fd, (off_t)len) == 0); /* file must be big enough */
    char *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);

    /* Store through memory ... */
    const char rec[] = "record written through mmap";
    memcpy(map + 100, rec, sizeof rec);
    /* ... msync(MS_SYNC) writes the dirty pages to the storage device and
     * waits. Not needed for OTHER PROCESSES to see the data (they share the
     * page cache immediately) — it is about DURABILITY (power loss). */
    CHECK(msync(map, len, MS_SYNC) == 0);

    /* read() sees it: same page-cache pages, no explicit write() needed. */
    char back[sizeof rec];
    CHECK(pread(fd, back, sizeof back, 100) == (ssize_t)sizeof back);
    CHECK(memcmp(back, rec, sizeof rec) == 0);
    printf("3) file mmap: stored via memory, pread() returns \"%s\"\n", back);

    /* ... and the other way: write() is visible in the mapping. */
    const char w[] = "written with pwrite";
    CHECK(pwrite(fd, w, sizeof w, (off_t)page) == (ssize_t)sizeof w);
    CHECK(strcmp(map + page, w) == 0);
    printf("   pwrite() at offset %ld is visible in the mapping: \"%s\"\n",
           page, map + page);

    /* A child that maps the same file by path also shares the pages. */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        int cfd = open(path, O_RDWR);
        CHECK(cfd >= 0);
        char *cm = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, cfd, 0);
        CHECK(cm != MAP_FAILED);
        close(cfd);
        CHECK(memcmp(cm + 100, rec, sizeof rec) == 0);
        memcpy(cm + 200, "from child", 11);
        CHECK(munmap(cm, len) == 0);
        _exit(0);
    }
    wait_ok(pid);
    CHECK(strcmp(map + 200, "from child") == 0);
    printf("   a child mapped the file by path and wrote \"%s\"\n", map + 200);

    CHECK(munmap(map, len) == 0);
    CHECK(close(fd) == 0);
    CHECK(unlink(path) == 0);

    printf("02_anon_and_file_mmap: all checks passed\n");
    return 0;
}
