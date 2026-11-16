/*
 * 05_pshared_mutex_condvar_sem.c — synchronisation INSIDE shared memory:
 *                                  process-shared mutex + condition
 *                                  variables, and sem_init(pshared = 1)
 *
 * Concept : Shared memory gives two processes the same bytes, nothing more.
 *           To coordinate, put the synchronisation objects IN the shared
 *           memory and mark them PTHREAD_PROCESS_SHARED. (A default mutex
 *           is "process-private": glibc may use optimisations that only
 *           work within one process.) Then the classic bounded buffer —
 *           one mutex, two condvars — works between processes exactly as it
 *           does between threads (dsa/19 c/03, lesson 04).
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/05_pshared_mutex_condvar_sem.c
 * Read    : ../README.md  (section "3.5 Process-shared mutex, condvar, semaphore")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define SLOTS 8
#define N_ITEMS 2000
#define ROUNDS 100

/*
 * Layout of the shared mapping (one page, MAP_SHARED|MAP_ANONYMOUS):
 *
 *   +-------------------- struct shm_box -------------------------+
 *   | lock (pshared mutex) | not_empty | not_full (pshared conds) |
 *   | head tail count | slots[8] ring  | producer_done             |
 *   | ping, pong (pshared sem_t) | turn_log[200] | turns           |
 *   +--------------------------------------------------------------+
 */
struct shm_box {
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    int head, tail, count;
    int slots[SLOTS];
    int producer_done;

    sem_t ping, pong;
    int turns;
    char turn_log[2 * ROUNDS];
};

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

/* Deadline `ms` from now on CLOCK_MONOTONIC (the condvars are set to that
 * clock, so wall-clock jumps cannot shorten or stretch the wait). */
static struct timespec deadline_mono(long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

/* sem_timedwait only knows CLOCK_REALTIME. */
static struct timespec deadline_real(long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

static void init_box(struct shm_box *b)
{
    pthread_mutexattr_t ma;
    CHECK(pthread_mutexattr_init(&ma) == 0);
    CHECK(pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED) == 0);
    CHECK(pthread_mutex_init(&b->lock, &ma) == 0);
    CHECK(pthread_mutexattr_destroy(&ma) == 0);

    pthread_condattr_t ca;
    CHECK(pthread_condattr_init(&ca) == 0);
    CHECK(pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED) == 0);
    CHECK(pthread_condattr_setclock(&ca, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_cond_init(&b->not_empty, &ca) == 0);
    CHECK(pthread_cond_init(&b->not_full, &ca) == 0);
    CHECK(pthread_condattr_destroy(&ca) == 0);

    /* pshared = 1: the semaphore lives in shared memory and is used by
     * several processes. With pshared = 0 it would only work for threads
     * of one process. */
    CHECK(sem_init(&b->ping, 1, 0) == 0);
    CHECK(sem_init(&b->pong, 1, 0) == 0);
}

/* Wait on a condvar with a 2 s safety deadline: a dead peer must make the
 * test FAIL, not hang. Returns with the mutex held. */
static void cond_wait_2s(pthread_cond_t *c, pthread_mutex_t *m)
{
    struct timespec dl = deadline_mono(2000);
    int r = pthread_cond_timedwait(c, m, &dl);
    CHECK(r == 0 || r == ETIMEDOUT);
    CHECK(r != ETIMEDOUT); /* peer died or deadlocked */
}

static void put(struct shm_box *b, int v)
{
    CHECK(pthread_mutex_lock(&b->lock) == 0);
    while (b->count == SLOTS) /* WHILE, not if: spurious wakeups */
        cond_wait_2s(&b->not_full, &b->lock);
    b->slots[b->tail] = v;
    b->tail = (b->tail + 1) % SLOTS;
    b->count++;
    CHECK(pthread_cond_signal(&b->not_empty) == 0);
    CHECK(pthread_mutex_unlock(&b->lock) == 0);
}

/* Returns 0 when the producer is done and the buffer is empty. */
static int get(struct shm_box *b, int *out)
{
    CHECK(pthread_mutex_lock(&b->lock) == 0);
    while (b->count == 0 && !b->producer_done)
        cond_wait_2s(&b->not_empty, &b->lock);
    int ok = 0;
    if (b->count > 0) {
        *out = b->slots[b->head];
        b->head = (b->head + 1) % SLOTS;
        b->count--;
        ok = 1;
        CHECK(pthread_cond_signal(&b->not_full) == 0);
    }
    CHECK(pthread_mutex_unlock(&b->lock) == 0);
    return ok;
}

int main(void)
{
    alarm(10);
    /* The objects must be in memory that BOTH processes map: an anonymous
     * shared mapping created before fork (shm_open works the same). */
    struct shm_box *b = mmap(NULL, sizeof *b, PROT_READ | PROT_WRITE,
                             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(b != MAP_FAILED);
    init_box(b); /* initialise ONCE, before anyone else uses it */

    /* ---- 1. bounded buffer: child produces, parent consumes ---- */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        for (int i = 1; i <= N_ITEMS; i++)
            put(b, i);
        CHECK(pthread_mutex_lock(&b->lock) == 0);
        b->producer_done = 1;
        CHECK(pthread_cond_broadcast(&b->not_empty) == 0);
        CHECK(pthread_mutex_unlock(&b->lock) == 0);
        _exit(0);
    }
    long sum = 0;
    int got = 0, v, expect = 1;
    while (get(b, &v)) {
        CHECK(v == expect); /* single producer -> FIFO order kept */
        expect++;
        sum += v;
        got++;
    }
    wait_ok(pid);
    printf("1) pshared mutex + 2 condvars: %d items through an %d-slot "
           "buffer between processes, sum %ld\n", got, SLOTS, sum);
    CHECK(got == N_ITEMS);
    CHECK(sum == (long)N_ITEMS * (N_ITEMS + 1) / 2);

    /* ---- 2. sem_init(pshared=1): strict ping-pong ----
     *
     *   parent: log 'P', post(ping) -> wait(pong) -> log 'P', post(ping)...
     *   child :          wait(ping) -> log 'C', post(pong) -> wait(ping)...
     *
     * The semaphores hand the turn over, so the log must be "PCPCPC...". */
    fflush(stdout);
    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        for (int r = 0; r < ROUNDS; r++) {
            struct timespec dl = deadline_real(2000);
            int s;
            do
                s = sem_timedwait(&b->ping, &dl);
            while (s < 0 && errno == EINTR);
            CHECK(s == 0);
            b->turn_log[b->turns++] = 'C';
            CHECK(sem_post(&b->pong) == 0);
        }
        _exit(0);
    }
    for (int r = 0; r < ROUNDS; r++) {
        b->turn_log[b->turns++] = 'P';
        CHECK(sem_post(&b->ping) == 0);
        struct timespec dl = deadline_real(2000);
        int s;
        do
            s = sem_timedwait(&b->pong, &dl);
        while (s < 0 && errno == EINTR);
        CHECK(s == 0);
    }
    wait_ok(pid);
    CHECK(b->turns == 2 * ROUNDS);
    for (int i = 0; i < 2 * ROUNDS; i++)
        CHECK(b->turn_log[i] == (i % 2 == 0 ? 'P' : 'C'));
    printf("2) sem_init(pshared=1) ping-pong: %d turns, log starts \"%.10s\" "
           "and strictly alternates\n", b->turns, b->turn_log);

    /* Destroy only when no process can still use them. */
    CHECK(sem_destroy(&b->ping) == 0 && sem_destroy(&b->pong) == 0);
    CHECK(pthread_cond_destroy(&b->not_empty) == 0);
    CHECK(pthread_cond_destroy(&b->not_full) == 0);
    CHECK(pthread_mutex_destroy(&b->lock) == 0);
    CHECK(munmap(b, sizeof *b) == 0);

    printf("05_pshared_mutex_condvar_sem: all checks passed\n");
    return 0;
}
