/*
 * 08_spsc_lockfree_ring.c — lock-free single-producer single-consumer ring
 *                           buffer with C11 <stdatomic.h> and pthreads
 *
 * Concept : With exactly ONE writer thread and ONE reader thread, a ring
 *           buffer needs no lock: the producer is the only one who writes
 *           `tail`, the consumer the only one who writes `head`. All that is
 *           needed is the right MEMORY ORDERING so that when the consumer
 *           sees the new tail, it also sees the data written before it.
 *           That is release (publish) / acquire (subscribe).
 * Build   : make run P=dsa/07-stacks-and-queues/c/08_spsc_lockfree_ring.c
 * Read    : ../README.md  (section "Lock-free SPSC ring buffer")
 */
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define RING_SIZE 1024u /* power of two -> index & mask */
#define MASK (RING_SIZE - 1)
#define N_ITEMS 200000u

/*
 * Memory ordering in one picture.
 *
 * Without ordering, the CPU (ARM especially — the A72 cores on the TDA4VM
 * are weakly ordered) or the compiler may make the two stores below
 * visible to the other core in the WRONG order:
 *
 *   producer                         consumer
 *   --------                         --------
 *   buf[t] = x;          (1)         if (tail != head)      (3)
 *   tail = t + 1;        (2)             y = buf[head];     (4)
 *
 *   If (2) becomes visible before (1), the consumer sees the new tail and
 *   reads a STALE buf slot. Torn data, silently.
 *
 * Fix: a RELEASE store on tail and an ACQUIRE load of tail.
 *
 *   release store = "everything I wrote before this is published WITH it"
 *                   (like sealing an envelope: contents first, then post it)
 *   acquire load  = "if I see that value, I also see everything published
 *                    with it" (open the envelope: the contents are there)
 *
 *   producer:  write buf  ->  store_release(tail)
 *                                   \ synchronises-with
 *   consumer:                 load_acquire(tail)  ->  read buf
 *
 * Same in the other direction for slot reuse: the consumer's
 * store_release(head) after reading guarantees the producer (who
 * load_acquire's head) will not overwrite a slot the consumer is still
 * reading.
 *
 * A thread's OWN index can be loaded with memory_order_relaxed: nobody else
 * writes it, so there is nothing to synchronise with.
 *
 * On x86 acquire/release compile to plain MOVs (x86 is strongly ordered);
 * on ARMv8 they become LDAR / STLR. memory_order_seq_cst (the default for
 * atomic_store/atomic_load) would also be correct, just slightly slower.
 */
typedef struct {
    /* Each index on its own cache line: otherwise producer and consumer
     * keep stealing the same line from each other ("false sharing"). 64
     * bytes is the line size on Cortex-A72 and most x86. */
    _Alignas(64) _Atomic uint32_t head; /* written only by the consumer */
    _Alignas(64) _Atomic uint32_t tail; /* written only by the producer */
    _Alignas(64) uint32_t buf[RING_SIZE];
} SpscRing;

/* Producer side. O(1), wait-free (never spins inside). */
static bool spsc_push(SpscRing *r, uint32_t v)
{
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed); /* own */
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t - h == RING_SIZE)
        return false; /* full (free-running indices, see 03_ring_buffer.c) */
    r->buf[t & MASK] = v;                                           /* (1) */
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);   /* (2) */
    return true;
}

/* Consumer side. O(1), wait-free. */
static bool spsc_pop(SpscRing *r, uint32_t *out)
{
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed); /* own */
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire); /* (3) */
    if (t == h)
        return false; /* empty */
    *out = r->buf[h & MASK];                                        /* (4) */
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return true;
}

typedef struct {
    SpscRing *ring;
    uint64_t sum;
    uint32_t count;
    bool in_order;
    uint32_t full_spins;
} Ctx;

static void *producer(void *arg)
{
    Ctx *c = arg;
    for (uint32_t i = 1; i <= N_ITEMS; i++) {
        while (!spsc_push(c->ring, i)) {
            c->full_spins++; /* only the producer touches this field */
            sched_yield();   /* be polite when full instead of burning CPU */
        }
    }
    return NULL;
}

static void *consumer(void *arg)
{
    Ctx *c = arg;
    uint32_t expect = 1, v;
    while (expect <= N_ITEMS) {
        if (spsc_pop(c->ring, &v)) {
            if (v != expect)
                c->in_order = false;
            c->sum += v;
            c->count++;
            expect++;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

int main(void)
{
    /* Ownership: main allocates the ring and frees it after both threads
     * have been joined (join is itself a synchronisation point). */
    SpscRing *ring = aligned_alloc(64, sizeof *ring);
    CHECK(ring != NULL);
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);

    /* Single-threaded sanity: capacity and FIFO order */
    uint32_t v;
    CHECK(!spsc_pop(ring, &v));
    for (uint32_t i = 0; i < RING_SIZE; i++)
        CHECK(spsc_push(ring, i));
    CHECK(!spsc_push(ring, 999)); /* full at exactly RING_SIZE */
    for (uint32_t i = 0; i < RING_SIZE; i++)
        CHECK(spsc_pop(ring, &v) && v == i);
    CHECK(!spsc_pop(ring, &v));

    /* Two threads: 200k items through a 1024-slot ring */
    Ctx pc = {.ring = ring, .in_order = true};
    Ctx cc = {.ring = ring, .in_order = true};
    pthread_t tp, tc;
    CHECK(pthread_create(&tc, NULL, consumer, &cc) == 0);
    CHECK(pthread_create(&tp, NULL, producer, &pc) == 0);
    CHECK(pthread_join(tp, NULL) == 0);
    CHECK(pthread_join(tc, NULL) == 0);

    uint64_t want = (uint64_t)N_ITEMS * (N_ITEMS + 1) / 2;
    printf("consumer got %u items, sum %llu (expected %llu), in order: %s\n",
           cc.count, (unsigned long long)cc.sum, (unsigned long long)want,
           cc.in_order ? "yes" : "NO");
    printf("producer found the ring full %u times (timing-dependent)\n",
           pc.full_spins);
    CHECK(cc.count == N_ITEMS);
    CHECK(cc.sum == want);
    CHECK(cc.in_order);
    CHECK(atomic_load(&ring->head) == atomic_load(&ring->tail));

    free(ring);
    printf("08_spsc_lockfree_ring: all checks passed\n");
    return 0;
}
