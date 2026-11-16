/*
 * 03_ticket_lock.c — a FAIR spinlock: the ticket lock (the "take a number"
 *                    machine at a bakery), as Linux used on x86/ARM before
 *                    qspinlock
 *
 * Concept : TAS/TTAS locks are unfair: whoever wins the race to the cache
 *           line gets the lock, so the thread that just unlocked (whose
 *           cache already has the line) often wins again and others starve.
 *           A ticket lock hands the lock out in strict FIFO order with two
 *           counters: `next` (ticket dispenser) and `owner` (now serving).
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/03_ticket_lock.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/03_ticket_lock.c SAN=tsan
 * Read    : ../README.md  (section "Ticket lock")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
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
 *   next  = 5   (next ticket to hand out)        waiters hold tickets 3, 4
 *   owner = 2   (ticket currently being served)
 *
 *     lock():   my = fetch_add(&next, 1);        // take a number (atomic)
 *               while (owner != my) spin;        // wait until it is called
 *     unlock(): owner = owner + 1;               // call the next number
 *
 *   Only the owner writes `owner`, so unlock is a plain release store, not
 *   an RMW. Unsigned wrap-around is fine: we only compare for equality, and
 *   2^32 waiters cannot exist.
 *
 *   Downside: every waiter spins on the SAME `owner` line, so every unlock
 *   invalidates it in all waiters' caches (N cache misses per hand-over).
 *   That is what MCS / qspinlock fix (04_mcs_lock.c).
 */
typedef struct {
    atomic_uint next;
    atomic_uint owner;
} TicketLock;

static void ticket_init(TicketLock *l)
{
    atomic_init(&l->next, 0);
    atomic_init(&l->owner, 0);
}

static void ticket_lock(TicketLock *l)
{
    /* relaxed is enough for taking a number: it is the load of `owner`
     * below that must synchronise with the previous unlock. */
    unsigned my = atomic_fetch_add_explicit(&l->next, 1, memory_order_relaxed);
    unsigned spins = 0;
    while (atomic_load_explicit(&l->owner, memory_order_acquire) != my) {
        cpu_relax();
        /* Fair locks suffer badly if a waiter AHEAD of us is preempted:
         * nobody behind it can get the lock. Yield now and then. */
        if (++spins % 1024 == 0)
            sched_yield();
    }
}

static bool ticket_trylock(TicketLock *l)
{
    /* Only take a ticket if it would be served immediately. */
    unsigned owner = atomic_load_explicit(&l->owner, memory_order_relaxed);
    unsigned expect = owner;
    return atomic_compare_exchange_strong_explicit(
        &l->next, &expect, owner + 1, memory_order_acquire,
        memory_order_relaxed);
}

static void ticket_unlock(TicketLock *l)
{
    unsigned o = atomic_load_explicit(&l->owner, memory_order_relaxed);
    atomic_store_explicit(&l->owner, o + 1, memory_order_release);
}

/* ---- correctness under contention + a fairness measurement --------------- */
static TicketLock g_lock;
static long g_counter;           /* protected by g_lock */
static int g_order[4 * ITERS];   /* who got the lock, in order (protected) */
static int g_order_len;          /* protected */
static atomic_int g_ready;       /* start gate: all threads begin together */
static int g_nthreads;

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    /* Start gate: without it, the first thread created runs hundreds of
     * iterations alone before the others exist, which says nothing about
     * fairness. (A pthread_barrier would also do, see lesson 08.) */
    atomic_fetch_add(&g_ready, 1);
    while (atomic_load(&g_ready) < g_nthreads)
        sched_yield();
    for (int i = 0; i < ITERS; i++) {
        ticket_lock(&g_lock);
        g_counter++;
        g_order[g_order_len++] = id;
        ticket_unlock(&g_lock);
    }
    return NULL;
}

/* ---- deterministic FIFO test --------------------------------------------- */
static atomic_int g_fifo_seq; /* order in which waiters got the lock */
typedef struct { int got_pos; } Waiter;

static void *fifo_waiter(void *arg)
{
    Waiter *w = arg;
    ticket_lock(&g_lock);
    w->got_pos = atomic_fetch_add(&g_fifo_seq, 1);
    ticket_unlock(&g_lock);
    return NULL;
}

/* Wait (bounded) until `next` reaches a value, i.e. a waiter took a ticket. */
static void wait_next(unsigned want)
{
    for (int i = 0; i < 5000000; i++) {
        if (atomic_load(&g_lock.next) == want)
            return;
        sched_yield();
    }
    CHECK(!"timed out waiting for a waiter to take its ticket");
}

int main(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (int)(ncpu < 4 ? (ncpu < 2 ? 2 : ncpu) : 4);
    ticket_init(&g_lock);

    /* 1. trylock semantics */
    CHECK(ticket_trylock(&g_lock));
    CHECK(!ticket_trylock(&g_lock));
    ticket_unlock(&g_lock);
    CHECK(ticket_trylock(&g_lock));
    ticket_unlock(&g_lock);

    /* 2. FIFO, deterministically: main holds the lock, waiter A takes its
     *    ticket, THEN waiter B takes its ticket. Main unlocks. A must be
     *    served before B, regardless of scheduling. */
    ticket_lock(&g_lock);
    unsigned base = atomic_load(&g_lock.next); /* = main's ticket + 1 */
    Waiter a = {-1}, b = {-1};
    pthread_t ta, tb;
    CHECK(pthread_create(&ta, NULL, fifo_waiter, &a) == 0);
    wait_next(base + 1); /* A has its ticket */
    CHECK(pthread_create(&tb, NULL, fifo_waiter, &b) == 0);
    wait_next(base + 2); /* B has its ticket */
    ticket_unlock(&g_lock);
    CHECK(pthread_join(ta, NULL) == 0);
    CHECK(pthread_join(tb, NULL) == 0);
    printf("FIFO: A (ticket first) served at %d, B served at %d\n",
           a.got_pos, b.got_pos);
    CHECK(a.got_pos == 0 && b.got_pos == 1);

    /* 3. Contention: correctness + how evenly the lock was handed out. */
    pthread_t th[4];
    g_nthreads = nthreads;
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_create(&th[i], NULL, worker, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    CHECK(g_counter == (long)nthreads * ITERS);
    CHECK(g_order_len == nthreads * ITERS);

    /* Longest run of consecutive acquisitions by the same thread. With a
     * ticket lock, a thread that unlocks and immediately re-locks gets a
     * ticket BEHIND everyone already waiting, so runs stay short once all
     * threads are running. (Printed, not asserted: it depends on timing,
     * e.g. a waiter that is preempted lets nobody pass.) Try the same
     * measurement with the TTAS lock from 02: runs of hundreds are normal. */
    int longest = 1, run = 1;
    for (int i = 1; i < g_order_len; i++) {
        run = (g_order[i] == g_order[i - 1]) ? run + 1 : 1;
        if (run > longest)
            longest = run;
    }
    printf("contention: counter %ld (expected %ld), longest same-thread run "
           "%d of %d (timing-dependent)\n", g_counter,
           (long)nthreads * ITERS, longest, g_order_len);

    /* 4. Counter wrap-around: start both counters near UINT_MAX. */
    atomic_store(&g_lock.next, 0xFFFFFFFEu);
    atomic_store(&g_lock.owner, 0xFFFFFFFEu);
    for (int i = 0; i < 5; i++) {
        ticket_lock(&g_lock);
        ticket_unlock(&g_lock);
    }
    CHECK(atomic_load(&g_lock.owner) == 3u && atomic_load(&g_lock.next) == 3u);
    printf("wrap-around past UINT_MAX: ok\n");

    printf("03_ticket_lock: all checks passed\n");
    return 0;
}
