/*
 * 04_sysv_shm.c — System V shared memory: shmget, shmat, shmdt,
 *                 shmctl(IPC_STAT / IPC_RMID), attach counts
 *
 * Concept : The 1983 API that predates shm_open. A segment is identified by
 *           a key (ftok or IPC_PRIVATE) -> integer id; shmat() maps it,
 *           shmdt() unmaps it. IPC_RMID only MARKS it for destruction: it
 *           disappears when the attach count (shm_nattch) drops to zero.
 *           Still common in older code (X11 MIT-SHM, PostgreSQL before 9.3,
 *           some industrial software); new code should use shm_open/memfd.
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/04_sysv_shm.c
 * Read    : ../README.md  (section "3.4 System V shared memory")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
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

#define SEG_SIZE 8192

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

static unsigned long nattch(int id)
{
    struct shmid_ds ds;
    CHECK(shmctl(id, IPC_STAT, &ds) == 0);
    return (unsigned long)ds.shm_nattch;
}

int main(void)
{
    alarm(10);
    char keyfile[] = "/tmp/sysv_shm_key_XXXXXX";
    int kfd = mkstemp(keyfile);
    CHECK(kfd >= 0);
    close(kfd);
    key_t key = ftok(keyfile, 'S');
    CHECK(key != -1);
    int id = shmget(key, SEG_SIZE, IPC_CREAT | IPC_EXCL | 0600);
    int have_key = 1;
    if (id < 0 && errno == EEXIST) {
        /* Someone else owns that key: use a private segment (reachable only
         * through its id) and never look the key up again. */
        printf("ftok key collision, using IPC_PRIVATE\n");
        id = shmget(IPC_PRIVATE, SEG_SIZE, IPC_CREAT | 0600);
        have_key = 0;
    }
    CHECK(id >= 0);
    printf("1) shmget(key 0x%08x, %d bytes) -> id %d   (see: ipcs -m)\n",
           (unsigned)key, SEG_SIZE, id);

    char *p = shmat(id, NULL, 0); /* NULL: let the kernel pick the address */
    CHECK(p != (char *)-1);
    CHECK(p[0] == 0); /* new segments are zero-filled */
    CHECK(nattch(id) == 1);
    printf("   shmat -> %p, shm_nattch = %lu\n", (void *)p, nattch(id));

    /* ---- 2. child attaches by id, writes, detaches ----
     * (Attachments are also inherited across fork; we attach again to show
     * the "unrelated process" path, which only needs the id or the key.) */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        CHECK(shmdt(p) == 0); /* drop the inherited attachment */
        /* An unrelated program would find it by key (size 0 = "existing
         * segment, any size"). With the IPC_PRIVATE fallback, the id is
         * all there is. */
        int cid = have_key ? shmget(key, 0, 0) : id;
        CHECK(cid == id);
        char *cp = shmat(cid, NULL, 0);
        CHECK(cp != (char *)-1);
        snprintf(cp, SEG_SIZE, "hello from child %d", (int)getpid());
        CHECK(shmdt(cp) == 0);
        _exit(0);
    }
    wait_ok(pid); /* sync point: the child's stores are complete */
    printf("2) parent reads: \"%s\"\n", p);
    CHECK(strncmp(p, "hello from child ", 17) == 0);

    /* ---- 3. IPC_RMID with an attachment still present ----
     * The segment is only marked (ipcs shows status "dest"); the memory
     * stays usable until the last shmdt(). */
    CHECK(shmctl(id, IPC_RMID, NULL) == 0);
    struct shmid_ds ds;
    CHECK(shmctl(id, IPC_STAT, &ds) == 0); /* still exists... */
    printf("3) after IPC_RMID: still attached (nattch=%lu), memory still "
           "usable: \"%.5s...\"\n", (unsigned long)ds.shm_nattch, p);
    CHECK(ds.shm_nattch == 1);
    if (have_key) /* ...but the key no longer finds it */
        CHECK(shmget(key, 0, 0) == -1 && errno == ENOENT);
    CHECK(shmdt(p) == 0); /* last detach -> destroyed now */
    errno = 0;
    CHECK(shmctl(id, IPC_STAT, &ds) == -1 &&
          (errno == EINVAL || errno == EIDRM));
    printf("   last shmdt: segment destroyed (IPC_STAT -> EINVAL)\n");
    CHECK(unlink(keyfile) == 0);

    printf("   Forgotten segments survive the process: list with `ipcs -m`, "
           "remove with `ipcrm -m <id>`.\n");
    printf("04_sysv_shm: all checks passed\n");
    return 0;
}
