/*
 * 07_deadlock_abba_bug.c — INTENTIONALLY BROKEN: the classic ABBA deadlock
 *
 * Concept : Thread 1 locks A then B. Thread 2 locks B then A. Two barriers
 *           make the deadlock happen on EVERY run:
 *
 *              thread 1                     thread 2
 *              lock(A) lock(B) ... unlock   (one good transfer: A -> B)
 *              ---------------- barrier "warmed_up" -----------------
 *              lock(A)  ok                  lock(B)  ok
 *              ---------------- barrier "both_hold_one" -------------
 *              lock(B)  waits for T2  <-+   lock(A)  waits for T1
 *                                       +-- cycle: nobody ever proceeds
 *
 *           Without tools the program simply hangs (no CPU use, no output).
 *
 * Find it :
 *   1. ThreadSanitizer reports "lock-order-inversion (potential deadlock)"
 *      with both stacks when thread 2, holding B, asks for A:
 *        TSAN_OPTIONS=halt_on_error=1 timeout 5 \
 *          make run P=linux-internals/03-mutexes-and-deadlock/c/07_deadlock_abba_bug.c SAN=tsan
 *      (halt_on_error makes TSan exit after the report; without it the
 *      program reports and then hangs, hence `timeout 5`).
 *      How TSan knows: it keeps a graph of "lock X was taken while holding
 *      Y" edges, built from SUCCESSFUL acquisitions. The warm-up transfer
 *      records A -> B; thread 2 asking for A while holding B would close a
 *      cycle -> report. That is also why TSan finds the bug on runs that
 *      happen NOT to deadlock. Without the warm-up, the very first attempt
 *      deadlocks before any A -> B edge exists and TSan stays silent: it
 *      predicts deadlocks from lock ORDER, it does not watch for hangs.
 *   2. Plain build: it hangs. `timeout 5 make run P=...` exits with 124.
 *      Attach gdb to the hung process: `gdb -p $(pidof 07_deadlock_abba_bug)`
 *      then `thread apply all bt` shows both threads in
 *      __lll_lock_wait / futex_wait, each inside the other's lock call.
 *      `cat /proc/<pid>/task/<tid>/wchan` says futex_wait_queue.
 *   3. Valgrind: `make SAN=0 ...; valgrind --tool=helgrind ./build/...`
 *      reports "lock order violated".
 * Fix     : one global lock order (06_deadlock_avoidance.c) or
 *           std::scoped_lock in C++ (../cpp/01_std_mutex_raii.cpp).
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/07_deadlock_abba_bug.c
 * Read    : ../README.md  (section 3.8 "Deadlock")
 */
#define _GNU_SOURCE
#include <pthread.h>
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

static pthread_mutex_t lock_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t lock_b = PTHREAD_MUTEX_INITIALIZER;
static pthread_barrier_t warmed_up, both_hold_one;
static int balance_a = 100, balance_b = 100;

static void *thread1(void *arg)
{
    (void)arg;
    /* One transfer that works: records "B taken while holding A". */
    pthread_mutex_lock(&lock_a);
    pthread_mutex_lock(&lock_b);
    balance_a -= 1;
    balance_b += 1;
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    pthread_barrier_wait(&warmed_up);

    pthread_mutex_lock(&lock_a);
    printf("T1: holds A, wants B\n");
    fflush(stdout);
    pthread_barrier_wait(&both_hold_one);
    pthread_mutex_lock(&lock_b); /* BUG: order A -> B */
    balance_a -= 10;
    balance_b += 10;
    pthread_mutex_unlock(&lock_b);
    pthread_mutex_unlock(&lock_a);
    return NULL;
}

static void *thread2(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&warmed_up);
    pthread_mutex_lock(&lock_b);
    printf("T2: holds B, wants A\n");
    fflush(stdout);
    pthread_barrier_wait(&both_hold_one);
    pthread_mutex_lock(&lock_a); /* BUG: order B -> A  (inverted!) */
    balance_b -= 20;
    balance_a += 20;
    pthread_mutex_unlock(&lock_a);
    pthread_mutex_unlock(&lock_b);
    return NULL;
}

int main(void)
{
    CHECK(pthread_barrier_init(&warmed_up, NULL, 2) == 0);
    CHECK(pthread_barrier_init(&both_hold_one, NULL, 2) == 0);
    pthread_t t1, t2;
    CHECK(pthread_create(&t1, NULL, thread1, NULL) == 0);
    CHECK(pthread_create(&t2, NULL, thread2, NULL) == 0);
    printf("main: joining... (if nothing follows, the threads are deadlocked;"
           " Ctrl-C or `timeout 5`)\n");
    fflush(stdout);
    CHECK(pthread_join(t1, NULL) == 0); /* never returns */
    CHECK(pthread_join(t2, NULL) == 0);
    printf("balances %d %d (unreachable)\n", balance_a, balance_b);
    pthread_barrier_destroy(&both_hold_one);
    pthread_barrier_destroy(&warmed_up);
    return 0;
}
