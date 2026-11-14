/*
 * 03_ring_buffer.c — queue on a circular buffer, three ways to tell full
 *                    from empty
 *
 * Concept : A ring buffer is a fixed array whose end wraps to its start.
 *           `head` is where we read, `tail` is where we write. The trap:
 *           when head == tail, is the buffer empty or full? Three classic
 *           answers are implemented:
 *             A. keep one slot always empty   (capacity N-1)
 *             B. keep an explicit count       (capacity N)
 *             C. free-running indices, size a power of two, mask on access
 *                (capacity N; what Linux kfifo and most DMA rings do)
 * Build   : make run P=dsa/07-stacks-and-queues/c/03_ring_buffer.c
 * Read    : ../README.md  (section "Ring buffer")
 */
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

#define N 8

/*
 *  The picture to remember (N = 8):
 *
 *     index:  0   1   2   3   4   5   6   7
 *           +---+---+---+---+---+---+---+---+
 *           |   |   | a | b | c | d |   |   |
 *           +---+---+---+---+---+---+---+---+
 *                     ^               ^
 *                    head            tail
 *                  (next read)    (next write)
 *
 *   push: buf[tail] = x; tail = (tail + 1) % N
 *   pop : x = buf[head]; head = (head + 1) % N
 *
 *  After wrapping, the data can be split in two pieces:
 *
 *           +---+---+---+---+---+---+---+---+
 *           | g | h |   |   |   | d | e | f |
 *           +---+---+---+---+---+---+---+---+
 *                     ^           ^
 *                    tail        head
 *
 *  THE AMBIGUITY: fill it completely and tail catches up to head:
 *  head == tail. But head == tail is also the empty state. Same picture,
 *  two meanings — so we need one extra bit of information.
 */

/* ------------------------------------------------------------------------ */
/* A. Waste one slot: full when (tail + 1) % N == head                      */
/* ------------------------------------------------------------------------ */
/* Pro: no shared counter, so producer only writes tail and consumer only
 * writes head — this is what makes the lock-free SPSC version possible.
 * Con: capacity is N-1. */
typedef struct {
    int buf[N];
    size_t head, tail;
} RingA;

static bool a_empty(const RingA *r) { return r->head == r->tail; }
static bool a_full(const RingA *r) { return (r->tail + 1) % N == r->head; }

static bool a_push(RingA *r, int v) /* O(1) */
{
    if (a_full(r))
        return false;
    r->buf[r->tail] = v;
    r->tail = (r->tail + 1) % N;
    return true;
}

static bool a_pop(RingA *r, int *out) /* O(1) */
{
    if (a_empty(r))
        return false;
    *out = r->buf[r->head];
    r->head = (r->head + 1) % N;
    return true;
}

/* ------------------------------------------------------------------------ */
/* B. Explicit count: empty when count == 0, full when count == N           */
/* ------------------------------------------------------------------------ */
/* Pro: uses all N slots, easy to read. Con: `count` is written by BOTH
 * producer and consumer, so with two threads (or ISR + main loop) it needs
 * a lock or atomic read-modify-write. Common in single-threaded code. */
typedef struct {
    int buf[N];
    size_t head, count;
} RingB;

static bool b_push(RingB *r, int v) /* O(1) */
{
    if (r->count == N)
        return false;
    r->buf[(r->head + r->count) % N] = v; /* tail is derived, not stored */
    r->count++;
    return true;
}

static bool b_pop(RingB *r, int *out) /* O(1) */
{
    if (r->count == 0)
        return false;
    *out = r->buf[r->head];
    r->head = (r->head + 1) % N;
    r->count--;
    return true;
}

/* ------------------------------------------------------------------------ */
/* C. Free-running indices + power-of-two mask (Linux kfifo style)          */
/* ------------------------------------------------------------------------ */
/*
 * head and tail are NOT wrapped; they just count up forever (unsigned, so
 * overflow wraps mod 2^32, which is well-defined in C). The slot is
 * index & (N-1). Then:
 *
 *     used  = tail - head        (unsigned subtraction survives wrap-around)
 *     empty : used == 0
 *     full  : used == N
 *
 * The extra "bit" that resolves the ambiguity is hidden in the upper bits
 * of the counters. Requires N to be a power of two so that
 *  (a) i & (N-1) == i % N  (a mask is one AND; % is a division), and
 *  (b) 2^32 is a multiple of N, so slots stay consistent across wrap.
 */
_Static_assert((N & (N - 1)) == 0, "N must be a power of two for RingC");

typedef struct {
    int buf[N];
    uint32_t head, tail; /* free-running */
} RingC;

static uint32_t c_used(const RingC *r) { return r->tail - r->head; }

static bool c_push(RingC *r, int v) /* O(1) */
{
    if (c_used(r) == N)
        return false;
    r->buf[r->tail & (N - 1)] = v;
    r->tail++;
    return true;
}

static bool c_pop(RingC *r, int *out) /* O(1) */
{
    if (c_used(r) == 0)
        return false;
    *out = r->buf[r->head & (N - 1)];
    r->head++;
    return true;
}

/* ------------------------------------------------------------------------ */
/* An embedded-style use: a UART RX ring that drops on overflow             */
/* ------------------------------------------------------------------------ */
/* In a driver, the RX interrupt calls push() for each received byte and the
 * reader calls pop(). If the reader is too slow the ring fills; the ISR
 * cannot block, so it DROPS the byte and counts an overrun (what
 * /proc/tty/driver/serial reports as "oe"). */
static void uart_demo(void)
{
    RingC rx = {.head = 0, .tail = 0};
    const char *wire = "HELLO-UART-RING!"; /* 16 bytes arrive, ring holds 8 */
    unsigned overruns = 0;
    for (const char *p = wire; *p; p++)
        if (!c_push(&rx, *p))
            overruns++;
    char got[N + 1];
    int c;
    size_t i = 0;
    while (c_pop(&rx, &c))
        got[i++] = (char)c;
    got[i] = '\0';
    printf("uart: received \"%s\", overruns %u\n", got, overruns);
    CHECK(i == N && overruns == 8 && got[0] == 'H' && got[7] == 'A');
}

int main(void)
{
    int v;

    /* A: only N-1 elements fit */
    RingA a = {.head = 0, .tail = 0};
    int pushed = 0;
    while (a_push(&a, pushed))
        pushed++;
    printf("A (waste one slot): capacity %d of %d slots\n", pushed, N);
    CHECK(pushed == N - 1);
    for (int i = 0; i < pushed; i++)
        CHECK(a_pop(&a, &v) && v == i); /* FIFO order */
    CHECK(a_empty(&a));

    /* B: all N fit */
    RingB b = {.head = 0, .count = 0};
    pushed = 0;
    while (b_push(&b, pushed))
        pushed++;
    printf("B (count)         : capacity %d of %d slots\n", pushed, N);
    CHECK(pushed == N);

    /* Wrap-around test for B: pop 5, push 5 more — writes cross index 7->0 */
    for (int i = 0; i < 5; i++)
        CHECK(b_pop(&b, &v) && v == i);
    for (int i = 100; i < 105; i++)
        CHECK(b_push(&b, i));
    int expect[] = {5, 6, 7, 100, 101, 102, 103, 104};
    for (int i = 0; i < N; i++)
        CHECK(b_pop(&b, &v) && v == expect[i]);
    CHECK(!b_pop(&b, &v));

    /* C: all N fit, and survives counter overflow. Start the counters just
     * below UINT32_MAX to prove tail - head still works when tail wraps. */
    RingC c = {.head = UINT32_MAX - 3, .tail = UINT32_MAX - 3};
    pushed = 0;
    while (c_push(&c, pushed))
        pushed++;
    printf("C (free-running)  : capacity %d of %d slots, tail wrapped to %u\n",
           pushed, N, (unsigned)c.tail);
    CHECK(pushed == N);
    CHECK(c.tail < c.head); /* numerically wrapped... */
    CHECK(c_used(&c) == N); /* ...but the subtraction is still right */
    for (int i = 0; i < N; i++)
        CHECK(c_pop(&c, &v) && v == i);
    CHECK(c_used(&c) == 0);

    uart_demo();

    printf("03_ring_buffer: all checks passed\n");
    return 0;
}
