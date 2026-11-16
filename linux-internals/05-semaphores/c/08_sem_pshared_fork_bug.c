/*
 * 08_sem_pshared_fork_bug.c — INTENTIONALLY BROKEN: an unnamed semaphore
 *                             that is NOT really shared between processes
 *
 * Concept : Two ways to get "child posts, parent waits" wrong with sem_t:
 *
 *   Bug A: the sem_t is an ordinary global. fork() gives the child a COPY
 *          (copy-on-write). The child posts its copy; the parent's
 *          semaphore never changes. The parent waits forever.
 *
 *            parent memory          child memory (copy after fork)
 *            [ sem value 0 ]        [ sem value 0 ] --post--> [ 1 ]
 *            wait... forever          (nobody looks at this one)
 *
 *   Bug B: the sem_t IS in MAP_SHARED memory, but sem_init(&s, 0, 0) was
 *          called with pshared = 0. glibc then uses PRIVATE futexes, whose
 *          wait queue is keyed by (address space, address). The parent sleeps
 *          on its private key; the child's sem_post increments the shared
 *          value but its FUTEX_WAKE_PRIVATE looks in the child's own address
 *          space and finds nobody. The parent stays asleep even though the
 *          value now says 1. It is very confusing: sem_getvalue shows 1
 *          while a process is blocked in sem_wait.
 *          (POSIX simply says: undefined behaviour.)
 *
 *          Both bugs are fixed the same way: put the sem_t in shared memory
 *          AND init it with pshared = 1, or use a named semaphore (05).
 *
 * Find it : visible misbehaviour. Each wait has a 1 s timeout here (real
 *           code would hang); the program prints what it saw and exits 1:
 *             make run P=linux-internals/05-semaphores/c/08_sem_pshared_fork_bug.c
 *           To see bug B at the syscall level:
 *             make SAN=0 build/linux-internals/05-semaphores/c/08_sem_pshared_fork_bug
 *             strace -f -e trace=futex ./build/linux-internals/05-semaphores/c/08_sem_pshared_fork_bug
 *           shows FUTEX_WAIT_BITSET_PRIVATE in the parent and
 *           FUTEX_WAKE_PRIVATE in the child (with pshared = 1 they are
 *           FUTEX_WAIT_BITSET / FUTEX_WAKE without _PRIVATE).
 *           Heisenbug warning: under strace, bug B usually DISAPPEARS. The
 *           tracer stops the parent when SIGCHLD arrives, which interrupts
 *           and restarts its futex wait; the restarted wait re-reads the
 *           value (now 1) and returns. Timing-sensitive bugs often hide
 *           when observed — that is why the plain run is the test.
 *           ThreadSanitizer cannot help: TSan does not follow processes.
 * Build   : make run P=linux-internals/05-semaphores/c/08_sem_pshared_fork_bug.c
 * Read    : ../README.md  (section 7 "Common mistakes")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
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

static sem_t g_sem; /* Bug A: plain global, private to each process */

/* Child: sleep a bit so the parent is surely asleep, then post. */
static pid_t spawn_poster(sem_t *s)
{
    fflush(stdout);
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) {
        struct timespec t = {0, 100 * 1000000L};
        nanosleep(&t, NULL);
        sem_post(s);
        _exit(0);
    }
    return p;
}

/* Returns 0 if woken, ETIMEDOUT after 1 s. */
static int wait_1s(sem_t *s)
{
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 1;
    int rc;
    while ((rc = sem_timedwait(s, &dl)) == -1 && errno == EINTR)
        ;
    return rc == 0 ? 0 : errno;
}

int main(void)
{
    int bugs = 0, v = -1;

    /* ---- Bug A -------------------------------------------------------- */
    CHECK(sem_init(&g_sem, 1, 0) == 0); /* pshared=1 does not help: COPY */
    pid_t p = spawn_poster(&g_sem);
    int rc = wait_1s(&g_sem);
    CHECK(waitpid(p, NULL, 0) == p);
    CHECK(sem_getvalue(&g_sem, &v) == 0);
    printf("[A] global sem_t, child posted: parent %s, parent's value = %d\n",
           rc == ETIMEDOUT ? "TIMED OUT (never woken)" : "was woken", v);
    if (rc == ETIMEDOUT)
        bugs++;
    sem_destroy(&g_sem);

    /* ---- Bug B -------------------------------------------------------- */
    sem_t *shs = mmap(NULL, sizeof(sem_t), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shs != MAP_FAILED);
    CHECK(sem_init(shs, 0, 0) == 0); /* BUG: pshared = 0 in shared memory */
    p = spawn_poster(shs);
    rc = wait_1s(shs);
    CHECK(waitpid(p, NULL, 0) == p);
    CHECK(sem_getvalue(shs, &v) == 0);
    printf("[B] shared memory but pshared=0, child posted: parent %s, "
           "value = %d\n",
           rc == ETIMEDOUT ? "TIMED OUT (wake-up went to the wrong futex queue)"
                           : "was woken (this libc/kernel tolerated it)",
           v);
    if (rc == ETIMEDOUT)
        bugs++;
    sem_destroy(shs);

    /* ---- The fix, for contrast ---------------------------------------- */
    CHECK(sem_init(shs, 1, 0) == 0); /* shared memory AND pshared = 1 */
    p = spawn_poster(shs);
    rc = wait_1s(shs);
    CHECK(waitpid(p, NULL, 0) == p);
    printf("[fix] shared memory + pshared=1: parent %s\n",
           rc == 0 ? "woken after ~100 ms, as intended" : "timed out (?)");
    sem_destroy(shs);
    munmap(shs, sizeof(sem_t));

    if (bugs) {
        printf("%d of 2 broken variants failed to wake the parent: BUG "
               "demonstrated\n", bugs);
        return 1;
    }
    return 0;
}
