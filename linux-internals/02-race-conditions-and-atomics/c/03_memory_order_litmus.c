/*
 * 03_memory_order_litmus.c — memory orders in action: message passing (MP)
 *                            and store buffering (SB / Dekker) litmus tests,
 *                            with relaxed, release/acquire, fences, seq_cst
 *
 * Concept : Atomicity is not enough: a CPU (and the compiler) may make your
 *           writes visible to other cores in a different order than you
 *           wrote them, and may perform your reads early. Memory orders say
 *           which reorderings are forbidden:
 *             - MP needs release (writer) + acquire (reader).
 *             - SB (Dekker / Peterson style "I set my flag, then read
 *               yours") needs seq_cst — release/acquire is NOT enough.
 *           Correct orders are CHECKed; the outcomes of deliberately too-weak
 *           orders are only PRINTED (they depend on the CPU: x86 hides MP
 *           reordering but shows SB reordering; ARM can show both).
 * Build   : make run P=linux-internals/02-race-conditions-and-atomics/c/03_memory_order_litmus.c
 *           (run it on the BeagleBone AI-64 too: the relaxed MP case can
 *            show anomalies there that x86 never shows)
 * Gotcha  : the memory_order argument must be a COMPILE-TIME CONSTANT.
 *           Pass it through a function parameter and GCC quietly upgrades
 *           it to seq_cst — the first version of this program did that and
 *           the store-buffering test never showed anything (see SPIN_UNTIL).
 * Read    : ../README.md  (sections 3.6 - 3.9)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define MP_ROUNDS 20000
#define SB_ROUNDS 20000

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Called on every failed spin: yields now and then (the machine may have
 * fewer free cores than spinning threads) and gives up after 10 s so a bug
 * can never hang `make test`. */
static void spin_backoff(unsigned n, double *deadline)
{
    if ((n & 255) == 0)
        sched_yield();
    if ((n & 0xFFFF) == 0) {
        if (*deadline == 0)
            *deadline = now_s() + 10;
        else
            CHECK(now_s() < *deadline);
    }
}

/*
 * Spin until *x == want, loading with memory order MO.
 *
 * A MACRO, not a function taking `memory_order mo`, on purpose: the order
 * must be a COMPILE-TIME CONSTANT. If GCC cannot see the constant it
 * silently uses memory_order_seq_cst — correct, but it would hide exactly
 * the relaxed behaviour this program wants to show (on ARM a seq_cst load
 * is an LDAR, which already has acquire semantics).
 */
#define SPIN_UNTIL(x, want, MO)                                              \
    do {                                                                     \
        double dl_ = 0;                                                      \
        for (unsigned n_ = 1; atomic_load_explicit((x), MO) != (want); n_++) \
            spin_backoff(n_, &dl_);                                          \
    } while (0)

/* =====================================================================
 * Litmus 1: MESSAGE PASSING
 *
 *   writer                         reader
 *   ------                         ------
 *   data = 42;          (W1)       while (flag != 1) ;   (R1)
 *   flag = 1;           (W2)       use(data);            (R2)
 *
 *   Question: can the reader see flag == 1 but the OLD data?
 *
 *   - relaxed everywhere: YES on ARM. The A72 may make W2 visible before W1
 *     (stores drain out of order), or perform R2 before R1 (loads are
 *     speculated). x86 (TSO) keeps store->store and load->load order in
 *     hardware, so you will not see it there — but the COMPILER may still
 *     reorder relaxed accesses on any CPU.
 *   - flag store RELEASE + flag load ACQUIRE: NO, never.
 *
 *        writer                               reader
 *        data = 42  ─┐ cannot move below     ┌─ use(data) cannot move above
 *        store_release(flag) ── synchronizes-with ──> load_acquire(flag)
 *
 *   We run MP_ROUNDS rounds; the reader acknowledges each round (ack)
 *   so the writer never overwrites data the reader has not read yet.
 * ===================================================================== */

typedef enum { MP_RELAXED, MP_REL_ACQ, MP_FENCES } MpMode;

typedef struct {
    MpMode mode;
    atomic_int flag, ack;
    /* Payload. For RELEASE/ACQUIRE this is a plain struct: the protocol
     * makes the access race-free. For the relaxed/fence variants it is a
     * pair of relaxed atomics so the "too weak" case is still free of data
     * races (just possibly wrong values) and ThreadSanitizer, which does
     * not model fences, stays quiet. */
    struct { int a, b; } plain;
    atomic_int a, b;
    int anomalies;
} Mp;

static void *mp_writer(void *arg)
{
    Mp *m = arg;
    for (int r = 1; r <= MP_ROUNDS; r++) {
        switch (m->mode) {
        case MP_REL_ACQ:
            SPIN_UNTIL(&m->ack, r - 1, memory_order_acquire); /* reader done */
            m->plain.a = r;
            m->plain.b = 3 * r;
            atomic_store_explicit(&m->flag, r, memory_order_release);
            break;
        case MP_RELAXED:
            SPIN_UNTIL(&m->ack, r - 1, memory_order_relaxed);
            atomic_store_explicit(&m->a, r, memory_order_relaxed);
            atomic_store_explicit(&m->b, 3 * r, memory_order_relaxed);
            atomic_store_explicit(&m->flag, r, memory_order_relaxed);
            break;
        case MP_FENCES:
            SPIN_UNTIL(&m->ack, r - 1, memory_order_relaxed);
            atomic_thread_fence(memory_order_acquire); /* pairs with reader's
                                                          release fence */
            atomic_store_explicit(&m->a, r, memory_order_relaxed);
            atomic_store_explicit(&m->b, 3 * r, memory_order_relaxed);
            /* release FENCE: all earlier memory ops are ordered before all
             * LATER stores (here: the flag store). Same effect as making
             * the flag store a release store, but it covers every store
             * after it — handy when publishing several flags at once. */
            atomic_thread_fence(memory_order_release);
            atomic_store_explicit(&m->flag, r, memory_order_relaxed);
            break;
        }
    }
    return NULL;
}

static void *mp_reader(void *arg)
{
    Mp *m = arg;
    for (int r = 1; r <= MP_ROUNDS; r++) {
        int a = 0, b = 0;
        switch (m->mode) {
        case MP_REL_ACQ:
            SPIN_UNTIL(&m->flag, r, memory_order_acquire);
            a = m->plain.a;
            b = m->plain.b;
            atomic_store_explicit(&m->ack, r, memory_order_release);
            break;
        case MP_RELAXED:
            SPIN_UNTIL(&m->flag, r, memory_order_relaxed);
            a = atomic_load_explicit(&m->a, memory_order_relaxed);
            b = atomic_load_explicit(&m->b, memory_order_relaxed);
            atomic_store_explicit(&m->ack, r, memory_order_relaxed);
            break;
        case MP_FENCES:
            SPIN_UNTIL(&m->flag, r, memory_order_relaxed);
            atomic_thread_fence(memory_order_acquire); /* later loads stay
                                                          after the flag load */
            a = atomic_load_explicit(&m->a, memory_order_relaxed);
            b = atomic_load_explicit(&m->b, memory_order_relaxed);
            atomic_thread_fence(memory_order_release); /* our loads are done
                                                          before the ack */
            atomic_store_explicit(&m->ack, r, memory_order_relaxed);
            break;
        }
        if (a != r || b != 3 * r)
            m->anomalies++; /* only the reader touches this */
    }
    return NULL;
}

static int run_mp(MpMode mode)
{
    Mp *m = calloc(1, sizeof *m); /* calloc: all atomics start at 0 */
    CHECK(m != NULL);
    m->mode = mode;
    pthread_t w, r;
    CHECK(pthread_create(&r, NULL, mp_reader, m) == 0);
    CHECK(pthread_create(&w, NULL, mp_writer, m) == 0);
    CHECK(pthread_join(w, NULL) == 0);
    CHECK(pthread_join(r, NULL) == 0);
    int n = m->anomalies;
    free(m);
    return n;
}

/* =====================================================================
 * Litmus 2: STORE BUFFERING (the core of Dekker's / Peterson's algorithm)
 *
 *   x = y = 0
 *   thread 1            thread 2
 *   x = 1;              y = 1;
 *   r1 = y;             r2 = x;
 *
 *   Question: can r1 == 0 && r2 == 0?   ("both entered the critical
 *   section", if x/y were "I want in" flags)
 *
 *   Interleaving thinking says no: whoever stores first, the other must
 *   see it. Real CPUs say YES — even x86:
 *
 *     core 1                    core 2
 *     [x=1] -> store buffer     [y=1] -> store buffer      not yet in cache!
 *     r1 = y  (reads 0 from cache)
 *                               r2 = x  (reads 0 from cache)
 *     store buffers drain: x=1, y=1    ... too late, both read 0
 *
 *   release/acquire does NOT forbid this: a release store followed by an
 *   acquire load of a DIFFERENT variable may still be reordered (store->load
 *   is the one reordering even x86 TSO allows). Only seq_cst (one global
 *   order of all seq_cst ops) forbids it: x86 emits MFENCE / XCHG for the
 *   seq_cst store, ARMv8 uses STLR followed by LDAR, which are specified
 *   not to reorder with each other.
 * ===================================================================== */

/* Every variable on its OWN 64-byte cache line. If x, y and the go/done
 * handshake shared a line, every access would drag that one line between
 * cores and serialise the threads — the reordering window disappears and
 * the relaxed case would (misleadingly) always print 0. */
typedef struct {
    _Alignas(64) atomic_int x;
    _Alignas(64) atomic_int y;
    _Alignas(64) atomic_int go;
    _Alignas(64) atomic_int done;
    _Alignas(64) int r1; /* written by thread 1, read by main after `done` */
    _Alignas(64) int r2; /* written by thread 2, read by main after `done` */
} Sb;

static Sb g_sb; /* static storage: properly 64-byte aligned (calloc is not) */

/* The two threads leave the `go` spin at slightly different times (the
 * cache line reaches one core first), so they rarely execute their
 * store+load at the same moment. A small random busy delay in each thread
 * makes the windows overlap in some rounds — the trick used by litmus
 * testing tools. The delay touches only locals. */
static void random_delay(unsigned *seed)
{
    *seed = *seed * 1103515245u + 12345u;
    for (volatile unsigned i = (*seed >> 16) % 8; i > 0; i--)
        ;
}

/* One pair of thread functions per (store order, load order) combination,
 * so the orders are compile-time constants (see SPIN_UNTIL). */
#define DEFINE_SB(NAME, ST, LD)                                              \
    static void *sb_t1_##NAME(void *arg)                                     \
    {                                                                        \
        Sb *s = arg;                                                         \
        unsigned seed = 1;                                                   \
        for (int round = 1; round <= SB_ROUNDS; round++) {                   \
            SPIN_UNTIL(&s->go, round, memory_order_acquire);                 \
            random_delay(&seed);                                             \
            atomic_store_explicit(&s->x, 1, ST);  /* I want in   */          \
            s->r1 = atomic_load_explicit(&s->y, LD); /* do you?  */          \
            atomic_fetch_add_explicit(&s->done, 1, memory_order_release);    \
        }                                                                    \
        return NULL;                                                         \
    }                                                                        \
    static void *sb_t2_##NAME(void *arg)                                     \
    {                                                                        \
        Sb *s = arg;                                                         \
        unsigned seed = 2;                                                   \
        for (int round = 1; round <= SB_ROUNDS; round++) {                   \
            SPIN_UNTIL(&s->go, round, memory_order_acquire);                 \
            random_delay(&seed);                                             \
            atomic_store_explicit(&s->y, 1, ST);                             \
            s->r2 = atomic_load_explicit(&s->x, LD);                         \
            atomic_fetch_add_explicit(&s->done, 1, memory_order_release);    \
        }                                                                    \
        return NULL;                                                         \
    }

DEFINE_SB(relaxed, memory_order_relaxed, memory_order_relaxed)
DEFINE_SB(relacq, memory_order_release, memory_order_acquire)
DEFINE_SB(seqcst, memory_order_seq_cst, memory_order_seq_cst)

static int run_sb(void *(*t1)(void *), void *(*t2)(void *))
{
    Sb *s = &g_sb; /* reset it: no thread is running at this point */
    atomic_store(&s->go, 0);
    atomic_store(&s->done, 0);
    pthread_t a, b;
    CHECK(pthread_create(&a, NULL, t1, s) == 0);
    CHECK(pthread_create(&b, NULL, t2, s) == 0);
    int both_zero = 0;
    for (int round = 1; round <= SB_ROUNDS; round++) {
        atomic_store_explicit(&s->x, 0, memory_order_relaxed);
        atomic_store_explicit(&s->y, 0, memory_order_relaxed);
        /* release: the resets above are visible before the workers start */
        atomic_store_explicit(&s->go, round, memory_order_release);
        SPIN_UNTIL(&s->done, 2 * round, memory_order_acquire);
        if (s->r1 == 0 && s->r2 == 0)
            both_zero++;
    }
    CHECK(pthread_join(a, NULL) == 0);
    CHECK(pthread_join(b, NULL) == 0);
    return both_zero;
}

int main(void)
{
    printf("[1] message passing, %d rounds each (anomaly = flag seen, data old)\n",
           MP_ROUNDS);
    int relaxed = run_mp(MP_RELAXED);
    printf("    relaxed          : %d anomalies  (allowed! usually 0 on x86,"
           " try the board)\n", relaxed);
    int ra = run_mp(MP_REL_ACQ);
    printf("    release/acquire  : %d anomalies  (must be 0)\n", ra);
    CHECK(ra == 0);
    int fe = run_mp(MP_FENCES);
    printf("    relaxed + fences : %d anomalies  (must be 0)\n", fe);
    CHECK(fe == 0);

    printf("\n[2] store buffering (Dekker), %d rounds each: how often r1 == r2 == 0\n",
           SB_ROUNDS);
    int sb_rlx = run_sb(sb_t1_relaxed, sb_t2_relaxed);
    printf("    relaxed          : %5d times  (allowed; shows up on x86 too)\n",
           sb_rlx);
    int sb_ra = run_sb(sb_t1_relacq, sb_t2_relacq);
    printf("    release/acquire  : %5d times  (STILL allowed — store->load"
           " reordering)\n", sb_ra);
    int sb_sc = run_sb(sb_t1_seqcst, sb_t2_seqcst);
    printf("    seq_cst          : %5d times  (must be 0)\n", sb_sc);
    CHECK(sb_sc == 0);

    printf("\n(counts for relaxed/release-acquire depend on CPU, load and luck;"
           " only the 0s are guaranteed)\n");
    printf("\n03_memory_order_litmus: all checks passed\n");
    return 0;
}
