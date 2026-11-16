/*
 * 04_mpmc_bounded_queue.c — multi-producer multi-consumer bounded queue with
 *                           one mutex, two condition variables and close()
 *
 * Concept : The workhorse of threaded programs (GStreamer's queue element,
 *           a logger thread, a thread pool's job queue):
 *             push() sleeps while FULL   on cv "not_full"
 *             pop()  sleeps while EMPTY  on cv "not_empty"
 *           Two cvs, because producers and consumers wait for DIFFERENT
 *           predicates: with one cv, a signal meant for a consumer could
 *           wake a producer, which re-sleeps, and the consumer never hears
 *           it. close() ends the stream cleanly:
 *             push after close  -> refused (returns false)
 *             pop  after close  -> drains what is left, then returns false
 *           dsa/19-system-design-structures/c/03 has a single-producer ring
 *           version; this one is MPMC and tests shutdown while blocked.
 * Build   : make run P=linux-internals/04-condition-variables/c/04_mpmc_bounded_queue.c
 *           make run P=linux-internals/04-condition-variables/c/04_mpmc_bounded_queue.c SAN=tsan
 * Read    : ../README.md  (section 3.7 "Bounded buffer")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/*
 *            producers                               consumers
 *   P0 --push--+                                 +--pop--> C0
 *   P1 --push--+--> [ m | buf[cap] head tail n ] +--pop--> C1
 *   P2 --push--+      not_full  ^      not_empty  +--pop--> C2
 *                   (P sleeps here)  (C sleeps here)
 *
 *   push: lock; while (n == cap && !closed) wait(not_full)
 *         if closed: unlock, return false
 *         buf[tail] = x; tail = (tail+1) % cap; n++
 *         signal(not_empty); unlock            <- ONE consumer can use it
 *   pop : lock; while (n == 0 && !closed) wait(not_empty)
 *         if n == 0 (so closed): unlock, return false
 *         x = buf[head]; head = (head+1) % cap; n--
 *         signal(not_full); unlock             <- ONE producer can use it
 *   close: lock; closed = 1; broadcast(not_full); broadcast(not_empty); unlock
 *                                              <- EVERYONE must see it
 */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t not_full, not_empty;
    uint32_t *buf; /* owned by the queue: malloc in init, free in destroy */
    size_t cap, head, tail, n;
    bool closed;
    int blocked_producers; /* for the test: how many sleep in push() */
} Queue;

static void q_init(Queue *q, size_t cap)
{
    memset(q, 0, sizeof *q);
    q->buf = malloc(cap * sizeof *q->buf);
    CHECK(q->buf != NULL);
    q->cap = cap;
    CHECK(pthread_mutex_init(&q->m, NULL) == 0);
    CHECK(pthread_cond_init(&q->not_full, NULL) == 0);
    CHECK(pthread_cond_init(&q->not_empty, NULL) == 0);
}

static void q_destroy(Queue *q)
{
    CHECK(pthread_cond_destroy(&q->not_full) == 0);
    CHECK(pthread_cond_destroy(&q->not_empty) == 0);
    CHECK(pthread_mutex_destroy(&q->m) == 0);
    free(q->buf);
}

/* O(1). Blocks while full. false = queue closed, item NOT stored. */
static bool q_push(Queue *q, uint32_t x)
{
    pthread_mutex_lock(&q->m);
    while (q->n == q->cap && !q->closed) {
        q->blocked_producers++;
        pthread_cond_wait(&q->not_full, &q->m);
        q->blocked_producers--;
    }
    if (q->closed) {
        pthread_mutex_unlock(&q->m);
        return false;
    }
    q->buf[q->tail] = x;
    q->tail = (q->tail + 1) % q->cap;
    q->n++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->m);
    return true;
}

/* O(1). Blocks while empty. false = closed AND drained. */
static bool q_pop(Queue *q, uint32_t *out)
{
    pthread_mutex_lock(&q->m);
    while (q->n == 0 && !q->closed)
        pthread_cond_wait(&q->not_empty, &q->m);
    if (q->n == 0) { /* only possible when closed */
        pthread_mutex_unlock(&q->m);
        return false;
    }
    *out = q->buf[q->head];
    q->head = (q->head + 1) % q->cap;
    q->n--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return true;
}

static void q_close(Queue *q)
{
    pthread_mutex_lock(&q->m);
    q->closed = true;
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->m);
}

/* ---------------------------------------------------------------------- */
#define N_PROD 3
#define N_CONS 3
#define PER_PROD 3000
#define TOTAL (N_PROD * PER_PROD)

typedef struct {
    Queue *q;
    int id;
    uint64_t sum;
    uint32_t count;
    uint8_t *seen; /* shared bitmap, index = item; each slot written by
                      exactly one consumer (the one that popped it) */
} Arg;

static void *producer(void *p)
{
    Arg *a = p;
    for (uint32_t i = 0; i < PER_PROD; i++) {
        uint32_t item = (uint32_t)a->id * PER_PROD + i; /* unique 0..TOTAL-1 */
        CHECK(q_push(a->q, item));
    }
    return NULL;
}

static void *consumer(void *p)
{
    Arg *a = p;
    uint32_t x;
    while (q_pop(a->q, &x)) {
        CHECK(x < TOTAL);
        a->seen[x]++;
        a->sum += x;
        a->count++;
    }
    return NULL;
}

static void part1_mpmc(void)
{
    Queue q;
    q_init(&q, 8); /* small: producers block often, which is the point */
    uint8_t *seen = calloc(TOTAL, 1);
    CHECK(seen != NULL);
    pthread_t tp[N_PROD], tc[N_CONS];
    Arg ap[N_PROD], ac[N_CONS];
    for (int i = 0; i < N_CONS; i++) {
        ac[i] = (Arg){&q, i, 0, 0, seen};
        CHECK(pthread_create(&tc[i], NULL, consumer, &ac[i]) == 0);
    }
    for (int i = 0; i < N_PROD; i++) {
        ap[i] = (Arg){&q, i, 0, 0, seen};
        CHECK(pthread_create(&tp[i], NULL, producer, &ap[i]) == 0);
    }
    for (int i = 0; i < N_PROD; i++)
        CHECK(pthread_join(tp[i], NULL) == 0);
    q_close(&q); /* all items are in; consumers drain the rest and exit */
    uint64_t sum = 0;
    uint32_t count = 0;
    for (int i = 0; i < N_CONS; i++) {
        CHECK(pthread_join(tc[i], NULL) == 0);
        printf("    consumer %d took %u items\n", i, ac[i].count);
        sum += ac[i].sum;
        count += ac[i].count;
    }
    uint64_t want = (uint64_t)TOTAL * (TOTAL - 1) / 2;
    printf("[1] %d producers x %d items through an 8-slot queue: %u popped, "
           "sum %llu (expected %llu)\n", N_PROD, PER_PROD, count,
           (unsigned long long)sum, (unsigned long long)want);
    CHECK(count == TOTAL);
    CHECK(sum == want);
    for (uint32_t i = 0; i < TOTAL; i++)
        CHECK(seen[i] == 1); /* every item exactly once: none lost, none twice */
    free(seen);
    q_destroy(&q);
}

/* ---------------------------------------------------------------------- */
typedef struct {
    Queue *q;
    int pushed;
    bool last_rc;
} Blocked;

static void *pusher(void *p)
{
    Blocked *b = p;
    for (uint32_t i = 0; i < 5; i++) {
        b->last_rc = q_push(b->q, i);
        if (!b->last_rc)
            break;
        b->pushed++;
    }
    return NULL;
}

static void part2_close_wakes_blocked(void)
{
    Queue q;
    q_init(&q, 2);
    Blocked b = {&q, 0, true};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, pusher, &b) == 0);

    /* Wait (max 5 s) until the producer is asleep on a full queue. Polling
     * with a short sleep is fine in a test. */
    int blocked = 0;
    for (int i = 0; i < 5000 && !blocked; i++) {
        pthread_mutex_lock(&q.m);
        blocked = q.blocked_producers == 1 && q.n == 2;
        pthread_mutex_unlock(&q.m);
        if (!blocked) {
            struct timespec ms = {0, 1000000L};
            nanosleep(&ms, NULL);
        }
    }
    CHECK(blocked);

    q_close(&q); /* must wake the sleeping producer */
    CHECK(pthread_join(t, NULL) == 0);
    printf("[2] producer blocked on a full 2-slot queue; close() woke it, "
           "push returned false after %d items\n", b.pushed);
    CHECK(b.pushed == 2 && !b.last_rc);

    uint32_t x;
    CHECK(q_pop(&q, &x) && x == 0); /* drain still works after close */
    CHECK(q_pop(&q, &x) && x == 1);
    CHECK(!q_pop(&q, &x));          /* closed and empty: end of stream */
    CHECK(!q_push(&q, 99));
    printf("[3] after close: the 2 queued items drained, then pop -> false, "
           "push -> false\n");
    q_destroy(&q);
}

int main(void)
{
    part1_mpmc();
    part2_close_wakes_blocked();
    printf("04_mpmc_bounded_queue: all checks passed\n");
    return 0;
}
