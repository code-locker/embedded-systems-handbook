/*
 * 04_mcs_lock.c — the MCS queue lock (Mellor-Crummey & Scott, 1991): a fair
 *                 spinlock where every waiter spins on ITS OWN cache line
 *
 * Concept : Ticket locks are fair but every waiter spins on one shared
 *           `owner` word, so each unlock causes N cache misses. MCS builds
 *           an explicit linked QUEUE of waiters. Each waiter spins on a
 *           flag in its own node; the unlocker writes only its successor's
 *           flag: O(1) cache traffic per hand-over, however many waiters.
 *           Linux's qspinlock (kernel/locking/qspinlock.c) is a 4-byte
 *           compressed MCS lock; it has been the arm64 and x86 spinlock
 *           since ~v4.2 (x86) / v4.18 (arm64).
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/04_mcs_lock.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/04_mcs_lock.c SAN=tsan
 * Read    : ../README.md  (section "MCS and qspinlock")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define ITERS 20000

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    sched_yield();
#endif
}

/*
 * The lock is just a pointer to the LAST node in the queue (NULL = free).
 * Each thread brings its own node (usually on its stack; the kernel keeps
 * 4 per-CPU nodes, one per context: task, softirq, hardirq, NMI).
 *
 *          lock->tail ------------------------------------+
 *                                                         v
 *   [ A: owner ] --next--> [ B: locked=1 ] --next--> [ C: locked=1 ]
 *                            spins on B.locked         spins on C.locked
 *
 *   lock(me):   me.next = NULL; me.locked = 1;
 *               prev = xchg(&tail, me);           // join the queue, O(1)
 *               if (prev) { prev->next = me;      // link in
 *                           while (me.locked) ; } // spin on MY line only
 *
 *   unlock(me): if (me.next == NULL) {            // nobody linked yet?
 *                   if (cas(&tail, me, NULL)) return;  // really nobody
 *                   while (me.next == NULL) ;     // someone is mid-join:
 *               }                                 //   wait for the link
 *               me.next->locked = 0;              // hand over directly
 *
 * The tricky window is between xchg(&tail) and prev->next = me: the new
 * waiter is in the queue but not yet linked. That is why unlock must wait
 * for `next` when the CAS fails.
 *
 * Memory ordering:
 *   - xchg on tail is acq_rel: release publishes my initialised node,
 *     acquire syncs with the previous holder's CAS-to-NULL unlock.
 *   - prev->next store is release; unlocker loads it with acquire, so it
 *     sees my node initialised before it writes my->locked.
 *   - me.locked = 0 is the release of the critical section; the waiter's
 *     load of its own flag is the acquire.
 */
typedef struct McsNode {
    _Alignas(64) _Atomic(struct McsNode *) next; /* own cache line */
    atomic_bool locked;
} McsNode;

typedef struct {
    _Atomic(McsNode *) tail;
} McsLock;

static void mcs_lock(McsLock *l, McsNode *me)
{
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, true, memory_order_relaxed);
    McsNode *prev = atomic_exchange_explicit(&l->tail, me, memory_order_acq_rel);
    if (prev == NULL)
        return; /* queue was empty: lock is ours, no spinning */
    atomic_store_explicit(&prev->next, me, memory_order_release);
    unsigned spins = 0;
    while (atomic_load_explicit(&me->locked, memory_order_acquire)) {
        cpu_relax();
        if (++spins % 1024 == 0)
            sched_yield(); /* user space: the thread ahead may be preempted */
    }
}

static bool mcs_trylock(McsLock *l, McsNode *me)
{
    atomic_store_explicit(&me->next, NULL, memory_order_relaxed);
    atomic_store_explicit(&me->locked, true, memory_order_relaxed);
    McsNode *expect = NULL;
    return atomic_compare_exchange_strong_explicit(
        &l->tail, &expect, me, memory_order_acq_rel, memory_order_relaxed);
}

static void mcs_unlock(McsLock *l, McsNode *me)
{
    McsNode *next = atomic_load_explicit(&me->next, memory_order_acquire);
    if (next == NULL) {
        McsNode *expect = me;
        if (atomic_compare_exchange_strong_explicit(
                &l->tail, &expect, NULL, memory_order_release,
                memory_order_relaxed))
            return; /* we were the last one: lock is free */
        /* Someone swapped tail but has not linked prev->next yet. */
        while ((next = atomic_load_explicit(&me->next, memory_order_acquire)) == NULL)
            cpu_relax();
    }
    atomic_store_explicit(&next->locked, false, memory_order_release);
}

/* ------------------------------------------------------------------------- */
static McsLock g_lock;
static long g_counter;        /* protected by g_lock */
static long g_in_cs;          /* protected: must never exceed 1 */
static long g_max_in_cs;      /* protected */

static void *worker(void *arg)
{
    (void)arg;
    McsNode node; /* lives on this thread's stack for the whole loop.
                   * It must stay valid from lock() until unlock() returns:
                   * other threads write to it while we wait. */
    for (int i = 0; i < ITERS; i++) {
        mcs_lock(&g_lock, &node);
        g_in_cs++;
        if (g_in_cs > g_max_in_cs)
            g_max_in_cs = g_in_cs;
        g_counter++;
        g_in_cs--;
        mcs_unlock(&g_lock, &node);
    }
    return NULL;
}

int main(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (int)(ncpu < 4 ? (ncpu < 2 ? 2 : ncpu) : 4);
    atomic_init(&g_lock.tail, NULL);

    /* 1. Single-thread: uncontended lock never spins; trylock fails when
     *    held; unlock with no successor leaves tail == NULL. */
    McsNode a, b;
    mcs_lock(&g_lock, &a);
    CHECK(atomic_load(&g_lock.tail) == &a);
    CHECK(!mcs_trylock(&g_lock, &b));
    mcs_unlock(&g_lock, &a);
    CHECK(atomic_load(&g_lock.tail) == NULL);
    CHECK(mcs_trylock(&g_lock, &b));
    mcs_unlock(&g_lock, &b);
    printf("uncontended lock/trylock/unlock: ok (sizeof(McsNode) = %zu)\n",
           sizeof(McsNode));

    /* 2. Contention */
    pthread_t th[4];
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    printf("%d threads: counter %ld (expected %ld), max threads inside "
           "critical section %ld\n", nthreads, g_counter,
           (long)nthreads * ITERS, g_max_in_cs);
    CHECK(g_counter == (long)nthreads * ITERS);
    CHECK(g_max_in_cs == 1);
    CHECK(atomic_load(&g_lock.tail) == NULL);

    printf("04_mcs_lock: all checks passed\n");
    return 0;
}
