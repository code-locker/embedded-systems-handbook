/*
 * 03_ring_buffer_producer_consumer.c — bounded ring buffer with mutex + two condvars
 *
 * Concept : A fixed array used circularly (head/tail indices wrap around)
 *           gives a FIFO with zero allocation after init. Producers block on
 *           "not_full", consumers block on "not_empty". This is the shape of
 *           every driver RX/TX queue, GStreamer's queue element, audio
 *           buffers, and logging pipelines.
 *           Part 2 shows the lock-free single-producer/single-consumer
 *           variant with C11 atomics (like the kernel's kfifo).
 * Build   : make run P=dsa/19-system-design-structures/c/03_ring_buffer_producer_consumer.c
 * Read    : ../README.md  (section "Ring buffer")
 */
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 *  capacity 8, count 5
 *
 *  index:  0   1   2   3   4   5   6   7
 *        +---+---+---+---+---+---+---+---+
 *        | f | g |   |   |   | c | d | e |     head=5 (next to read)
 *        +---+---+---+---+---+---+---+---+     tail=2 (next to write)
 *                  ^tail       ^head
 *  push: buf[tail] = x; tail = (tail+1) % cap; count++
 *  pop : x = buf[head]; head = (head+1) % cap; count--
 *  We keep an explicit count so "full" (count==cap) and "empty" (count==0)
 *  are not confused — with only head/tail, head==tail means both.
 */
typedef struct {
    int *buf;
    size_t cap, head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty;
    int closed;                    /* producers finished: wake & drain consumers */
} Ring;

static void ring_init(Ring *r, size_t cap)
{
    r->buf = malloc(cap * sizeof *r->buf);
    assert(r->buf);
    r->cap = cap;
    r->head = r->tail = r->count = 0;
    r->closed = 0;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->not_full, NULL);
    pthread_cond_init(&r->not_empty, NULL);
}

static void ring_destroy(Ring *r)
{
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->not_full);
    pthread_cond_destroy(&r->not_empty);
    free(r->buf);
}

/* Blocks while full. */
static void ring_push(Ring *r, int x)
{
    pthread_mutex_lock(&r->lock);
    /* WHILE, not if: wake-ups can be spurious, and another producer may
       have filled the slot between the signal and us re-acquiring the lock. */
    while (r->count == r->cap)
        pthread_cond_wait(&r->not_full, &r->lock);
    r->buf[r->tail] = x;
    r->tail = (r->tail + 1) % r->cap;
    r->count++;
    pthread_cond_signal(&r->not_empty);
    pthread_mutex_unlock(&r->lock);
}

/* Blocks while empty; returns 0 when closed and drained. */
static int ring_pop(Ring *r, int *out)
{
    pthread_mutex_lock(&r->lock);
    while (r->count == 0 && !r->closed)
        pthread_cond_wait(&r->not_empty, &r->lock);
    if (r->count == 0) {           /* closed and empty: no more data ever */
        pthread_mutex_unlock(&r->lock);
        return 0;
    }
    *out = r->buf[r->head];
    r->head = (r->head + 1) % r->cap;
    r->count--;
    pthread_cond_signal(&r->not_full);
    pthread_mutex_unlock(&r->lock);
    return 1;
}

static void ring_close(Ring *r)
{
    pthread_mutex_lock(&r->lock);
    r->closed = 1;
    pthread_cond_broadcast(&r->not_empty);   /* wake ALL consumers so they can exit */
    pthread_mutex_unlock(&r->lock);
}

/* ---- multi-producer / multi-consumer test -------------------------------- */
enum { PRODUCERS = 4, CONSUMERS = 3, PER_PRODUCER = 20000 };

typedef struct { Ring *r; int id; } ProdArg;
typedef struct { Ring *r; long long sum; long count; unsigned char *seen; } ConsArg;

static void *producer(void *p)
{
    ProdArg *a = p;
    for (int i = 0; i < PER_PRODUCER; i++)
        ring_push(a->r, a->id * PER_PRODUCER + i);   /* globally unique values */
    return NULL;
}

static void *consumer(void *p)
{
    ConsArg *a = p;
    int x;
    while (ring_pop(a->r, &x)) {
        a->sum += x;
        a->count++;
        a->seen[x]++;              /* each consumer has its own array: no race */
    }
    return NULL;
}

/* ---- lock-free SPSC ring (power-of-two size, free-running indices) ------- */
/*
 * Only ONE producer writes `tail`, only ONE consumer writes `head`.
 * Indices grow forever (unsigned wrap is well defined); slot = idx & (N-1).
 * full  <=> tail - head == N        empty <=> tail == head
 * release on store / acquire on load makes the data write visible before the
 * index that publishes it — the same contract as the kernel's smp_store_release.
 */
#define SPSC_N 64u
typedef struct {
    int buf[SPSC_N];
    _Atomic unsigned head, tail;
} Spsc;

static int spsc_push(Spsc *q, int x)
{
    unsigned t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&q->head, memory_order_acquire);
    if (t - h == SPSC_N) return 0;           /* full */
    q->buf[t & (SPSC_N - 1)] = x;
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return 1;
}

static int spsc_pop(Spsc *q, int *out)
{
    unsigned h = atomic_load_explicit(&q->head, memory_order_relaxed);
    unsigned t = atomic_load_explicit(&q->tail, memory_order_acquire);
    if (t == h) return 0;                    /* empty */
    *out = q->buf[h & (SPSC_N - 1)];
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
    return 1;
}

enum { SPSC_ITEMS = 200000 };
static void *spsc_producer(void *p)
{
    Spsc *q = p;
    for (int i = 0; i < SPSC_ITEMS; )
        if (spsc_push(q, i)) i++;            /* spin when full (demo only) */
    return NULL;
}

int main(void)
{
    printf("== single-thread wrap-around check (cap 4) ==\n");
    Ring r;
    ring_init(&r, 4);
    int x;
    for (int round = 0; round < 10; round++) {   /* forces head/tail to wrap */
        ring_push(&r, round * 10 + 1);
        ring_push(&r, round * 10 + 2);
        ring_push(&r, round * 10 + 3);
        assert(ring_pop(&r, &x) && x == round * 10 + 1);
        assert(ring_pop(&r, &x) && x == round * 10 + 2);
        assert(ring_pop(&r, &x) && x == round * 10 + 3);
    }
    printf("FIFO order kept across 10 wrap-arounds, head=%zu tail=%zu\n", r.head, r.tail);
    ring_destroy(&r);

    printf("\n== %d producers, %d consumers, ring cap 16 ==\n", PRODUCERS, CONSUMERS);
    ring_init(&r, 16);
    pthread_t pt[PRODUCERS], ct[CONSUMERS];
    ProdArg pa[PRODUCERS];
    ConsArg ca[CONSUMERS];
    const int total = PRODUCERS * PER_PRODUCER;
    for (int i = 0; i < CONSUMERS; i++) {
        ca[i] = (ConsArg){ &r, 0, 0, calloc((size_t)total, 1) };
        assert(ca[i].seen);
        pthread_create(&ct[i], NULL, consumer, &ca[i]);
    }
    for (int i = 0; i < PRODUCERS; i++) {
        pa[i] = (ProdArg){ &r, i };
        pthread_create(&pt[i], NULL, producer, &pa[i]);
    }
    for (int i = 0; i < PRODUCERS; i++) pthread_join(pt[i], NULL);
    ring_close(&r);                        /* only after ALL producers are done */
    for (int i = 0; i < CONSUMERS; i++) pthread_join(ct[i], NULL);

    long long sum = 0;
    long count = 0;
    for (int i = 0; i < CONSUMERS; i++) {
        printf("consumer %d took %ld items\n", i, ca[i].count);
        sum += ca[i].sum;
        count += ca[i].count;
    }
    long long expect = (long long)total * (total - 1) / 2;
    assert(count == total);
    assert(sum == expect);
    for (int v = 0; v < total; v++) {      /* every value delivered exactly once */
        int n = 0;
        for (int i = 0; i < CONSUMERS; i++) n += ca[i].seen[v];
        assert(n == 1);
    }
    for (int i = 0; i < CONSUMERS; i++) free(ca[i].seen);
    ring_destroy(&r);
    printf("all %d items delivered exactly once, sum=%lld\n", total, sum);

    printf("\n== lock-free SPSC ring, %d items ==\n", SPSC_ITEMS);
    static Spsc q;                         /* zero-initialised atomics */
    pthread_t sp;
    pthread_create(&sp, NULL, spsc_producer, &q);
    for (int expect_v = 0; expect_v < SPSC_ITEMS; ) {
        int got;
        if (spsc_pop(&q, &got)) {
            assert(got == expect_v);       /* SPSC preserves strict order */
            expect_v++;
        }
    }
    pthread_join(sp, NULL);
    printf("received 0..%d in order\n", SPSC_ITEMS - 1);
    printf("OK\n");
    return 0;
}
