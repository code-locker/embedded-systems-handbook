/*
 * 03_seqlock.c — a sequence lock in user space with C11 atomics: readers
 *                never block and never write shared memory; they just
 *                retry if a writer was active while they read
 *
 * Concept : A sequence counter guards the data. The writer makes it ODD
 *           before changing the data and EVEN again afterwards. A reader
 *           notes the counter, copies the data, and checks the counter
 *           again: if it was odd, or changed, the copy may be torn, so
 *           retry. Writers are never delayed by readers. This is how the
 *           Linux kernel publishes the time (jiffies_64, timekeeping) and
 *           how the vDSO's clock_gettime() reads it without a syscall.
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/03_seqlock.c
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/03_seqlock.c SAN=tsan
 * Read    : ../README.md  (section "Seqlock")
 */
#define _GNU_SOURCE
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

#define UPDATES_PER_WRITER 20000
#define NREADERS 3

/*
 *   seq:   0      1        2      3        4
 *          |  W:  [writing]  |  W: [writing]  |
 *   R reads seq=2 ... copies ... reads seq=2  -> consistent, done
 *   R reads seq=2 ... copies ... reads seq=4  -> a write happened, RETRY
 *   R reads seq=3 (odd)                        -> writer active,   RETRY
 *
 * Why the payload must be atomic here (and not in the kernel):
 *   The reader copies the data WHILE the writer may be changing it. In
 *   C11 that is a data race = undefined behaviour, even though the reader
 *   throws the torn copy away. So every payload field is an _Atomic,
 *   accessed with explicit orderings (on ARMv8: LDAR/STLR for the
 *   acquire/release ones, plain LDR/STR for relaxed). The kernel instead
 *   uses plain memory + READ_ONCE/WRITE_ONCE + smp_rmb()/smp_wmb(), which
 *   is fine under the kernel's own memory model (LKMM), not under C11.
 *
 * Ordering used (no stand-alone fences, which ThreadSanitizer cannot model):
 *   writer: seq = s+1 (relaxed)
 *           data = ... (RELEASE stores: the odd seq store cannot sink below
 *                       them, so anyone who sees new data sees seq odd)
 *           seq = s+2 (RELEASE: publishes the data)
 *   reader: s1 = seq (ACQUIRE)
 *           data   (ACQUIRE loads: the 2nd seq load cannot rise above them)
 *           s2 = seq (relaxed)
 *           valid iff s1 even && s1 == s2
 *   The kernel's cheaper variant uses relaxed data accesses + one
 *   smp_rmb()/smp_wmb() fence on each side.
 */
typedef struct {
    atomic_uint seq;
    /* The protected "struct": three fields with an invariant y = 2x,
     * z = 3x, so a torn read is detectable. Imagine (sec, nsec, mult) of
     * a clock: mixing old sec with new nsec is exactly a torn read. */
    _Atomic uint64_t x, y, z;
} SeqData;

typedef struct { uint64_t x, y, z; } Snapshot;

/* Writers must be serialised among themselves: the seqlock only protects
 * readers from writers, not writers from each other (the kernel's
 * seqlock_t = seqcount_t + spinlock_t for exactly this reason). */
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;

static void seq_write(SeqData *d, uint64_t v)
{
    pthread_mutex_lock(&g_write_lock);
    unsigned s = atomic_load_explicit(&d->seq, memory_order_relaxed);
    atomic_store_explicit(&d->seq, s + 1, memory_order_relaxed); /* odd */
    atomic_store_explicit(&d->x, v, memory_order_release);
    atomic_store_explicit(&d->y, 2 * v, memory_order_release);
    atomic_store_explicit(&d->z, 3 * v, memory_order_release);
    atomic_store_explicit(&d->seq, s + 2, memory_order_release); /* even */
    pthread_mutex_unlock(&g_write_lock);
}

/* Returns the number of retries it needed (for the statistics). */
static unsigned seq_read(SeqData *d, Snapshot *out)
{
    unsigned retries = 0;
    for (;;) {
        unsigned s1 = atomic_load_explicit(&d->seq, memory_order_acquire);
        if (s1 & 1u) { /* writer in progress: do not even bother copying */
            /* User space: the writer may have been preempted mid-update
             * (the kernel's writers disable preemption). On a 2-core board
             * with more readers than cores, give it the CPU now and then. */
            if (++retries % 1024 == 0)
                sched_yield();
            continue;
        }
        out->x = atomic_load_explicit(&d->x, memory_order_acquire);
        out->y = atomic_load_explicit(&d->y, memory_order_acquire);
        out->z = atomic_load_explicit(&d->z, memory_order_acquire);
        unsigned s2 = atomic_load_explicit(&d->seq, memory_order_relaxed);
        if (s1 == s2)
            return retries; /* nobody wrote meanwhile: consistent copy */
        retries++;
    }
}

/* ---- the test ------------------------------------------------------------ */
static SeqData g_data;
static atomic_bool g_writers_done;

typedef struct {
    long reads, retries, torn;
    uint64_t last_x; /* values only go up: a reader must never go back */
    bool went_back;
} ReaderStats;

static void *reader(void *p)
{
    ReaderStats *st = p;
    Snapshot s;
    /* Read until the writers finish, then once more. */
    for (;;) {
        bool done = atomic_load(&g_writers_done);
        st->retries += seq_read(&g_data, &s);
        st->reads++;
        if (s.y != 2 * s.x || s.z != 3 * s.x)
            st->torn++;
        if (s.x < st->last_x)
            st->went_back = true;
        st->last_x = s.x;
        if (done)
            break;
    }
    return NULL;
}

/* Two writers write increasing values: writer 0 even numbers, writer 1 odd.
 * Each one's values increase, but interleaved they may not be globally
 * monotonic, so the "never go back" check uses one writer below. */
static void *writer(void *p)
{
    uint64_t base = (uint64_t)(uintptr_t)p;
    for (uint64_t i = 1; i <= UPDATES_PER_WRITER; i++)
        seq_write(&g_data, base + 2 * i);
    return NULL;
}

static void run(int nwriters)
{
    atomic_store(&g_data.seq, 0);
    atomic_store(&g_data.x, 0);
    atomic_store(&g_data.y, 0);
    atomic_store(&g_data.z, 0);
    atomic_store(&g_writers_done, false);

    pthread_t r[NREADERS], w[2];
    ReaderStats st[NREADERS] = {{0}};
    for (int i = 0; i < NREADERS; i++)
        CHECK(pthread_create(&r[i], NULL, reader, &st[i]) == 0);
    for (int i = 0; i < nwriters; i++)
        CHECK(pthread_create(&w[i], NULL, writer, (void *)(uintptr_t)i) == 0);
    for (int i = 0; i < nwriters; i++)
        CHECK(pthread_join(w[i], NULL) == 0);
    atomic_store(&g_writers_done, true);
    long reads = 0, retries = 0, torn = 0;
    bool went_back = false;
    for (int i = 0; i < NREADERS; i++) {
        CHECK(pthread_join(r[i], NULL) == 0);
        reads += st[i].reads;
        retries += st[i].retries;
        torn += st[i].torn;
        went_back |= st[i].went_back;
    }
    unsigned seq = atomic_load(&g_data.seq);
    printf("%d writer(s): seq=%u, %ld consistent reads, %ld retries "
           "(timing-dependent), %ld torn\n", nwriters, seq, reads, retries, torn);
    CHECK(torn == 0);                                       /* never torn  */
    CHECK(seq == 2u * nwriters * UPDATES_PER_WRITER);       /* even, all done */
    if (nwriters == 1)
        CHECK(!went_back); /* single writer: readers see values in order */
    /* Every reader's FINAL read happened after all writers finished. */
    for (int i = 0; i < NREADERS; i++)
        CHECK(st[i].last_x == (nwriters == 1 ? 2u * UPDATES_PER_WRITER
                                             : atomic_load(&g_data.x)));
}

int main(void)
{
    /* Single-thread sanity: write, read back, seq advanced by 2. */
    Snapshot s;
    seq_write(&g_data, 7);
    CHECK(seq_read(&g_data, &s) == 0);
    CHECK(s.x == 7 && s.y == 14 && s.z == 21);
    CHECK(atomic_load(&g_data.seq) == 2);

    run(1);
    run(2);

    printf("03_seqlock: all checks passed\n");
    return 0;
}
