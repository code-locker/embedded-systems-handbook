/*
 * 06_deadlock_avoidance.c — bank transfers between accounts without
 *                           deadlock: lock ordering by id, by address, and
 *                           try-lock with back-off
 *
 * Concept : transfer(from, to) must lock BOTH accounts. If thread 1 does
 *           transfer(A, B) and thread 2 does transfer(B, A) and each locks
 *           "from" first, they can deadlock (see 07_deadlock_abba_bug.c).
 *           Break the "circular wait" condition:
 *             1. ORDER BY ID      : always lock the lower id first
 *             2. ORDER BY ADDRESS : same idea when objects have no id
 *             3. TRY AND BACK OFF : lock one, trylock the other, on failure
 *                                   release everything and retry later
 *           Money is conserved in every strategy: that is the check.
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/06_deadlock_avoidance.c
 *           make run P=linux-internals/03-mutexes-and-deadlock/c/06_deadlock_avoidance.c SAN=tsan
 * Read    : ../README.md  (sections 3.8 "Deadlock" and 3.9 "Avoiding it")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
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

#define N_ACCOUNTS 6
#define N_THREADS 4
#define N_TRANSFERS 10000
#define START_BALANCE 1000

typedef struct {
    int id;
    long balance; /* protected by lock */
    pthread_mutex_t lock;
} Account;

enum Strategy { BY_ID, BY_ADDRESS, TRY_BACKOFF };

typedef struct {
    Account *acc;
    enum Strategy strat;
    uint32_t seed;
    long retries; /* only TRY_BACKOFF: how often the trylock failed */
} Arg;

static uint32_t rnd(uint32_t *s)
{
    *s = *s * 1103515245u + 12345u; /* LCG: deterministic per thread */
    return *s >> 8;
}

/* Move money while holding both locks. Never lets a balance go negative. */
static void move_money(Account *from, Account *to, long amount)
{
    if (from->balance >= amount) {
        from->balance -= amount;
        to->balance += amount;
    }
}

/* ------------------------------------------------------------------------
 * 1. Global order by id.
 *
 *    T1: transfer(A=1, B=2)  locks 1 then 2
 *    T2: transfer(B=2, A=1)  locks 1 then 2   <- same order, no cycle
 *    Whoever gets lock 1 first finishes; the other just waits for it.
 * ---------------------------------------------------------------------- */
static void transfer_by_id(Account *from, Account *to, long amount)
{
    Account *first = from->id < to->id ? from : to;
    Account *second = from->id < to->id ? to : from;
    pthread_mutex_lock(&first->lock);
    pthread_mutex_lock(&second->lock);
    move_money(from, to, amount);
    pthread_mutex_unlock(&second->lock); /* unlock order does not matter for */
    pthread_mutex_unlock(&first->lock);  /* deadlock; reverse is customary  */
}

/* ------------------------------------------------------------------------
 * 2. Order by address: works for any objects, no id field needed.
 *    Compare as uintptr_t: comparing pointers into DIFFERENT objects with
 *    `<` is undefined in ISO C, comparing integers is not. The Linux kernel
 *    does exactly this, e.g. double_rq_lock() (two runqueues) and
 *    lock_two_nondirectories() (two inodes).
 * ---------------------------------------------------------------------- */
static void transfer_by_address(Account *from, Account *to, long amount)
{
    int from_first = (uintptr_t)from < (uintptr_t)to;
    pthread_mutex_t *a = from_first ? &from->lock : &to->lock;
    pthread_mutex_t *b = from_first ? &to->lock : &from->lock;
    pthread_mutex_lock(a);
    pthread_mutex_lock(b);
    move_money(from, to, amount);
    pthread_mutex_unlock(b);
    pthread_mutex_unlock(a);
}

/* ------------------------------------------------------------------------
 * 3. Try-lock and back off: breaks "hold and wait". Use it when no global
 *    order exists (e.g. the second lock is only known after taking the
 *    first). Risk: LIVELOCK — two threads that keep backing off in lockstep
 *    and retrying forever. The randomised, growing pause breaks the symmetry
 *    (the same trick as Ethernet's exponential back-off).
 * ---------------------------------------------------------------------- */
static void transfer_try_backoff(Account *from, Account *to, long amount,
                                 uint32_t *seed, long *retries)
{
    unsigned spins = 1;
    for (;;) {
        pthread_mutex_lock(&from->lock);
        int rc = pthread_mutex_trylock(&to->lock);
        if (rc == 0)
            break; /* have both */
        CHECK(rc == EBUSY);
        pthread_mutex_unlock(&from->lock); /* give back what we hold */
        (*retries)++;
        unsigned pause = rnd(seed) % spins + 1;
        for (unsigned i = 0; i < pause; i++)
            sched_yield();
        if (spins < 64)
            spins *= 2;
    }
    move_money(from, to, amount);
    pthread_mutex_unlock(&to->lock);
    pthread_mutex_unlock(&from->lock);
}

static void *worker(void *p)
{
    Arg *a = p;
    for (int i = 0; i < N_TRANSFERS; i++) {
        int f = (int)(rnd(&a->seed) % N_ACCOUNTS);
        int t = (int)(rnd(&a->seed) % N_ACCOUNTS);
        if (f == t)
            continue; /* locking the same NORMAL mutex twice = self-deadlock */
        long amount = (long)(rnd(&a->seed) % 50) + 1;
        Account *from = &a->acc[f], *to = &a->acc[t];
        switch (a->strat) {
        case BY_ID: transfer_by_id(from, to, amount); break;
        case BY_ADDRESS: transfer_by_address(from, to, amount); break;
        case TRY_BACKOFF:
            transfer_try_backoff(from, to, amount, &a->seed, &a->retries);
            break;
        }
    }
    return NULL;
}

static void run(enum Strategy s, const char *name)
{
    /* A fresh set of accounts (and mutexes) per strategy: mixing two lock
     * orders on the SAME mutexes is exactly what creates a deadlock risk —
     * and ThreadSanitizer would rightly flag it. */
    Account acc[N_ACCOUNTS];
    for (int i = 0; i < N_ACCOUNTS; i++) {
        acc[i].id = i;
        acc[i].balance = START_BALANCE;
        CHECK(pthread_mutex_init(&acc[i].lock, NULL) == 0);
    }
    pthread_t th[N_THREADS];
    Arg args[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) {
        args[i] = (Arg){acc, s, 12345u + 7919u * (uint32_t)i, 0};
        CHECK(pthread_create(&th[i], NULL, worker, &args[i]) == 0);
    }
    long retries = 0;
    for (int i = 0; i < N_THREADS; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        retries += args[i].retries;
    }
    long total = 0;
    for (int i = 0; i < N_ACCOUNTS; i++) {
        CHECK(acc[i].balance >= 0);
        total += acc[i].balance;
        CHECK(pthread_mutex_destroy(&acc[i].lock) == 0);
    }
    printf("    %-18s: %d threads x %d transfers finished, total money %ld "
           "(expected %d)", name, N_THREADS, N_TRANSFERS, total,
           N_ACCOUNTS * START_BALANCE);
    if (s == TRY_BACKOFF)
        printf(", %ld back-offs (timing-dependent)", retries);
    printf("\n");
    CHECK(total == (long)N_ACCOUNTS * START_BALANCE);
}

int main(void)
{
    /*
     * The four Coffman conditions (1971). Deadlock needs ALL four:
     *   1. mutual exclusion   - a lock has one owner      (that's the point)
     *   2. hold and wait      - hold A while waiting for B (strategy 3 breaks)
     *   3. no preemption      - nobody can take A away from you
     *   4. circular wait      - T1 waits for T2 waits for T1 (1 and 2 break)
     */
    printf("[1] money transfers among %d accounts, three deadlock-free "
           "strategies\n", N_ACCOUNTS);
    run(BY_ID, "lock order by id");
    run(BY_ADDRESS, "lock order by addr");
    run(TRY_BACKOFF, "trylock + back-off");
    printf("06_deadlock_avoidance: all checks passed\n");
    return 0;
}
