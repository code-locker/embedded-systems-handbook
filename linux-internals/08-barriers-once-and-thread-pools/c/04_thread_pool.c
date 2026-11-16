/*
 * 04_thread_pool.c — a fixed-size thread pool in C: task queue protected by
 *                    a mutex, two condition variables, wait-for-all, and a
 *                    graceful shutdown that finishes every queued task
 *
 * Concept : Creating a thread per job costs ~10-50 us and unbounded threads
 *           thrash the 2 cores of the BeagleBone. A pool starts N workers
 *           ONCE; they sleep on a condition variable until a task is
 *           queued. The pool is a producer-consumer queue (lesson 04) plus
 *           lifecycle rules: who may submit when, how to wait until all
 *           work is done, and how to stop without losing tasks.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/04_thread_pool.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/04_thread_pool.c SAN=tsan
 * Read    : ../README.md  (section "Thread pool")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
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

/*
 *   submit() ---> [ task | task | task ] ---> worker 0  (sleeps on has_work)
 *                  head            tail  ---> worker 1
 *                    (mutex m)            ---> worker 2
 *
 *   has_work : signalled when a task is queued or on shutdown
 *   all_idle : signalled when queue empty AND no worker is running a task
 */
typedef void (*TaskFn)(void *arg);

typedef struct Task {
    TaskFn fn;
    void *arg;
    struct Task *next;
} Task;

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t has_work, all_idle;
    Task *head, *tail;  /* FIFO queue                 (under m) */
    int active;         /* workers running a task     (under m) */
    bool stopping;      /* no new tasks; drain & exit (under m) */
    int nthreads;
    pthread_t *threads;
} Pool;

static void *pool_worker(void *p)
{
    Pool *pool = p;
    CHECK(pthread_mutex_lock(&pool->m) == 0);
    for (;;) {
        while (pool->head == NULL && !pool->stopping)
            CHECK(pthread_cond_wait(&pool->has_work, &pool->m) == 0);
        if (pool->head == NULL && pool->stopping)
            break; /* graceful: only exit once the queue is EMPTY */
        Task *t = pool->head;
        pool->head = t->next;
        if (pool->head == NULL)
            pool->tail = NULL;
        pool->active++;
        CHECK(pthread_mutex_unlock(&pool->m) == 0);

        t->fn(t->arg); /* run WITHOUT the lock: tasks run in parallel */
        free(t);       /* ownership: the worker frees the Task node */

        CHECK(pthread_mutex_lock(&pool->m) == 0);
        pool->active--;
        if (pool->active == 0 && pool->head == NULL)
            CHECK(pthread_cond_broadcast(&pool->all_idle) == 0);
    }
    CHECK(pthread_mutex_unlock(&pool->m) == 0);
    return NULL;
}

static Pool *pool_create(int nthreads)
{
    Pool *p = calloc(1, sizeof *p);
    CHECK(p != NULL);
    CHECK(pthread_mutex_init(&p->m, NULL) == 0);
    CHECK(pthread_cond_init(&p->has_work, NULL) == 0);
    CHECK(pthread_cond_init(&p->all_idle, NULL) == 0);
    p->nthreads = nthreads;
    p->threads = calloc((size_t)nthreads, sizeof *p->threads);
    CHECK(p->threads != NULL);
    for (int i = 0; i < nthreads; i++)
        CHECK(pthread_create(&p->threads[i], NULL, pool_worker, p) == 0);
    return p;
}

/* O(1). Returns false after shutdown started (task NOT queued). */
static bool pool_submit(Pool *p, TaskFn fn, void *arg)
{
    Task *t = malloc(sizeof *t);
    CHECK(t != NULL);
    *t = (Task){fn, arg, NULL};
    CHECK(pthread_mutex_lock(&p->m) == 0);
    if (p->stopping) {
        CHECK(pthread_mutex_unlock(&p->m) == 0);
        free(t);
        return false;
    }
    if (p->tail)
        p->tail->next = t;
    else
        p->head = t;
    p->tail = t;
    CHECK(pthread_cond_signal(&p->has_work) == 0); /* one task: wake one */
    CHECK(pthread_mutex_unlock(&p->m) == 0);
    return true;
}

/* Block until the queue is empty and no task is running. */
static void pool_wait(Pool *p)
{
    CHECK(pthread_mutex_lock(&p->m) == 0);
    while (p->head != NULL || p->active > 0)
        CHECK(pthread_cond_wait(&p->all_idle, &p->m) == 0);
    CHECK(pthread_mutex_unlock(&p->m) == 0);
}

/* Graceful shutdown: refuse new tasks, let workers drain the queue, join. */
static void pool_destroy(Pool *p)
{
    CHECK(pthread_mutex_lock(&p->m) == 0);
    p->stopping = true;
    CHECK(pthread_cond_broadcast(&p->has_work) == 0); /* wake ALL sleepers */
    CHECK(pthread_mutex_unlock(&p->m) == 0);
    for (int i = 0; i < p->nthreads; i++)
        CHECK(pthread_join(p->threads[i], NULL) == 0);
    CHECK(p->head == NULL); /* drained */
    CHECK(pthread_cond_destroy(&p->has_work) == 0);
    CHECK(pthread_cond_destroy(&p->all_idle) == 0);
    CHECK(pthread_mutex_destroy(&p->m) == 0);
    free(p->threads);
    free(p);
}

/* ---- tasks --------------------------------------------------------------- */
typedef struct {
    int input;
    long output; /* written by the task, read by main after pool_wait() */
} Job;

static atomic_int g_ran;

static void square_task(void *arg)
{
    Job *j = arg;
    long acc = 0;
    for (int k = 0; k < 200; k++) /* a bit of "work" */
        acc += j->input;
    j->output = acc / 200 * j->input;
    atomic_fetch_add(&g_ran, 1);
}

static void count_task(void *arg)
{
    (void)arg;
    atomic_fetch_add(&g_ran, 1);
}

int main(void)
{
    enum { NJOBS = 2000 };
    static Job jobs[NJOBS];
    Pool *pool = pool_create(4);

    /* 1. Submit, wait for all, check every result. Reuse the pool twice:
     *    pool_wait() does not stop it. */
    for (int round = 0; round < 2; round++) {
        atomic_store(&g_ran, 0);
        for (int i = 0; i < NJOBS; i++) {
            jobs[i] = (Job){.input = i + round, .output = -1};
            CHECK(pool_submit(pool, square_task, &jobs[i]));
        }
        pool_wait(pool); /* happens-before: all outputs visible now */
        for (int i = 0; i < NJOBS; i++)
            CHECK(jobs[i].output == (long)(i + round) * (i + round));
        printf("round %d: %d tasks ran on 4 workers, all results correct\n",
               round, atomic_load(&g_ran));
        CHECK(atomic_load(&g_ran) == NJOBS);
    }

    /* 2. Graceful shutdown: queue many tasks and destroy IMMEDIATELY.
     *    Every queued task must still run; later submits are refused. */
    atomic_store(&g_ran, 0);
    for (int i = 0; i < 5000; i++)
        CHECK(pool_submit(pool, count_task, NULL));
    pool_destroy(pool);
    printf("shutdown with 5000 queued tasks: %d ran before the workers "
           "exited\n", atomic_load(&g_ran));
    CHECK(atomic_load(&g_ran) == 5000);

    /* 3. Submitting after shutdown started is refused (checked on a fresh
     *    pool, since `pool` is gone). */
    Pool *p2 = pool_create(2);
    CHECK(pthread_mutex_lock(&p2->m) == 0);
    p2->stopping = true; /* simulate "shutdown in progress" */
    CHECK(pthread_cond_broadcast(&p2->has_work) == 0);
    CHECK(pthread_mutex_unlock(&p2->m) == 0);
    CHECK(!pool_submit(p2, count_task, NULL));
    pool_destroy(p2);
    printf("submit during shutdown -> refused\n");

    printf("04_thread_pool: all checks passed\n");
    return 0;
}
