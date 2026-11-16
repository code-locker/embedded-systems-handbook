/*
 * 04_rcu_userspace.c — Read-Copy-Update in user space: lock-free readers,
 *                      copy-modify-publish writers, and a grace period
 *                      that tells the writer when the old copy is safe to
 *                      free
 *
 * Concept : Readers just load a pointer (acquire) and use the object.
 *           A writer never modifies the object in place: it COPIES it,
 *           modifies the copy, and PUBLISHES the new pointer (release).
 *           Readers that started earlier may still be using the old copy,
 *           so the writer waits for a GRACE PERIOD (until every reader
 *           that might have seen the old pointer has left its read-side
 *           critical section) and only then frees it.
 *           The grace-period scheme here: every reader has its own
 *           counter, odd while inside a read section ("am I reading?").
 *           synchronize_rcu() waits until each reader that was inside has
 *           moved on. Simple, correct, and the idea behind liburcu's
 *           "memb" flavour.
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/04_rcu_userspace.c
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/04_rcu_userspace.c SAN=tsan
 * Read    : ../README.md  (section "RCU")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
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

#define NREADERS 3
#define NUPDATES 300

/*
 * The object readers see. An invariant (checksum) lets readers detect a
 * half-updated or freed object (ASan would also catch a use-after-free).
 */
typedef struct {
    uint64_t version;
    int routes[8];
    uint64_t checksum;
} Table;

static uint64_t table_sum(const Table *t)
{
    uint64_t s = t->version * 1000003u;
    for (int i = 0; i < 8; i++)
        s = s * 31u + (uint64_t)t->routes[i];
    return s;
}

/* ---- the RCU core -------------------------------------------------------- */
static _Atomic(Table *) g_table; /* THE shared pointer */

/* One counter per reader thread, each on its own cache line: readers
 * never write a shared line, so read-side cost does not grow with the
 * number of cores (the whole point of RCU). */
typedef struct {
    _Alignas(64) atomic_ulong ctr; /* odd = inside a read-side section */
} ReaderSlot;
static ReaderSlot g_slot[NREADERS];

/*
 *   reader i                                  writer
 *   --------                                  ------
 *   rcu_read_lock:  ctr++ (now odd)           new = copy(old); modify(new)
 *   p = load(g_table)                         old = xchg(g_table, new)
 *   ... use *p ...                            synchronize_rcu():
 *   rcu_read_unlock: ctr++ (now even)            for each reader:
 *                                                  if ctr odd: wait until ctr
 *                                                  changes (reader moved on)
 *                                             free(old)    <- nobody can hold it
 *
 * Correctness needs a "Dekker" ordering: the reader's ctr++ must be
 * visible before its pointer load, and the writer's pointer store before
 * its ctr loads. Only seq_cst (or full fences) forbids that store->load
 * reordering, so these four operations are seq_cst. Then, for each reader,
 * EITHER the writer sees it inside (and waits for it), OR the reader's
 * load comes later in the single seq_cst order and sees the NEW pointer.
 *
 * Kernel RCU makes the read side completely free (rcu_read_lock() is just
 * preempt_disable(), or nothing at all on a non-preemptible kernel) by
 * having the WRITER wait for every CPU to pass a quiescent state (a
 * context switch). liburcu's QSBR flavour does the same with explicit
 * rcu_quiescent_state() calls; its memb flavour uses membarrier(2) so
 * that readers can use plain (compiler-barrier only) accesses.
 */
static void rcu_read_lock(int me)
{
    atomic_fetch_add_explicit(&g_slot[me].ctr, 1, memory_order_seq_cst);
}

static void rcu_read_unlock(int me)
{
    /* release (implied by seq_cst): my reads of the old object happen
     * before the writer, seeing this new count, frees it. */
    atomic_fetch_add_explicit(&g_slot[me].ctr, 1, memory_order_seq_cst);
}

static Table *rcu_dereference(void)
{
    return atomic_load_explicit(&g_table, memory_order_seq_cst);
}

static void synchronize_rcu(void)
{
    for (int i = 0; i < NREADERS; i++) {
        unsigned long snap = atomic_load_explicit(&g_slot[i].ctr, memory_order_seq_cst);
        if ((snap & 1ul) == 0)
            continue; /* not reading now: any LATER read sees the new pointer */
        /* Reading now, maybe the old pointer: wait until it moves on. Any
         * change of the counter means it left that section. (It may have
         * entered a NEW one since, but that one sees the new pointer.) */
        while (atomic_load_explicit(&g_slot[i].ctr, memory_order_acquire) == snap)
            sched_yield();
    }
}

/* ---- readers and writer -------------------------------------------------- */
static atomic_bool g_stop;
static atomic_long g_grace_waits; /* how often synchronize_rcu had to wait */

typedef struct {
    int id;
    long reads, bad;
    uint64_t last_version;
    bool went_back;
} ReaderArg;

static void *reader(void *p)
{
    ReaderArg *a = p;
    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        rcu_read_lock(a->id);
        const Table *t = rcu_dereference();
        /* Use the object freely: it cannot be freed or modified under us. */
        if (table_sum(t) != t->checksum)
            a->bad++;
        if (t->version < a->last_version)
            a->went_back = true;
        a->last_version = t->version;
        for (volatile int k = 0; k < 50; k++) {
        } /* pretend to do some work while holding the snapshot */
        rcu_read_unlock(a->id);
        a->reads++;
    }
    return NULL;
}

/* Writers are serialised with an ordinary mutex: RCU only coordinates
 * readers with writers, not writers with each other. */
static pthread_mutex_t g_update_lock = PTHREAD_MUTEX_INITIALIZER;
static long g_freed;

static void update_route(int idx, int value)
{
    pthread_mutex_lock(&g_update_lock);
    Table *old = atomic_load_explicit(&g_table, memory_order_relaxed);
    Table *copy = malloc(sizeof *copy);         /* READ ...          */
    CHECK(copy != NULL);
    memcpy(copy, old, sizeof *copy);            /* ... COPY ...      */
    copy->routes[idx] = value;                  /* ... UPDATE        */
    copy->version = old->version + 1;
    copy->checksum = table_sum(copy);
    /* publish: release (part of seq_cst) makes the initialised copy
     * visible to any reader that loads the new pointer */
    atomic_store_explicit(&g_table, copy, memory_order_seq_cst);
    pthread_mutex_unlock(&g_update_lock);

    /* Grace period: after this, no reader can still hold `old`. */
    unsigned long before = 0;
    for (int i = 0; i < NREADERS; i++)
        before += atomic_load(&g_slot[i].ctr) & 1ul;
    if (before)
        atomic_fetch_add(&g_grace_waits, 1);
    synchronize_rcu();
    free(old); /* safe. Freeing BEFORE synchronize_rcu is the classic bug:
                * see 05_rcu_free_too_early_bug.c (ASan reports it). */
    g_freed++;
}

int main(void)
{
    Table *first = calloc(1, sizeof *first);
    CHECK(first != NULL);
    first->checksum = table_sum(first);
    atomic_init(&g_table, first);
    for (int i = 0; i < NREADERS; i++)
        atomic_init(&g_slot[i].ctr, 0);

    pthread_t th[NREADERS];
    ReaderArg args[NREADERS];
    for (int i = 0; i < NREADERS; i++) {
        args[i] = (ReaderArg){.id = i};
        CHECK(pthread_create(&th[i], NULL, reader, &args[i]) == 0);
    }

    for (int u = 1; u <= NUPDATES; u++)
        update_route(u % 8, u);

    atomic_store(&g_stop, true);
    long reads = 0, bad = 0;
    for (int i = 0; i < NREADERS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        reads += args[i].reads;
        bad += args[i].bad;
        CHECK(!args[i].went_back); /* single writer: versions only go up */
    }

    Table *last = atomic_load(&g_table);
    printf("%d updates, %ld old tables freed after a grace period "
           "(%ld times a reader was inside and had to be waited for: "
           "timing-dependent)\n", NUPDATES, g_freed, atomic_load(&g_grace_waits));
    printf("%d readers did %ld lock-free reads, %ld saw an inconsistent "
           "table; final version %llu\n", NREADERS, reads, bad,
           (unsigned long long)last->version);
    CHECK(bad == 0);
    CHECK(g_freed == NUPDATES);
    CHECK(last->version == NUPDATES);
    for (int i = 0; i < 8; i++) /* last write to slot i was the largest u with u%8==i */
        CHECK(last->routes[i] == (NUPDATES - ((NUPDATES - i) % 8)));
    for (int i = 0; i < NREADERS; i++)
        CHECK((atomic_load(&g_slot[i].ctr) & 1ul) == 0); /* all readers out */

    free(last); /* ownership: main frees the final table, nobody reads it */
    printf("04_rcu_userspace: all checks passed\n");
    return 0;
}
