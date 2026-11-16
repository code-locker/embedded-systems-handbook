/*
 * 05_named_sem_fork.c — named POSIX semaphores between processes:
 *                       sem_open / sem_close / sem_unlink across fork()
 *
 * Concept : A NAMED semaphore ("/esh_lock_<pid>") lives in /dev/shm as
 *           the file sem.esh_lock_<pid>. Any process that knows the name
 *           can sem_open() it — no shared memory setup, no common parent
 *           needed. Lifetime rules are like files:
 *             sem_close(s)    : this process stops using it
 *             sem_unlink(name): remove the NAME; the object dies when the
 *                               last process closes it
 *           Here 3 children (fresh sem_open by name) increment a counter in
 *           shared memory under the semaphore; a second named semaphore
 *           tells the parent "child finished" — with a timeout, so a broken
 *           child cannot hang the test.
 * Build   : make run P=linux-internals/05-semaphores/c/05_named_sem_fork.c
 *           make run P=linux-internals/05-semaphores/c/05_named_sem_fork.c SAN=tsan
 * Read    : ../README.md  (section 3.7 "Named semaphores")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define N_CHILDREN 3
#define PER_CHILD 2000

/*
 *                 /dev/shm/sem.esh_lock_<pid>   (value 1: a lock)
 *                 /dev/shm/sem.esh_done_<pid>   (value 0: "finished" count)
 *                        ^       ^       ^
 *   parent ---fork--> child0  child1  child2   each: sem_open(name, 0)
 *      |                 \       |       /
 *      |          *counter in mmap(MAP_SHARED | MAP_ANONYMOUS) page
 *      +-- sem_timedwait(done) x3, waitpid x3, sem_unlink both names
 */
static char g_lock_name[64], g_done_name[64];
static pid_t g_parent;

/* atexit: if a CHECK fails in the parent, still remove the names, so no
 * stale /dev/shm/sem.* files are left behind. Children never run it (they
 * leave with _exit). */
static void cleanup_names(void)
{
    if (getpid() == g_parent) {
        sem_unlink(g_lock_name);
        sem_unlink(g_done_name);
    }
}

static void child(volatile long *counter)
{
    /* Open BY NAME, as an unrelated process would (the handles inherited
     * through fork would also work). */
    sem_t *lock = sem_open(g_lock_name, 0);
    sem_t *done = sem_open(g_done_name, 0);
    if (lock == SEM_FAILED || done == SEM_FAILED)
        _exit(2);
    for (int i = 0; i < PER_CHILD; i++) {
        while (sem_wait(lock) == -1)
            if (errno != EINTR)
                _exit(3);
        long v = *counter; /* read-modify-write, deliberately non-atomic: */
        *counter = v + 1;  /* only the semaphore makes it safe            */
        sem_post(lock);
    }
    sem_post(done);
    sem_close(lock);
    sem_close(done);
    _exit(0); /* not exit(): do not run the parent's atexit handlers */
}

int main(void)
{
    g_parent = getpid();
    /* Unique per run: two copies of this test (or a parallel `make test`)
     * must not share a semaphore. Names start with '/', no other '/'. */
    snprintf(g_lock_name, sizeof g_lock_name, "/esh_lock_%d", (int)g_parent);
    snprintf(g_done_name, sizeof g_done_name, "/esh_done_%d", (int)g_parent);
    CHECK(atexit(cleanup_names) == 0);

    /* O_EXCL: fail if it already exists rather than silently reuse a stale
     * semaphore with an unknown value. */
    sem_t *lock = sem_open(g_lock_name, O_CREAT | O_EXCL, 0600, 1);
    sem_t *done = sem_open(g_done_name, O_CREAT | O_EXCL, 0600, 0);
    CHECK(lock != SEM_FAILED && done != SEM_FAILED);

    char path[128];
    snprintf(path, sizeof path, "/dev/shm/sem.%s", g_lock_name + 1);
    int visible = access(path, F_OK) == 0;
    printf("[1] created %s and %s (%s %s)\n", g_lock_name, g_done_name, path,
           visible ? "exists" : "not visible here");

    volatile long *counter = mmap(NULL, sizeof(long), PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(counter != MAP_FAILED);
    *counter = 0;

    fflush(stdout); /* else the children inherit and re-print the buffer */
    pid_t kids[N_CHILDREN];
    for (int i = 0; i < N_CHILDREN; i++) {
        kids[i] = fork();
        CHECK(kids[i] >= 0);
        if (kids[i] == 0)
            child(counter);
    }

    /* Wait for N "done" posts, each with a 5 s deadline. */
    for (int i = 0; i < N_CHILDREN; i++) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += 5;
        int rc;
        while ((rc = sem_timedwait(done, &dl)) == -1 && errno == EINTR)
            ;
        CHECK(rc == 0);
    }
    for (int i = 0; i < N_CHILDREN; i++) {
        int st;
        CHECK(waitpid(kids[i], &st, 0) == kids[i]);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    printf("[2] %d processes x %d locked increments = %ld (expected %d)\n",
           N_CHILDREN, PER_CHILD, *counter, N_CHILDREN * PER_CHILD);
    CHECK(*counter == N_CHILDREN * PER_CHILD);

    /* Lifetime: unlink removes the name; our handle still works until
     * sem_close. */
    CHECK(sem_unlink(g_lock_name) == 0);
    CHECK(access(path, F_OK) != 0);
    CHECK(sem_post(lock) == 0); /* still usable via the open handle */
    errno = 0;
    CHECK(sem_open(g_lock_name, 0) == SEM_FAILED && errno == ENOENT);
    printf("[3] after sem_unlink: name gone (sem_open -> ENOENT), "
           "existing handle still works until sem_close\n");
    CHECK(sem_close(lock) == 0);
    CHECK(sem_unlink(g_done_name) == 0);
    CHECK(sem_close(done) == 0);
    CHECK(munmap((void *)counter, sizeof(long)) == 0);
    printf("05_named_sem_fork: all checks passed\n");
    return 0;
}
