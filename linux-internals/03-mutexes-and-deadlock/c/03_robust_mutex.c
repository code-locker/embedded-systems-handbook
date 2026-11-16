/*
 * 03_robust_mutex.c — robust mutexes: surviving an owner that dies while
 *                     holding the lock (EOWNERDEAD, pthread_mutex_consistent)
 *
 * Concept : If the owner of a NORMAL mutex dies, the mutex stays locked
 *           forever and everybody else hangs. A ROBUST mutex is released by
 *           the kernel when its owner exits; the next locker gets EOWNERDEAD
 *           (and the lock!) and must repair the protected data, then call
 *           pthread_mutex_consistent(). If it does not, the mutex becomes
 *           permanently unusable: ENOTRECOVERABLE.
 *           The real use is between PROCESSES (mutex in shared memory, one
 *           process crashes): see ../../11-ipc-shared-memory. Here a thread
 *           that exits while holding the lock plays the dying owner.
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/03_robust_mutex.c
 *           make run P=linux-internals/03-mutexes-and-deadlock/c/03_robust_mutex.c SAN=tsan
 * Read    : ../README.md  (section 3.5 "Robust mutexes")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/*
 * How the kernel knows: every thread registers a "robust list" with the
 * kernel (set_robust_list syscall, glibc does it at thread start). glibc
 * links each robust mutex the thread holds into that list. When the thread
 * dies, the kernel walks the list, marks each mutex's futex word with
 * FUTEX_OWNER_DIED and wakes one waiter.
 *
 *   thread T                       kernel at T's exit           next locker
 *   lock(m)  -> m on T's list
 *   ...half-updated data...
 *   exit/crash  ---------------->  walk T's robust list
 *                                  m.futex |= OWNER_DIED  ---->  lock(m) = EOWNERDEAD
 *                                                               (you own m now)
 *                                                               repair data
 *                                                               consistent(m)
 *                                                               unlock(m)
 */

/* The protected data has an invariant: a + b == 100. An update moves value
 * from a to b in two steps, so a crash in the middle breaks the invariant. */
typedef struct {
    pthread_mutex_t lock;
    int a, b;
    int generation; /* bumped every time the data was repaired */
} Shared;

static void shared_init(Shared *s)
{
    pthread_mutexattr_t attr;
    CHECK(pthread_mutexattr_init(&attr) == 0);
    CHECK(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0);
    int robust = -1;
    CHECK(pthread_mutexattr_getrobust(&attr, &robust) == 0);
    CHECK(robust == PTHREAD_MUTEX_ROBUST);
    CHECK(pthread_mutex_init(&s->lock, &attr) == 0);
    CHECK(pthread_mutexattr_destroy(&attr) == 0);
    s->a = 100;
    s->b = 0;
    s->generation = 0;
}

/* Dies in the middle of the critical section: the invariant is broken. */
static void *dying_owner(void *arg)
{
    Shared *s = arg;
    CHECK(pthread_mutex_lock(&s->lock) == 0);
    s->a -= 30; /* step 1 of 2 ... */
    /* ... and the thread "crashes" before step 2 and before unlock. */
    pthread_exit(NULL);
}

/* Lock, handling EOWNERDEAD properly. Returns 0 when the caller owns a
 * mutex whose data is consistent, or an error code. */
static int robust_lock(Shared *s, int repair)
{
    int rc = pthread_mutex_lock(&s->lock);
    if (rc == EOWNERDEAD) {
        /* We OWN the lock now, but the data may be half-updated. */
        printf("    lock -> EOWNERDEAD: owner died, data a=%d b=%d "
               "(a+b=%d, invariant broken)\n", s->a, s->b, s->a + s->b);
        if (!repair)
            return EOWNERDEAD; /* caller will unlock WITHOUT consistent() */
        s->b = 100 - s->a; /* repair: restore the invariant */
        s->generation++;
        CHECK(pthread_mutex_consistent(&s->lock) == 0);
        printf("    repaired to a=%d b=%d, pthread_mutex_consistent() called\n",
               s->a, s->b);
        rc = 0;
    }
    return rc;
}

static void run_dying_owner(Shared *s)
{
    pthread_t t;
    CHECK(pthread_create(&t, NULL, dying_owner, s) == 0);
    CHECK(pthread_join(t, NULL) == 0); /* owner is gone, lock still "held" */
}

int main(void)
{
    Shared s;

    /* --- Case 1: the right way ------------------------------------------ */
    printf("[1] owner dies holding the lock; next locker repairs\n");
    shared_init(&s);
    run_dying_owner(&s);
    CHECK(robust_lock(&s, 1) == 0);
    CHECK(s.a + s.b == 100);
    CHECK(pthread_mutex_unlock(&s.lock) == 0);
    /* The mutex is fully usable again: a normal lock returns 0. */
    CHECK(pthread_mutex_lock(&s.lock) == 0);
    CHECK(pthread_mutex_unlock(&s.lock) == 0);
    printf("    later lock() -> 0, mutex recovered (generation %d)\n",
           s.generation);
    CHECK(s.generation == 1);
    CHECK(pthread_mutex_destroy(&s.lock) == 0);

    /* --- Case 2: forgetting pthread_mutex_consistent() ------------------ */
    printf("[2] next locker unlocks WITHOUT calling consistent()\n");
    shared_init(&s);
    run_dying_owner(&s);
    CHECK(robust_lock(&s, 0) == EOWNERDEAD);
    CHECK(pthread_mutex_unlock(&s.lock) == 0);
    int rc = pthread_mutex_lock(&s.lock);
    printf("    every later lock() -> %s  (the mutex is dead for good; only "
           "destroy + init helps)\n", strerror(rc));
    CHECK(rc == ENOTRECOVERABLE);
    CHECK(pthread_mutex_destroy(&s.lock) == 0);

    /* --- Contrast: a non-robust mutex ----------------------------------- */
    /* We do NOT run it: lock() would block forever. With a non-robust mutex
     * the only clue is that trylock keeps returning EBUSY although nobody is
     * alive to unlock. */
    printf("[3] with a NON-robust mutex the same situation is a silent, "
           "permanent hang (not run)\n");

    printf("03_robust_mutex: all checks passed\n");
    return 0;
}
