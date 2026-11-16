/*
 * 08_naive_spinlock_bug.c — INTENTIONALLY BROKEN: a "spinlock" written with
 *                           a plain int and two separate steps
 *
 * Concept : The lock below checks "is it free?" and then, in a SEPARATE
 *           step, sets it to taken. Two threads can both see it free and
 *           both enter the critical section (check-then-act race). It is
 *           also a data race in C11 terms (plain int, two threads, at least
 *           one writing): undefined behaviour. The fix is ONE atomic
 *           read-modify-write (exchange / test_and_set, see 02).
 *
 * Which tool finds it and how:
 *   1. ThreadSanitizer:
 *        make run P=linux-internals/06-spinlocks-and-futex/c/08_naive_spinlock_bug.c SAN=tsan
 *      prints "WARNING: ThreadSanitizer: data race" on `g_locked` (and on
 *      g_counter), with both threads' stacks.
 *   2. The program itself: it counts how many threads were inside the
 *      critical section at once and how many increments were lost, and
 *      exits 1 when it caught the race (usual on a multi-core machine).
 *   3. The optimiser: build with `make OPT=1 SAN=0` and run it under
 *      `timeout 5 ./build/linux-internals/06-spinlocks-and-futex/c/08_naive_spinlock_bug`.
 *      GCC may assume no other thread touches these plain variables (a data
 *      race is UB), so it may hoist the load of g_locked out of the while
 *      loop (endless loop: exit status 124 = hung) or keep values in
 *      registers across iterations (counts come out wildly wrong; GCC 11 on
 *      x86-64 lost exactly half the increments). Both are legal.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/08_naive_spinlock_bug.c SAN=tsan
 * Read    : ../README.md  (section "Common mistakes and pitfalls")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define ITERS 200000

static int g_locked;      /* BUG: plain int used as a lock */
static long g_counter;    /* "protected" by g_locked */
static int g_inside;      /* how many threads are in the critical section */
static int g_max_inside;

static void naive_lock(void)
{
    while (g_locked) /* step 1: test  (another thread can run here...)   */
        ;
    g_locked = 1;    /* step 2: set   (...and also see 0 before this)    */
}

static void naive_unlock(void)
{
    g_locked = 0; /* BUG: no release ordering either */
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++) {
        naive_lock();
        int in = ++g_inside;
        if (in > g_max_inside)
            g_max_inside = in;
        long v = g_counter;
        g_counter = v + 1;
        g_inside--;
        naive_unlock();
    }
    return NULL;
}

int main(void)
{
    pthread_t a, b;
    pthread_create(&a, NULL, worker, NULL);
    pthread_create(&b, NULL, worker, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    long want = 2L * ITERS;
    printf("counter %ld, expected %ld, lost %ld; max threads inside the "
           "critical section at once: %d\n", g_counter, want, want - g_counter,
           g_max_inside);
    if (g_counter != want || g_max_inside > 1) {
        printf("08_naive_spinlock_bug: RACE CAUGHT (mutual exclusion broken)\n");
        return 1;
    }
    printf("08_naive_spinlock_bug: race not observed this run "
           "(it is still there: run under SAN=tsan)\n");
    return 0;
}
