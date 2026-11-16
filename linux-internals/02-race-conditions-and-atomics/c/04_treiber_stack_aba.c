/*
 * 04_treiber_stack_aba.c — lock-free Treiber stack on a node pool, the ABA
 *                          problem shown step by step, and the tag fix
 *
 * Concept : A Treiber stack pushes and pops with one CAS on `top`. CAS only
 *           checks "is top still the value I read?" — not "has nothing
 *           happened since". If top went A -> B -> A in between (ABA), a
 *           stale CAS succeeds and corrupts the list. Fix: pack a version
 *           TAG next to the pointer and bump it on every change, so A(tag 5)
 *           != A(tag 7). Nodes are 32-bit indices into a pool so index+tag
 *           fit in one lock-free 64-bit atomic (a 16-byte pointer+tag CAS
 *           would need cmpxchg16b / CASP and libatomic).
 * Build   : make run P=linux-internals/02-race-conditions-and-atomics/c/04_treiber_stack_aba.c
 * Read    : ../README.md  (section 3.10)
 */
#define _GNU_SOURCE
#include <pthread.h>
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

#define NIL UINT32_MAX
#define NNODES 64
#define NTHREADS 4
#define ITERS 20000

/*
 * Nodes live in a fixed pool and are NEVER freed, only recycled. That is
 * what makes reading `pool[top].next` safe even if another thread popped
 * that node a moment ago: the memory is still a node (just maybe not on
 * the stack any more). With malloc'd nodes the same read could be a
 * use-after-free — the "safe memory reclamation" problem solved by hazard
 * pointers, epochs or RCU (lesson 07-rwlocks-seqlocks-and-rcu).
 *
 * `next` is atomic (relaxed) because a slow popper may read it while the
 * node's new owner rewrites it before pushing it again. That read result is
 * then discarded by the failing CAS, but in C it must still not be a data
 * race.
 */
typedef struct {
    _Atomic uint32_t next;
    int value;      /* only touched by the thread that currently owns it */
    atomic_int owner; /* 0 = on the stack; else id of the thread holding it */
} Node;

static Node pool[NNODES];

/* ===== top word = [ tag:32 | index:32 ] ================================= */
static inline uint64_t pack(uint32_t tag, uint32_t idx) { return (uint64_t)tag << 32 | idx; }
static inline uint32_t idx_of(uint64_t w) { return (uint32_t)w; }
static inline uint32_t tag_of(uint64_t w) { return (uint32_t)(w >> 32); }

typedef struct {
    _Atomic uint64_t top;
} TaggedStack;

static void tagged_push(TaggedStack *s, uint32_t n)
{
    uint64_t old = atomic_load_explicit(&s->top, memory_order_relaxed);
    uint64_t neu;
    do {
        atomic_store_explicit(&pool[n].next, idx_of(old), memory_order_relaxed);
        neu = pack(tag_of(old) + 1, n); /* every change bumps the tag */
        /* release: the node's contents (value, next) are visible to whoever
         * pops it with acquire */
    } while (!atomic_compare_exchange_weak_explicit(&s->top, &old, neu,
                                                    memory_order_release,
                                                    memory_order_relaxed));
}

static uint32_t tagged_pop(TaggedStack *s)
{
    uint64_t old = atomic_load_explicit(&s->top, memory_order_acquire);
    uint64_t neu;
    do {
        if (idx_of(old) == NIL)
            return NIL;
        uint32_t next = atomic_load_explicit(&pool[idx_of(old)].next,
                                             memory_order_relaxed);
        neu = pack(tag_of(old) + 1, next);
    } while (!atomic_compare_exchange_weak_explicit(&s->top, &old, neu,
                                                    memory_order_acquire,
                                                    memory_order_acquire));
    return idx_of(old);
}

/* ===== 1. ABA, replayed deterministically ============================== */

/*
 * A pop is: read top (A), read A.next (B), CAS(top: A -> B). We split it in
 * two halves so a "second thread" can run in between, single-threaded and
 * repeatable:
 *
 *   start:            top -> A -> B -> C
 *   T1 pop, part 1:   sees top = A, next = B             ... T1 is preempted
 *   T2: pop A         top -> B -> C
 *   T2: pop B         top -> C               (T2 now owns B and uses it)
 *   T2: push A        top -> A -> C          (A is back on top!)
 *   T1 pop, part 2:   CAS(top: A -> B)
 *        untagged: top == A, so it SUCCEEDS:  top -> B -> C
 *                  B is owned by T2 and now ALSO on the stack (the next
 *                  pop hands it out twice), and A — which T2 just gave
 *                  back — has vanished from the stack
 *        tagged  : top is A/tag 4, T1 expected A/tag 1, CAS FAILS -> retry
 */
static int demo_aba(bool tagged)
{
    enum { A = 0, B = 1, C = 2 };
    TaggedStack s;
    atomic_init(&s.top, pack(0, NIL));
    tagged_push(&s, C);
    tagged_push(&s, B);
    tagged_push(&s, A); /* top -> A -> B -> C, tag 3 */

    /* T1, part 1 */
    uint64_t t1_seen = atomic_load(&s.top);
    uint32_t t1_next = atomic_load(&pool[idx_of(t1_seen)].next);
    CHECK(idx_of(t1_seen) == A && t1_next == B);

    /* T2 runs */
    CHECK(tagged_pop(&s) == A);
    CHECK(tagged_pop(&s) == B);
    tagged_push(&s, A);

    /* T1, part 2: the CAS. Untagged = compare only the index half, which is
     * exactly what a plain-pointer Treiber stack does. */
    bool ok;
    if (tagged) {
        uint64_t expected = t1_seen;
        ok = atomic_compare_exchange_strong(&s.top, &expected,
                                            pack(tag_of(t1_seen) + 1, t1_next));
    } else {
        uint64_t cur = atomic_load(&s.top);
        ok = idx_of(cur) == idx_of(t1_seen); /* "pointer" comparison */
        if (ok)
            atomic_store(&s.top, pack(tag_of(cur) + 1, t1_next));
    }

    /* walk what is now on the stack */
    int count = 0;
    bool has_b = false;
    for (uint32_t i = idx_of(atomic_load(&s.top)); i != NIL && count < 10;
         i = atomic_load(&pool[i].next)) {
        count++;
        has_b |= (i == B);
    }
    printf("    %-8s: stale CAS %s; stack now holds %d node(s)%s\n",
           tagged ? "tagged" : "untagged", ok ? "SUCCEEDED" : "failed (good)",
           count, has_b ? " (B, C): B is owned by T2, A is lost -> CORRUPT" : " (A, C)");
    if (tagged)
        CHECK(!ok && count == 2 && !has_b); /* A -> C, intact */
    else
        CHECK(ok && has_b); /* the corruption, demonstrated */
    return count;
}

/* ===== 2. multi-threaded stress with the tagged stack ================== */

static TaggedStack g_stack;
static atomic_int g_double_owner, g_empty_pops;

static void *stress_worker(void *arg)
{
    int me = (int)(intptr_t)arg;
    for (int i = 0; i < ITERS; i++) {
        uint32_t n = tagged_pop(&g_stack);
        if (n == NIL) { /* all 64 nodes are out at once — retry */
            atomic_fetch_add_explicit(&g_empty_pops, 1, memory_order_relaxed);
            continue;
        }
        /* If the stack were broken, two threads could pop the same node. */
        int prev = atomic_exchange(&pool[n].owner, me);
        if (prev != 0)
            atomic_fetch_add(&g_double_owner, 1);
        pool[n].value = me * 1000000 + i; /* exclusive use of the node */
        CHECK(pool[n].value == me * 1000000 + i);
        atomic_store(&pool[n].owner, 0);
        tagged_push(&g_stack, n);
    }
    return NULL;
}

static void demo_stress(void)
{
    printf("\n[2] tagged Treiber stack: %d threads x %d pop/push on %d nodes\n",
           NTHREADS, ITERS, NNODES);
    atomic_init(&g_stack.top, pack(0, NIL));
    for (uint32_t i = 0; i < NNODES; i++)
        tagged_push(&g_stack, i);

    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, stress_worker, (void *)(intptr_t)(i + 1)) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    /* Every node must be back exactly once. */
    bool seen[NNODES] = {false};
    int count = 0;
    for (uint32_t i = idx_of(atomic_load(&g_stack.top)); i != NIL;
         i = atomic_load(&pool[i].next)) {
        CHECK(i < NNODES && !seen[i]);
        seen[i] = true;
        count++;
    }
    uint64_t top = atomic_load(&g_stack.top);
    printf("    nodes back on the stack: %d / %d, double ownership: %d,"
           " empty pops: %d, final tag: %u\n", count, NNODES,
           atomic_load(&g_double_owner), atomic_load(&g_empty_pops), tag_of(top));
    CHECK(count == NNODES);
    CHECK(atomic_load(&g_double_owner) == 0);
}

int main(void)
{
    printf("[1] the ABA problem, replayed step by step\n");
    for (int i = 0; i < NNODES; i++)
        atomic_init(&pool[i].next, NIL);
    demo_aba(false);
    demo_aba(true);

    /* index+tag fit one 64-bit word: a real CPU atomic on x86-64 and
     * arm64, no hidden lock in libatomic */
    CHECK(atomic_is_lock_free(&g_stack.top));

    demo_stress();
    printf("\n04_treiber_stack_aba: all checks passed\n");
    return 0;
}
