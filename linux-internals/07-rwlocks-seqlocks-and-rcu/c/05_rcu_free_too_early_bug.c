/*
 * 05_rcu_free_too_early_bug.c — INTENTIONALLY BROKEN: the writer publishes
 *                               a new pointer and frees the old object
 *                               WITHOUT waiting for a grace period
 *
 * Concept : Publishing a new pointer does not stop readers who loaded the
 *           OLD pointer a moment earlier. They are still using it. Freeing
 *           it immediately is a use-after-free in every reader that was
 *           "in flight". That is the whole reason RCU has grace periods
 *           (synchronize_rcu / call_rcu, see 04_rcu_userspace.c).
 *           The program forces the bad interleaving with two flags, so the
 *           bug happens on EVERY run, not once a month in the field:
 *
 *             reader                     writer
 *             p = load(g_cfg)  -------> (reader_has_pointer = 1)
 *                                        publish new; free(old)  <- BUG
 *             (writer_freed = 1) <------
 *             read p->value              <- use after free
 *
 * Which tool finds it and how:
 *   AddressSanitizer (the default build):
 *     make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/05_rcu_free_too_early_bug.c
 *   prints "ERROR: AddressSanitizer: heap-use-after-free" with three
 *   stacks: the bad READ (reader), where it was FREED (writer), and where
 *   it was ALLOCATED. ThreadSanitizer (SAN=tsan) reports it too:
 *   "WARNING: ThreadSanitizer: heap-use-after-free". Without a sanitizer
 *   it usually prints a plausible value and nothing looks wrong.
 *   Fix: call synchronize_rcu() before free(),
 *   or hand the old object to a deferred reclaimer (call_rcu).
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/c/05_rcu_free_too_early_bug.c
 * Read    : ../README.md  (section "RCU", grace periods)
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { int value; } Config;

static _Atomic(Config *) g_cfg;
static atomic_int reader_has_pointer, writer_freed;

static void *reader(void *arg)
{
    (void)arg;
    Config *p = atomic_load_explicit(&g_cfg, memory_order_acquire);
    atomic_store(&reader_has_pointer, 1);
    while (!atomic_load(&writer_freed)) /* stand-in for "reader got preempted" */
        sched_yield();
    printf("reader sees value %d\n", p->value); /* BUG: p was freed */
    return NULL;
}

int main(void)
{
    Config *old = malloc(sizeof *old);
    if (!old)
        return 1;
    old->value = 1;
    atomic_store(&g_cfg, old);

    pthread_t t;
    pthread_create(&t, NULL, reader, NULL);
    while (!atomic_load(&reader_has_pointer))
        sched_yield();

    Config *fresh = malloc(sizeof *fresh);
    if (!fresh)
        return 1;
    fresh->value = 2;
    atomic_store_explicit(&g_cfg, fresh, memory_order_release); /* publish */
    free(old); /* BUG: no grace period, the reader still holds `old` */
    atomic_store(&writer_freed, 1);

    pthread_join(t, NULL);
    free(atomic_load(&g_cfg));
    printf("05_rcu_free_too_early_bug: finished (without ASan the bug is "
           "silent: that is what makes it dangerous)\n");
    return 0;
}
