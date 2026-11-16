/*
 * 02_atomic_counter_and_cas.c — C11 <stdatomic.h>: atomic_fetch_add,
 *                               compare-exchange loops, atomic_flag spinlock,
 *                               and a race condition WITHOUT a data race
 *
 * Concept : An atomic read-modify-write (RMW) does load+modify+store as ONE
 *           indivisible step, so no update is lost. When the "modify" is
 *           more than an add (max, bounded increment, push to a list) you
 *           write a compare-and-swap (CAS) loop: read, compute, "store only
 *           if nobody changed it meanwhile", else retry. Atomics remove
 *           DATA races; they do not make a check-then-act sequence correct —
 *           that is a RACE CONDITION, a logic bug no sanitizer reports.
 * Build   : make run P=linux-internals/02-race-conditions-and-atomics/c/02_atomic_counter_and_cas.c
 * Read    : ../README.md  (sections 3.1, 3.4, 3.5)
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

#define NTHREADS 4
#define ITERS 100000

static void run_threads(void *(*fn)(void *), void *arg)
{
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, fn, arg) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
}

/* ===== 1. atomic_fetch_add: the fixed counter ========================= */

static atomic_int g_counter; /* zero-initialised static: valid atomic */

static void *inc_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++)
        /* One instruction-level RMW:
         *   x86-64      : lock xadd
         *   ARMv8.0     : ldxr / add / stxr / cbnz retry   (LL/SC loop)
         *   ARMv8.1 LSE : ldadd                            (one instruction)
         * Relaxed is enough for a pure counter: we only need atomicity, not
         * ordering with other variables (the final read is after join). */
        atomic_fetch_add_explicit(&g_counter, 1, memory_order_relaxed);
    return NULL;
}

static void demo_fetch_add(void)
{
    printf("[1] atomic_fetch_add: %d threads x %d increments\n", NTHREADS, ITERS);
    run_threads(inc_worker, NULL);
    int v = atomic_load(&g_counter);
    printf("    counter = %d (expected %d) — compare 01_racy_counter_bug.c\n", v,
           NTHREADS * ITERS);
    CHECK(v == NTHREADS * ITERS);
    CHECK(atomic_is_lock_free(&g_counter)); /* a real CPU atomic, no hidden lock */
}

/* ===== 2. CAS loop: atomic maximum ===================================== */

static atomic_int g_max = -1;

/*
 * The canonical CAS loop:
 *
 *   old = load(x)
 *   do {
 *       new = f(old)                       compute from the snapshot
 *   } while (!compare_exchange_weak(&x, &old, new));
 *            \__ if x == old: x = new, return true
 *                else:        old = current x, return false -> recompute
 *
 * _weak may fail SPURIOUSLY (x == old but the store still failed) because
 * on LL/SC machines (ARM ldxr/stxr) the exclusive monitor can be lost to an
 * interrupt or cache eviction. Inside a retry loop that costs nothing and
 * avoids a nested loop, so use _weak in loops, _strong for one-shot tests.
 */
static void atomic_max(atomic_int *x, int v)
{
    int old = atomic_load_explicit(x, memory_order_relaxed);
    while (old < v &&
           !atomic_compare_exchange_weak_explicit(x, &old, v, memory_order_relaxed,
                                                  memory_order_relaxed))
        ; /* `old` was refreshed by the failed CAS — just loop */
}

static atomic_int g_next_id;

static void *max_worker(void *arg)
{
    (void)arg;
    int id = atomic_fetch_add(&g_next_id, 1);
    for (int i = 0; i < ITERS / 10; i++)
        atomic_max(&g_max, (i * 7919 + id * 104729) % 1000003);
    return NULL;
}

static void demo_cas_max(void)
{
    printf("\n[2] compare_exchange_weak loop: atomic maximum\n");
    run_threads(max_worker, NULL);
    /* single-threaded reference */
    int want = -1;
    for (int id = 0; id < NTHREADS; id++)
        for (int i = 0; i < ITERS / 10; i++) {
            int v = (i * 7919 + id * 104729) % 1000003;
            if (v > want)
                want = v;
        }
    printf("    atomic max = %d, sequential max = %d\n", atomic_load(&g_max), want);
    CHECK(atomic_load(&g_max) == want);
}

/* ===== 3. race condition without a data race: check-then-act ========== */

#define LIMIT 1000 /* e.g. max open connections, buffer slots */

static atomic_int g_slots_racy, g_slots_cas;
static atomic_int g_granted_racy, g_granted_cas;

/* WRONG: each step is atomic, the SEQUENCE is not.
 *
 *   T1: load -> 999 (< LIMIT, ok)
 *                         T2: load -> 999 (< LIMIT, ok)
 *   T1: fetch_add -> 1000
 *                         T2: fetch_add -> 1001   limit exceeded!
 *
 * TSan is silent here: every access is atomic, so there is no data race. */
static bool try_acquire_racy(void)
{
    if (atomic_load(&g_slots_racy) < LIMIT) {
        sched_yield(); /* widen the window so the bug shows up more often */
        atomic_fetch_add(&g_slots_racy, 1);
        return true;
    }
    return false;
}

/* RIGHT: decide and update in ONE atomic step with CAS. */
static bool try_acquire_cas(void)
{
    int cur = atomic_load_explicit(&g_slots_cas, memory_order_relaxed);
    do {
        if (cur >= LIMIT)
            return false;
    } while (!atomic_compare_exchange_weak_explicit(
        &g_slots_cas, &cur, cur + 1, memory_order_acq_rel, memory_order_relaxed));
    return true;
}

static void *slot_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LIMIT / 2; i++) { /* 4 x 500 = 2000 attempts total */
        if (try_acquire_racy())
            atomic_fetch_add(&g_granted_racy, 1);
        if (try_acquire_cas())
            atomic_fetch_add(&g_granted_cas, 1);
    }
    return NULL;
}

static void demo_check_then_act(void)
{
    printf("\n[3] check-then-act: a race condition with no data race\n");
    run_threads(slot_worker, NULL);
    printf("    racy version granted %d slots (limit %d) — over by %d"
           " (timing-dependent, may be 0)\n",
           atomic_load(&g_granted_racy), LIMIT, atomic_load(&g_granted_racy) - LIMIT);
    printf("    CAS  version granted %d slots\n", atomic_load(&g_granted_cas));
    CHECK(atomic_load(&g_granted_racy) >= LIMIT); /* never under */
    CHECK(atomic_load(&g_granted_cas) == LIMIT);  /* never over */
}

/* ===== 4. atomic_flag: the one type guaranteed lock-free =============== */

/*
 * atomic_flag is the only atomic type C11 GUARANTEES to be lock-free, so it
 * is the building block for everything else. Here: a minimal spinlock that
 * protects a NON-atomic pair of fields which must always stay equal.
 *
 *   lock:   while (test_and_set(acquire)) spin;   "I own it if it was clear"
 *   unlock: clear(release)                         publish my writes
 *
 * acquire on lock / release on unlock is what makes the protected writes
 * visible to the next owner (more in 03_memory_order_litmus.c). The real
 * spinlock with backoff, and why user space should usually use a mutex
 * instead, is lesson 06-spinlocks-and-futex.
 */
static atomic_flag g_lock = ATOMIC_FLAG_INIT;
static struct { long a, b; } g_pair; /* invariant: a == b */
static atomic_int g_torn;

static void spin_lock(atomic_flag *f)
{
    while (atomic_flag_test_and_set_explicit(f, memory_order_acquire))
        sched_yield(); /* polite: there may be fewer cores than threads */
}

static void spin_unlock(atomic_flag *f)
{
    atomic_flag_clear_explicit(f, memory_order_release);
}

static void *pair_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS / 10; i++) {
        spin_lock(&g_lock);
        if (g_pair.a != g_pair.b)
            atomic_fetch_add(&g_torn, 1);
        g_pair.a++;
        g_pair.b++;
        spin_unlock(&g_lock);
    }
    return NULL;
}

static void demo_atomic_flag(void)
{
    printf("\n[4] atomic_flag spinlock guarding a two-field invariant\n");
    run_threads(pair_worker, NULL);
    printf("    a = %ld, b = %ld, torn observations = %d\n", g_pair.a, g_pair.b,
           atomic_load(&g_torn));
    CHECK(g_pair.a == (long)NTHREADS * (ITERS / 10) && g_pair.a == g_pair.b);
    CHECK(atomic_load(&g_torn) == 0);
}

/* ===== 5. other RMW operations ======================================== */
static void demo_other_rmw(void)
{
    printf("\n[5] exchange, fetch_or/and, strong CAS (single thread, semantics only)\n");
    atomic_uint flags = 0;
    unsigned prev = atomic_fetch_or(&flags, 0x5u);  /* set bits, return OLD */
    CHECK(prev == 0 && atomic_load(&flags) == 0x5u);
    prev = atomic_fetch_and(&flags, ~0x1u);         /* clear bit 0 */
    CHECK(prev == 0x5u && atomic_load(&flags) == 0x4u);
    prev = atomic_exchange(&flags, 0x80u);          /* swap in, return old */
    CHECK(prev == 0x4u && atomic_load(&flags) == 0x80u);

    unsigned expected = 0x80u;
    CHECK(atomic_compare_exchange_strong(&flags, &expected, 0x81u));
    expected = 0x99u; /* wrong guess: fails and tells us the real value */
    CHECK(!atomic_compare_exchange_strong(&flags, &expected, 0u));
    CHECK(expected == 0x81u);
    printf("    fetch_or / fetch_and / exchange / CAS behave as documented\n");
}

int main(void)
{
    demo_fetch_add();
    demo_cas_max();
    demo_check_then_act();
    demo_atomic_flag();
    demo_other_rmw();
    printf("\n02_atomic_counter_and_cas: all checks passed\n");
    return 0;
}
