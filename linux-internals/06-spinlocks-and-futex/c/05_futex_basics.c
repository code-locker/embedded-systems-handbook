/*
 * 05_futex_basics.c — the futex system call: FUTEX_WAIT and FUTEX_WAKE on a
 *                     32-bit word, the primitive every Linux lock sleeps on
 *
 * Concept : A futex ("fast user-space mutex") is NOT a lock. It is a kernel
 *           wait queue keyed by the ADDRESS of an int in your memory, plus
 *           two operations:
 *             FUTEX_WAIT(addr, val): sleep, but ONLY IF *addr == val still
 *                                    (checked atomically inside the kernel)
 *             FUTEX_WAKE(addr, n)  : wake up to n threads sleeping on addr
 *           The lock/flag logic itself lives in user space with atomics, so
 *           the uncontended path never enters the kernel. glibc has no
 *           wrapper, you call syscall(SYS_futex, ...).
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/05_futex_basics.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/05_futex_basics.c SAN=tsan
 * Read    : ../README.md  (section "Futex")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
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

/*
 * Thin wrappers. FUTEX_PRIVATE_FLAG = "this word is only used by threads of
 * one process": the kernel can key the wait queue by (mm, address) instead
 * of looking up the backing page, which is faster. Drop the flag for a
 * futex in memory shared between processes (lesson 11).
 *
 * The word is an _Atomic int in C. The kernel just reads it as a u32; C11
 * guarantees atomic_int has the same size, and on Linux it is lock-free
 * and has the same representation as int.
 */
static long futex_wait(atomic_int *addr, int expected,
                       const struct timespec *rel_timeout)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT | FUTEX_PRIVATE_FLAG,
                   expected, rel_timeout, NULL, 0);
}

static long futex_wake(atomic_int *addr, int n)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, n, NULL,
                   NULL, 0);
}

/*
 * Why FUTEX_WAIT takes `expected` — the lost-wakeup race it closes:
 *
 *   waiter                               waker
 *   ------                               -----
 *   v = flag;           // 0, not ready
 *                                        flag = 1;
 *                                        futex_wake(&flag)  // nobody asleep yet!
 *   sleep(&flag)        // sleeps forever: LOST WAKEUP
 *
 * With futex_wait(&flag, 0) the kernel re-checks "flag == 0" under its
 * wait-queue lock. Here flag is already 1, so the call returns EAGAIN
 * immediately instead of sleeping. (The same idea as holding the mutex
 * across pthread_cond_wait, lesson 04.)
 */

/* ---- a one-shot event ("completion") built on a futex ------------------- */
typedef struct { atomic_int done; } Event; /* 0 = not yet, 1 = signalled */

static void event_wait(Event *e)
{
    /* Loop: futex_wait can return early (EAGAIN, EINTR, spurious wakeup).
     * Only the VALUE of the word tells the truth. Acquire pairs with the
     * release in event_signal(), so data written before the signal is
     * visible after the wait. */
    while (atomic_load_explicit(&e->done, memory_order_acquire) == 0)
        futex_wait(&e->done, 0, NULL);
}

static long event_signal(Event *e)
{
    atomic_store_explicit(&e->done, 1, memory_order_release);
    return futex_wake(&e->done, INT_MAX); /* wake ALL waiters */
}

static Event g_go;
static int g_payload; /* written before g_go is signalled */
static atomic_int g_seen;

static void *waiter(void *arg)
{
    (void)arg;
    event_wait(&g_go);
    CHECK(g_payload == 42); /* happens-before through the acquire load */
    atomic_fetch_add(&g_seen, 1);
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main(void)
{
    atomic_int word;
    atomic_init(&word, 7);

    /* 1. Value mismatch: WAIT returns at once with EAGAIN (EWOULDBLOCK). */
    errno = 0;
    long rc = futex_wait(&word, 5, NULL);
    printf("futex_wait(word=7, expected=5) -> %ld, errno %s\n", rc,
           errno == EAGAIN ? "EAGAIN" : "?");
    CHECK(rc == -1 && errno == EAGAIN);

    /* 2. Value matches, nobody wakes us: the relative timeout expires. */
    struct timespec to = {.tv_sec = 0, .tv_nsec = 50 * 1000 * 1000};
    double t0 = now_ms();
    errno = 0;
    rc = futex_wait(&word, 7, &to);
    double waited = now_ms() - t0;
    printf("futex_wait(word=7, expected=7, 50 ms) -> %ld, errno %s after "
           "%.1f ms\n", rc, errno == ETIMEDOUT ? "ETIMEDOUT" : "?", waited);
    CHECK(rc == -1 && errno == ETIMEDOUT);
    CHECK(waited >= 45.0); /* the kernel may round, never wake much earlier */

    /* 3. WAKE with no sleepers: returns 0. Waking is cheap but not free:
     *    it is still a syscall, which is why real locks track "are there
     *    waiters?" in the word and skip it when there are none (06). */
    rc = futex_wake(&word, 1);
    printf("futex_wake with nobody waiting -> %ld woken\n", rc);
    CHECK(rc == 0);

    /* 4. One-shot event: 4 threads sleep in the kernel, one signal wakes
     *    them all. */
    atomic_init(&g_go.done, 0);
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        CHECK(pthread_create(&th[i], NULL, waiter, NULL) == 0);
    /* Give them a moment to really fall asleep, purely so the wake count
     * printed below is interesting. Correctness does not depend on it:
     * a late waiter sees done == 1 and never sleeps. */
    struct timespec nap = {0, 20 * 1000 * 1000};
    nanosleep(&nap, NULL);
    g_payload = 42;
    long woken = event_signal(&g_go);
    for (int i = 0; i < 4; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    printf("event: futex_wake woke %ld sleeping thread(s) (timing-dependent), "
           "%d of 4 saw the payload\n", woken, atomic_load(&g_seen));
    CHECK(atomic_load(&g_seen) == 4);
    CHECK(woken >= 0 && woken <= 4);

    /* 5. Waiting on an already-signalled event costs no syscall at all:
     *    the fast path is one atomic load. */
    event_wait(&g_go);
    printf("event_wait on a signalled event returned without sleeping\n");

    printf("05_futex_basics: all checks passed\n");
    return 0;
}
