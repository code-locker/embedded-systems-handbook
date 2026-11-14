/*
 * 06_timer_queue.c — an event-loop timer queue: min-heap keyed by expiry time,
 *                    with O(log n) cancel and reschedule
 *
 * Concept : Every event loop (libevent, libuv, systemd's sd-event, GLib's main
 *           loop) asks one question each iteration: "when does the NEXT timer
 *           fire?" so it knows how long to sleep in poll()/epoll_wait(). A
 *           min-heap answers that in O(1). To cancel a timer in O(log n) each
 *           timer remembers its own index in the heap array (libevent calls it
 *           min_heap_idx), updated on every swap. Simulated clock: no sleeping.
 * Build   : make run P=dsa/10-heaps-priority-queues/c/06_timer_queue.c
 * Read    : ../README.md  (section "Timer queues and schedulers")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Timer Timer;
typedef void (*timer_cb)(Timer *t, uint64_t now, void *user);

struct Timer {
    uint64_t  expires;    /* absolute time in "ns" of the simulated clock */
    uint64_t  seq;        /* insertion order: equal expiries fire FIFO */
    uint64_t  period;     /* 0 = one-shot, else re-arm after firing */
    size_t    heap_idx;   /* position in heap array, SIZE_MAX when not queued */
    timer_cb  cb;
    void     *user;
    int       id;
};

typedef struct {
    Timer  **h;           /* array of pointers: heap does NOT own the timers */
    size_t   len, cap;
    uint64_t next_seq;
} TimerQueue;

static bool earlier(const Timer *a, const Timer *b)
{
    return a->expires != b->expires ? a->expires < b->expires : a->seq < b->seq;
}

/* Put t at slot i AND tell t where it lives. Every heap move goes through here,
 * that is what keeps heap_idx correct. */
static void place(TimerQueue *q, size_t i, Timer *t) { q->h[i] = t; t->heap_idx = i; }

static void sift_up(TimerQueue *q, size_t i)              /* O(log n) */
{
    Timer *t = q->h[i];
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!earlier(t, q->h[p])) break;
        place(q, i, q->h[p]);      /* parent moves down into the hole */
        i = p;
    }
    place(q, i, t);
}

static void sift_down(TimerQueue *q, size_t i)            /* O(log n) */
{
    Timer *t = q->h[i];
    for (;;) {
        size_t c = 2 * i + 1;
        if (c >= q->len) break;
        if (c + 1 < q->len && earlier(q->h[c + 1], q->h[c])) c++;
        if (!earlier(q->h[c], t)) break;
        place(q, i, q->h[c]);
        i = c;
    }
    place(q, i, t);
}

static void tq_init(TimerQueue *q) { q->h = NULL; q->len = q->cap = 0; q->next_seq = 0; }
static void tq_free(TimerQueue *q) { free(q->h); tq_init(q); }

/* O(log n): arm a timer that is not currently queued. */
static void tq_add(TimerQueue *q, Timer *t, uint64_t expires)
{
    assert(t->heap_idx == SIZE_MAX);
    if (q->len == q->cap) {
        size_t nc = q->cap ? 2 * q->cap : 16;
        Timer **nh = realloc(q->h, nc * sizeof *nh);
        assert(nh);
        q->h = nh; q->cap = nc;
    }
    t->expires = expires;
    t->seq = q->next_seq++;
    q->h[q->len] = t;
    t->heap_idx = q->len++;
    sift_up(q, t->heap_idx);
}

/*
 * tq_cancel — O(log n). Remove from the MIDDLE of the heap:
 * move the LAST element into the hole, then it may need to go UP or DOWN
 * (it came from another subtree, so it can be smaller than its new parent).
 *
 *          10                     10                    10
 *        /    \    cancel 30    /    \   55 > 10, so  /    \
 *      30      50    ---->    55      50 sift DOWN: 35      50
 *     /  \    /              /  \        swap w/35  /  \
 *    35  60  55             35  60                 55  60
 *    (55 is the last leaf: it fills the hole)      .
 *
 *  The UP case: cancel a node in the right subtree whose replacement (from
 *  the left subtree) is smaller than the new parent; then sift_up instead.
 */
static void tq_cancel(TimerQueue *q, Timer *t)
{
    size_t i = t->heap_idx;
    if (i == SIZE_MAX) return;              /* not armed: cancel is idempotent */
    t->heap_idx = SIZE_MAX;
    Timer *last = q->h[--q->len];
    if (i == q->len) return;                /* it WAS the last slot */
    place(q, i, last);
    if (i > 0 && earlier(last, q->h[(i - 1) / 2])) sift_up(q, i);
    else sift_down(q, i);
}

/* O(1): how long may the event loop sleep? */
static bool tq_next(const TimerQueue *q, uint64_t *when)
{
    if (!q->len) return false;
    *when = q->h[0]->expires;
    return true;
}

/* Fire every timer with expires <= now. O(m log n) for m expired timers. */
static int tq_run(TimerQueue *q, uint64_t now)
{
    int fired = 0;
    while (q->len && q->h[0]->expires <= now) {
        Timer *t = q->h[0];
        tq_cancel(q, t);                    /* dequeue BEFORE the callback, so */
        if (t->period)                      /* the callback may re-arm or free it */
            tq_add(q, t, t->expires + t->period);
        t->cb(t, now, t->user);
        fired++;
    }
    return fired;
}

/* ------------------------------------------------------------------------ */

typedef struct { int log[4096]; size_t n; } FireLog;

static void record(Timer *t, uint64_t now, void *user)
{
    (void)now;
    FireLog *fl = user;
    if (fl->n < 4096) fl->log[fl->n++] = t->id;
}

static void print_cb(Timer *t, uint64_t now, void *user)
{
    (void)user;
    printf("  t=%3llu  timer %d fires%s\n", (unsigned long long)now, t->id,
           t->period ? " (periodic, re-armed)" : "");
}

static bool heap_ok(const TimerQueue *q)
{
    for (size_t i = 0; i < q->len; i++) {
        if (q->h[i]->heap_idx != i) return false;
        if (i && earlier(q->h[i], q->h[(i - 1) / 2])) return false;
    }
    return true;
}

int main(void)
{
    TimerQueue q;
    tq_init(&q);

    /* Demo: like a GStreamer pipeline waiting on clock ids, plus a periodic tick. */
    Timer d[4] = {
        {.period = 0,  .cb = print_cb, .id = 1, .heap_idx = SIZE_MAX},
        {.period = 0,  .cb = print_cb, .id = 2, .heap_idx = SIZE_MAX},
        {.period = 40, .cb = print_cb, .id = 3, .heap_idx = SIZE_MAX},
        {.period = 0,  .cb = print_cb, .id = 4, .heap_idx = SIZE_MAX},
    };
    tq_add(&q, &d[0], 100);
    tq_add(&q, &d[1], 30);
    tq_add(&q, &d[2], 40);
    tq_add(&q, &d[3], 70);
    tq_cancel(&q, &d[3]);                   /* e.g. a seek cancelled that wait */
    printf("event loop (timer 4 cancelled):\n");
    uint64_t now = 0, when;
    while (tq_next(&q, &when) && when <= 130) {
        now = when;                         /* real loop: epoll_wait(timeout = when - now) */
        tq_run(&q, now);
    }
    tq_cancel(&q, &d[2]);                   /* stop the periodic tick */
    tq_cancel(&q, &d[0]);
    assert(q.len == 0);

    /* Randomised: random add / cancel / advance-clock, check against brute force:
     * the timers that fire must be exactly the armed ones with expires <= now,
     * in (expires, seq) order. */
    enum { NT = 64 };
    Timer *ts = calloc(NT, sizeof *ts);
    bool *armed = calloc(NT, sizeof *armed);
    assert(ts && armed);
    FireLog *fl = calloc(1, sizeof *fl);
    assert(fl);
    for (int i = 0; i < NT; i++)
        ts[i] = (Timer){.cb = record, .user = fl, .id = i, .heap_idx = SIZE_MAX};

    srand(2024);
    now = 0;
    for (int step = 0; step < 20000; step++) {
        int op = rand() % 10, i = rand() % NT;
        if (op < 5 && !armed[i]) {
            tq_add(&q, &ts[i], now + (uint64_t)(rand() % 100));
            armed[i] = true;
        } else if (op < 7) {
            tq_cancel(&q, &ts[i]);
            armed[i] = false;
        } else {
            now += (uint64_t)(rand() % 20);
            /* brute-force expected order */
            int expect[NT]; size_t ne = 0;
            for (int j = 0; j < NT; j++)
                if (armed[j] && ts[j].expires <= now) expect[ne++] = j;
            for (size_t a = 1; a < ne; a++)            /* insertion sort by (expires, seq) */
                for (size_t b = a; b > 0 && earlier(&ts[expect[b]], &ts[expect[b - 1]]); b--) {
                    int t = expect[b]; expect[b] = expect[b - 1]; expect[b - 1] = t;
                }
            fl->n = 0;
            int fired = tq_run(&q, now);
            assert((size_t)fired == ne && fl->n == ne);
            for (size_t k = 0; k < ne; k++) {
                assert(fl->log[k] == expect[k]);
                armed[expect[k]] = false;
            }
        }
        assert(heap_ok(&q));
        size_t cnt = 0;
        for (int j = 0; j < NT; j++) cnt += armed[j];
        assert(cnt == q.len);
    }
    printf("20000 random add/cancel/run steps: firing order matches brute force\n");

    free(ts); free(armed); free(fl);
    tq_free(&q);
    printf("all checks passed\n");
    return 0;
}
