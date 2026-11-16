/*
 * 06_robust_mutex.c — a process dies while holding a mutex in shared memory:
 *                     robust mutexes, EOWNERDEAD, pthread_mutex_consistent,
 *                     ENOTRECOVERABLE
 *
 * Concept : Threads of one process die together, but PROCESSES die alone. If
 *           a process is killed while holding a process-shared mutex, a
 *           normal mutex stays locked FOREVER: every other process hangs.
 *           A ROBUST mutex is registered with the kernel (the per-thread
 *           "robust list"); when the owner dies the kernel marks it, and the
 *           next locker gets EOWNERDEAD instead of hanging. It now owns the
 *           lock, must REPAIR the protected data (the dead owner may have
 *           left it half-updated), call pthread_mutex_consistent(), and
 *           unlock. Unlocking without consistent() makes the mutex
 *           permanently unusable (ENOTRECOVERABLE).
 *           (Lesson 03 shows mutex basics within one process and refers
 *           here for the cross-process case.)
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/06_robust_mutex.c
 * Read    : ../README.md  (section "3.6 Robust mutexes")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
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
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

/* Protected data with an invariant: a + b == 100 ("money moved between two
 * accounts"). A crash in the middle of a transfer breaks it. */
struct account_pair {
    pthread_mutex_t lock;
    int a, b;
};

static void init_mutex(pthread_mutex_t *m, int robust)
{
    pthread_mutexattr_t at;
    CHECK(pthread_mutexattr_init(&at) == 0);
    CHECK(pthread_mutexattr_setpshared(&at, PTHREAD_PROCESS_SHARED) == 0);
    CHECK(pthread_mutexattr_setrobust(
              &at, robust ? PTHREAD_MUTEX_ROBUST : PTHREAD_MUTEX_STALLED) == 0);
    CHECK(pthread_mutex_init(m, &at) == 0);
    CHECK(pthread_mutexattr_destroy(&at) == 0);
}

/* Child: lock, start a transfer, and get KILLED before finishing.
 * SIGKILL cannot be caught: no cleanup handler, no unlock — the real
 * thing (OOM killer, segfault, kill -9). */
static pid_t spawn_dying_owner(struct account_pair *p)
{
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        CHECK(pthread_mutex_lock(&p->lock) == 0);
        p->a -= 30; /* half of a transfer: invariant now broken */
        kill(getpid(), SIGKILL);
        _exit(3); /* not reached */
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    return pid;
}

static struct timespec in_ms(long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts); /* timedlock uses REALTIME */
    ts.tv_nsec += ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    return ts;
}

int main(void)
{
    alarm(10);
    struct account_pair *shm = mmap(NULL, 3 * sizeof *shm,
                                    PROT_READ | PROT_WRITE,
                                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shm != MAP_FAILED);
    struct account_pair *stalled = &shm[0];
    struct account_pair *robust = &shm[1];
    struct account_pair *ruined = &shm[2];

    /* ---- 1. NON-robust (default "stalled") mutex: owner dies -> stuck.
     * We use timedlock so the demo cannot hang; a plain lock would block
     * forever — every process touching this mutex would freeze. */
    init_mutex(&stalled->lock, 0);
    stalled->a = 50;
    stalled->b = 50;
    pid_t dead = spawn_dying_owner(stalled);
    struct timespec dl = in_ms(200);
    int r = pthread_mutex_timedlock(&stalled->lock, &dl);
    printf("1) normal mutex, owner %d killed: timedlock -> %s "
           "(plain lock would hang forever)\n", (int)dead,
           r == ETIMEDOUT ? "ETIMEDOUT" : strerror(r));
    CHECK(r == ETIMEDOUT);
    /* (This mutex is unusable now; we simply abandon it.) */

    /* ---- 2. robust mutex: owner dies -> EOWNERDEAD -> repair ---- */
    init_mutex(&robust->lock, 1);
    robust->a = 50;
    robust->b = 50;
    dead = spawn_dying_owner(robust);
    r = pthread_mutex_lock(&robust->lock);
    printf("2) robust mutex, owner %d killed: lock -> %s\n", (int)dead,
           r == EOWNERDEAD ? "EOWNERDEAD" : strerror(r));
    CHECK(r == EOWNERDEAD);
    /* We HOLD the lock now. The data may be inconsistent: check + repair.
     * Real systems redo/undo from a journal; here we know the rule. */
    printf("   data left by the dead owner: a=%d b=%d (a+b=%d, broken)\n",
           robust->a, robust->b, robust->a + robust->b);
    CHECK(robust->a + robust->b != 100);
    robust->b = 100 - robust->a; /* "complete" the transfer */
    CHECK(pthread_mutex_consistent(&robust->lock) == 0);
    CHECK(pthread_mutex_unlock(&robust->lock) == 0);
    printf("   repaired: a=%d b=%d, pthread_mutex_consistent(), unlock\n",
           robust->a, robust->b);
    /* Back to normal: lock/unlock work as usual. */
    CHECK(pthread_mutex_lock(&robust->lock) == 0);
    CHECK(robust->a + robust->b == 100);
    CHECK(pthread_mutex_unlock(&robust->lock) == 0);
    printf("   next lock -> 0: the mutex is healthy again\n");

    /* ---- 3. forgetting pthread_mutex_consistent ---- */
    init_mutex(&ruined->lock, 1);
    ruined->a = 50;
    ruined->b = 50;
    spawn_dying_owner(ruined);
    CHECK(pthread_mutex_lock(&ruined->lock) == EOWNERDEAD);
    CHECK(pthread_mutex_unlock(&ruined->lock) == 0); /* no consistent()! */
    r = pthread_mutex_lock(&ruined->lock);
    printf("3) EOWNERDEAD, unlocked WITHOUT consistent(): next lock -> %s "
           "(permanently)\n",
           r == ENOTRECOVERABLE ? "ENOTRECOVERABLE" : strerror(r));
    CHECK(r == ENOTRECOVERABLE);
    /* Only remedy: destroy and re-init it once nobody uses it any more. */
    CHECK(pthread_mutex_destroy(&ruined->lock) == 0);
    init_mutex(&ruined->lock, 1);
    CHECK(pthread_mutex_lock(&ruined->lock) == 0);
    CHECK(pthread_mutex_unlock(&ruined->lock) == 0);
    printf("   destroy + init again -> usable\n");

    CHECK(pthread_mutex_destroy(&ruined->lock) == 0);
    CHECK(pthread_mutex_destroy(&robust->lock) == 0);
    /* `stalled` is still locked by a dead process: destroying a locked
     * mutex is undefined, so we just unmap the memory. */
    CHECK(munmap(shm, 3 * sizeof *shm) == 0);

    printf("06_robust_mutex: all checks passed\n");
    return 0;
}
