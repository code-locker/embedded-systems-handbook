/*
 * 02_resource_pool.c — a counting semaphore limits concurrency to N:
 *                      a pool of 3 "DMA channels" shared by 8 threads
 *
 * Concept : Initialise the semaphore to the number of identical resources.
 *           sem_wait = take one (sleep if none left), sem_post = give it
 *           back. The semaphore only COUNTS; WHICH channel you got is a
 *           separate question, answered here by a small mutex-protected
 *           free-list. The program measures the maximum number of threads
 *           inside at once and checks it never exceeds N.
 * Build   : make run P=linux-internals/05-semaphores/c/02_resource_pool.c
 *           make run P=linux-internals/05-semaphores/c/02_resource_pool.c SAN=tsan
 * Read    : ../README.md  (section 3.4 "Resource pool")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdbool.h>
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

#define N_CHANNELS 3
#define N_THREADS 8
#define JOBS_PER_THREAD 6

/*
 *   sem value = free channels         free-list (mutex)       in use
 *       3      [ch0 ch1 ch2]                                   -
 *   T0 wait -> 2   take ch0                                    ch0
 *   T1 wait -> 1   take ch1                                    ch0 ch1
 *   T2 wait -> 0   take ch2                                    ch0 ch1 ch2
 *   T3 wait -> sleeps  ..........  T1 puts ch1 back, post -> T3 wakes, takes ch1
 *
 *   Order matters: take from the list AFTER sem_wait (a channel is
 *   guaranteed to be there), put back BEFORE sem_post (so the woken thread
 *   finds it).
 */
typedef struct {
    sem_t free_count;
    pthread_mutex_t list_lock;
    bool in_use[N_CHANNELS]; /* guarded by list_lock */
    long uses[N_CHANNELS];   /* guarded by list_lock */
    atomic_int inside;       /* threads currently holding a channel */
    atomic_int max_inside;
} Pool;

static void sem_wait_retry(sem_t *s)
{
    while (sem_wait(s) == -1)
        CHECK(errno == EINTR);
}

static int pool_acquire(Pool *p)
{
    sem_wait_retry(&p->free_count); /* a channel is now reserved for us */
    pthread_mutex_lock(&p->list_lock);
    int ch = -1;
    for (int i = 0; i < N_CHANNELS; i++)
        if (!p->in_use[i]) {
            ch = i;
            break;
        }
    CHECK(ch >= 0); /* the semaphore guarantees one is free */
    p->in_use[ch] = true;
    p->uses[ch]++;
    pthread_mutex_unlock(&p->list_lock);
    return ch;
}

static void pool_release(Pool *p, int ch)
{
    pthread_mutex_lock(&p->list_lock);
    CHECK(p->in_use[ch]);
    p->in_use[ch] = false;
    pthread_mutex_unlock(&p->list_lock);
    CHECK(sem_post(&p->free_count) == 0);
}

static void *worker(void *arg)
{
    Pool *p = arg;
    for (int j = 0; j < JOBS_PER_THREAD; j++) {
        int ch = pool_acquire(p);
        int now = atomic_fetch_add(&p->inside, 1) + 1;
        int prev = atomic_load(&p->max_inside);
        while (now > prev && !atomic_compare_exchange_weak(&p->max_inside, &prev, now))
            ; /* CAS loop: record the maximum (lesson 02) */
        struct timespec t = {0, 2 * 1000000L}; /* "use the DMA channel" */
        nanosleep(&t, NULL);
        atomic_fetch_sub(&p->inside, 1);
        pool_release(p, ch);
    }
    return NULL;
}

int main(void)
{
    Pool p = {.inside = 0, .max_inside = 0};
    CHECK(sem_init(&p.free_count, 0, N_CHANNELS) == 0);
    CHECK(pthread_mutex_init(&p.list_lock, NULL) == 0);

    pthread_t t[N_THREADS];
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, worker, &p) == 0);
    for (int i = 0; i < N_THREADS; i++)
        CHECK(pthread_join(t[i], NULL) == 0);

    long total = 0;
    for (int i = 0; i < N_CHANNELS; i++) {
        printf("    channel %d used %ld times\n", i, p.uses[i]);
        total += p.uses[i];
    }
    int maxin = atomic_load(&p.max_inside);
    int v = -1;
    CHECK(sem_getvalue(&p.free_count, &v) == 0);
    printf("[1] %d threads x %d jobs on %d channels: max inside at once = %d "
           "(limit %d), %ld jobs, all channels back: value %d\n",
           N_THREADS, JOBS_PER_THREAD, N_CHANNELS, maxin, N_CHANNELS, total, v);
    CHECK(maxin >= 1 && maxin <= N_CHANNELS); /* THE property */
    CHECK(total == N_THREADS * JOBS_PER_THREAD);
    CHECK(v == N_CHANNELS);

    CHECK(pthread_mutex_destroy(&p.list_lock) == 0);
    CHECK(sem_destroy(&p.free_count) == 0);
    printf("02_resource_pool: all checks passed\n");
    return 0;
}
