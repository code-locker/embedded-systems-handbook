/*
 * 02_tas_ttas_backoff.c — three hand-made spinlocks with C11 atomics:
 *                         test-and-set, test-and-test-and-set, and TTAS
 *                         with exponential backoff + a CPU relax hint
 *
 * Concept : A spinlock is one atomic word plus the right memory ordering:
 *           ACQUIRE when taking it (nothing from the critical section may
 *           move above the lock), RELEASE when dropping it (everything
 *           written inside is visible to the next owner). The three
 *           versions differ in how much they hammer the cache line while
 *           waiting, which is what makes a spinlock fast or slow under
 *           contention.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/02_tas_ttas_backoff.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/02_tas_ttas_backoff.c SAN=tsan
 * Read    : ../README.md  (sections "TAS", "TTAS and MESI", "Backoff")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

#define ITERS 40000

/*
 * cpu_relax(): tell the core "I am in a spin-wait loop".
 *   x86     PAUSE : saves power, avoids a memory-order pipeline flush when
 *                   the loop exits, gives the sibling hyper-thread the core.
 *   aarch64 YIELD : hint to the core / hypervisor that this thread is
 *                   spinning (what Linux's cpu_relax() on arm64 emits).
 *   other         : fall back to sched_yield() (a real syscall, much heavier).
 * Inline asm is a GCC/Clang extension, so it is guarded per architecture.
 */
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

/* ---------------------------------------------------------------------------
 * 1. TAS: test-and-set. atomic_flag is the ONLY type C11 guarantees to be
 *    lock-free, so it is the textbook building block.
 *
 *    lock:   while (test_and_set(flag)) ;     every iteration is a WRITE
 *    unlock: clear(flag)
 * ------------------------------------------------------------------------- */
typedef struct { atomic_flag f; } TasLock;

static void tas_lock(TasLock *l, long *rmw)
{
    /* acquire: the critical section cannot be hoisted above this point */
    for (;;) {
        (*rmw)++;
        if (!atomic_flag_test_and_set_explicit(&l->f, memory_order_acquire))
            return;
    }
}
static void tas_unlock(TasLock *l)
{
    atomic_flag_clear_explicit(&l->f, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * 2. TTAS: test-and-test-and-set. Spin on a plain LOAD (cache line stays
 *    Shared in every waiter's cache, no traffic), and only try the
 *    expensive exchange when the lock LOOKS free.
 *
 *    MESI in short. Each core's copy of a cache line is Modified,
 *    Exclusive, Shared or Invalid. A write needs the line in M/E: every
 *    other copy is invalidated first.
 *
 *      TAS, 3 waiters:               TTAS, 3 waiters:
 *      core1 xchg -> line M on 1     core1 load -> S
 *      core2 xchg -> steal, M on 2   core2 load -> S      all three spin in
 *      core3 xchg -> steal, M on 3   core3 load -> S      their OWN cache,
 *      core1 xchg -> steal ...       (owner unlocks: one invalidation,
 *      the line ping-pongs on        then they race once with xchg)
 *      every spin: the OWNER's
 *      unlock store must queue
 *      behind all that traffic.
 * ------------------------------------------------------------------------- */
typedef struct { atomic_bool locked; } TtasLock;

static void ttas_lock(TtasLock *l, long *rmw)
{
    for (;;) {
        /* test: read-only spin, relaxed is enough (it is only a hint) */
        while (atomic_load_explicit(&l->locked, memory_order_relaxed))
            cpu_relax();
        /* test-and-set: the real attempt, with acquire */
        (*rmw)++;
        if (!atomic_exchange_explicit(&l->locked, true, memory_order_acquire))
            return;
    }
}
static void ttas_unlock(TtasLock *l)
{
    atomic_store_explicit(&l->locked, false, memory_order_release);
}

/* ---------------------------------------------------------------------------
 * 3. TTAS + exponential backoff. After losing the race, wait a little
 *    before looking again, and double the wait each time (up to a cap).
 *    Waiters spread out in time, so fewer of them stampede at each unlock.
 *    Same idea as Ethernet's CSMA/CD backoff. Cost: a waiter may sleep
 *    through the moment the lock became free (less fair, more latency).
 * ------------------------------------------------------------------------- */
static void ttas_backoff_lock(TtasLock *l, long *rmw)
{
    unsigned delay = 1;
    const unsigned max_delay = 1024;
    for (;;) {
        while (atomic_load_explicit(&l->locked, memory_order_relaxed))
            cpu_relax();
        (*rmw)++;
        if (!atomic_exchange_explicit(&l->locked, true, memory_order_acquire))
            return;
        for (unsigned i = 0; i < delay; i++)
            cpu_relax();
        if (delay < max_delay)
            delay *= 2;
        else
            sched_yield(); /* spun a long time: maybe the owner is preempted */
    }
}

/* ------------------------------------------------------------------------- */
enum Kind { K_TAS, K_TTAS, K_BACKOFF };
static const char *kind_name[] = {"TAS (atomic_flag)", "TTAS", "TTAS + backoff"};

typedef struct {
    enum Kind kind;
    TasLock *tas;
    TtasLock *ttas;
    long *counter; /* protected by the lock under test */
    long rmw;      /* atomic read-modify-write attempts by this thread */
} Arg;

static void *worker(void *p)
{
    Arg *a = p;
    for (int i = 0; i < ITERS; i++) {
        switch (a->kind) {
        case K_TAS:
            tas_lock(a->tas, &a->rmw);
            (*a->counter)++;
            tas_unlock(a->tas);
            break;
        case K_TTAS:
            ttas_lock(a->ttas, &a->rmw);
            (*a->counter)++;
            ttas_unlock(a->ttas);
            break;
        case K_BACKOFF:
            ttas_backoff_lock(a->ttas, &a->rmw);
            (*a->counter)++;
            ttas_unlock(a->ttas);
            break;
        }
    }
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
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (int)(ncpu < 4 ? (ncpu < 2 ? 2 : ncpu) : 4);
    printf("%d threads x %d lock/unlock each\n", nthreads, ITERS);

    /* Single-thread semantics first: lock, look, unlock. */
    TasLock tl = {ATOMIC_FLAG_INIT};
    TtasLock ttl;
    atomic_init(&ttl.locked, false);
    long dummy = 0;
    tas_lock(&tl, &dummy);
    CHECK(atomic_flag_test_and_set(&tl.f)); /* already set -> returns true */
    tas_unlock(&tl);
    ttas_lock(&ttl, &dummy);
    CHECK(atomic_load(&ttl.locked));
    ttas_unlock(&ttl);
    CHECK(!atomic_load(&ttl.locked));

    for (int k = K_TAS; k <= K_BACKOFF; k++) {
        long counter = 0;
        Arg args[4];
        pthread_t th[4];
        double t0 = now_ms();
        for (int i = 0; i < nthreads; i++) {
            args[i] = (Arg){.kind = (enum Kind)k, .tas = &tl, .ttas = &ttl,
                            .counter = &counter, .rmw = 0};
            CHECK(pthread_create(&th[i], NULL, worker, &args[i]) == 0);
        }
        long rmw = 0;
        for (int i = 0; i < nthreads; i++) {
            CHECK(pthread_join(th[i], NULL) == 0);
            rmw += args[i].rmw;
        }
        double ms = now_ms() - t0;
        /* rmw / acquisitions = how many cache-line-stealing writes each
         * lock acquisition cost on average. 1.0 is the ideal. */
        printf("%-18s counter %ld  %7.1f ms  atomic RMW per lock %.2f "
               "(timing-dependent)\n", kind_name[k], counter, ms,
               (double)rmw / ((double)nthreads * ITERS));
        CHECK(counter == (long)nthreads * ITERS);
        CHECK(rmw >= (long)nthreads * ITERS); /* at least one per acquire */
    }

    printf("02_tas_ttas_backoff: all checks passed\n");
    return 0;
}
