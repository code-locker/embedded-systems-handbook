/*
 * 03_producer_consumer.c — the classic bounded buffer with two counting
 *                          semaphores and a mutex (Dijkstra's solution)
 *
 * Concept : empty_slots starts at N, full_slots starts at 0.
 *             producer: P(empty_slots); lock; put; unlock; V(full_slots)
 *             consumer: P(full_slots);  lock; get; unlock; V(empty_slots)
 *           The semaphores do the COUNTING and the sleeping; the mutex only
 *           protects the indices, for a few instructions. Compare with the
 *           condition-variable version in
 *           ../../04-condition-variables/c/04_mpmc_bounded_queue.c: no
 *           predicate loop, no lost wake-ups (a post is remembered), but
 *           also no easy "close": we stop consumers with poison pills.
 * Build   : make run P=linux-internals/05-semaphores/c/03_producer_consumer.c
 *           make run P=linux-internals/05-semaphores/c/03_producer_consumer.c SAN=tsan
 * Read    : ../README.md  (section 3.5 "Producer/consumer")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
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

#define CAP 8
#define N_PROD 3
#define N_CONS 2
#define PER_PROD 3000
#define POISON UINT32_MAX

/*
 *   empty_slots: 8 -> producers may run ahead by up to 8 items
 *   full_slots : 0 -> consumers sleep until something is there
 *
 *   ORDER MATTERS: P(sem) first, THEN lock. Lock first and then sleep in
 *   P() while holding the lock, and the other side can never get the lock
 *   to V() you -> deadlock.
 *       WRONG:  lock(m); P(empty_slots); ...      RIGHT: P(empty_slots); lock(m); ...
 */
typedef struct {
    sem_t empty_slots, full_slots;
    pthread_mutex_t m;     /* protects buf/head/tail only */
    uint32_t buf[CAP];
    unsigned head, tail;
} Buffer;

static void P(sem_t *s)
{
    while (sem_wait(s) == -1)
        CHECK(errno == EINTR);
}
static void V(sem_t *s) { CHECK(sem_post(s) == 0); }

static void put(Buffer *b, uint32_t x)
{
    P(&b->empty_slots);
    pthread_mutex_lock(&b->m);
    b->buf[b->tail] = x;
    b->tail = (b->tail + 1) % CAP;
    pthread_mutex_unlock(&b->m);
    V(&b->full_slots);
}

static uint32_t get(Buffer *b)
{
    P(&b->full_slots);
    pthread_mutex_lock(&b->m);
    uint32_t x = b->buf[b->head];
    b->head = (b->head + 1) % CAP;
    pthread_mutex_unlock(&b->m);
    V(&b->empty_slots);
    return x;
}

typedef struct {
    Buffer *b;
    int id;
    uint64_t sum;
    uint32_t count;
} Arg;

static void *producer(void *p)
{
    Arg *a = p;
    for (uint32_t i = 1; i <= PER_PROD; i++)
        put(a->b, (uint32_t)a->id * 100000u + i);
    return NULL;
}

static void *consumer(void *p)
{
    Arg *a = p;
    for (;;) {
        uint32_t x = get(a->b);
        if (x == POISON) /* one pill per consumer ends it */
            break;
        a->sum += x;
        a->count++;
    }
    return NULL;
}

int main(void)
{
    Buffer b = {.head = 0, .tail = 0};
    CHECK(sem_init(&b.empty_slots, 0, CAP) == 0);
    CHECK(sem_init(&b.full_slots, 0, 0) == 0);
    CHECK(pthread_mutex_init(&b.m, NULL) == 0);

    pthread_t tp[N_PROD], tc[N_CONS];
    Arg ap[N_PROD], ac[N_CONS];
    for (int i = 0; i < N_CONS; i++) {
        ac[i] = (Arg){&b, i, 0, 0};
        CHECK(pthread_create(&tc[i], NULL, consumer, &ac[i]) == 0);
    }
    for (int i = 0; i < N_PROD; i++) {
        ap[i] = (Arg){&b, i, 0, 0};
        CHECK(pthread_create(&tp[i], NULL, producer, &ap[i]) == 0);
    }
    for (int i = 0; i < N_PROD; i++)
        CHECK(pthread_join(tp[i], NULL) == 0);
    /* All real items are in. Poison pills come after them in FIFO order,
     * so every real item is consumed before a consumer stops. */
    for (int i = 0; i < N_CONS; i++)
        put(&b, POISON);

    uint64_t sum = 0, want = 0;
    uint32_t count = 0;
    for (int i = 0; i < N_CONS; i++) {
        CHECK(pthread_join(tc[i], NULL) == 0);
        printf("    consumer %d: %u items\n", i, ac[i].count);
        sum += ac[i].sum;
        count += ac[i].count;
    }
    for (uint32_t p = 0; p < N_PROD; p++)
        for (uint32_t i = 1; i <= PER_PROD; i++)
            want += p * 100000u + i;
    int e = -1, f = -1;
    CHECK(sem_getvalue(&b.empty_slots, &e) == 0);
    CHECK(sem_getvalue(&b.full_slots, &f) == 0);
    printf("[1] %d producers, %d consumers, %d slots: %u items, sum %llu "
           "(expected %llu); final empty=%d full=%d\n", N_PROD, N_CONS, CAP,
           count, (unsigned long long)sum, (unsigned long long)want, e, f);
    CHECK(count == N_PROD * PER_PROD);
    CHECK(sum == want);
    CHECK(e == CAP && f == 0); /* invariant: empty + full + in-flight == CAP */

    CHECK(pthread_mutex_destroy(&b.m) == 0);
    CHECK(sem_destroy(&b.empty_slots) == 0);
    CHECK(sem_destroy(&b.full_slots) == 0);
    printf("03_producer_consumer: all checks passed\n");
    return 0;
}
