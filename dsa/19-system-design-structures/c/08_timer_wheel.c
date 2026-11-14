/*
 * 08_timer_wheel.c — hierarchical timing wheel (the classic Linux kernel timer design)
 *
 * Concept : Thousands of timers (TCP retransmit, keepalive, timeouts), most
 *           cancelled before they fire. A heap gives O(log n) add/cancel; a
 *           timing wheel gives O(1): an array of buckets indexed by expiry
 *           tick, like a clock face. Far-away timers live in coarser outer
 *           wheels and "cascade" inward as time approaches — exactly
 *           kernel/timer.c before Linux 4.8 (tv1..tv5, 256+64*4 slots).
 * Build   : make run P=dsa/19-system-design-structures/c/08_timer_wheel.c
 * Read    : ../README.md  (section "Timer wheel")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 *  4 levels x 64 slots, 1 tick = e.g. 1 ms (a "jiffy")
 *
 *  level 0: slot = expires & 63           covers now .. now+63        (1 tick/slot)
 *  level 1: slot = (expires>>6)  & 63     covers up to 64^2 ticks     (64 ticks/slot)
 *  level 2: slot = (expires>>12) & 63     covers up to 64^3 ticks
 *  level 3: slot = (expires>>18) & 63     covers up to 64^4 ticks
 *
 *        level 0 (fine)                    level 1 (coarse)
 *   +--+--+--+--+--...--+--+         +----+----+----+--...
 *   | 0| 1| 2| 3|       |63|         | 0  | 1  | 2  |
 *   +--+--+--+--+--...--+--+         +----+----+----+--...
 *         ^ now&63                          |
 *                                           +--> when level 0 wraps to slot 0,
 *                                                every timer in this level-1
 *                                                slot is re-inserted ("cascade")
 *                                                and lands in an exact level-0 slot.
 */
#define BITS   6
#define SLOTS  (1u << BITS)
#define MASK   (SLOTS - 1)
#define LEVELS 4
#define MAX_DELTA ((1ull << (BITS * LEVELS)) - 1)

typedef struct Timer {
    struct Timer *next, **pprev;   /* hlist style: O(1) unlink, no head special case */
    uint64_t expires;
    void (*fn)(struct Timer *, uint64_t now);
    uint64_t fired_at;
    int fired_count;
} Timer;

typedef struct {
    uint64_t now;                  /* the tick about to be processed */
    Timer *slot[LEVELS][SLOTS];
    size_t pending;
} Wheel;

static void slot_push(Timer **head, Timer *t)
{
    t->next = *head;
    if (*head) (*head)->pprev = &t->next;
    *head = t;
    t->pprev = head;
}

static void timer_unlink(Timer *t)
{
    *t->pprev = t->next;
    if (t->next) t->next->pprev = t->pprev;
    t->next = NULL;
    t->pprev = NULL;
}

/* O(1): pick the level from how FAR away expiry is, the slot from the
   expiry's own bits at that level. */
static void internal_add(Wheel *w, Timer *t)
{
    uint64_t delta = t->expires < w->now ? 0 : t->expires - w->now;
    uint64_t e = t->expires < w->now ? w->now : t->expires;   /* overdue: fire next tick */
    int level = 0;
    while (level < LEVELS - 1 && delta >= (1ull << (BITS * (level + 1))))
        level++;
    slot_push(&w->slot[level][(e >> (BITS * level)) & MASK], t);
}

static void timer_add(Wheel *w, Timer *t, uint64_t expires)
{
    assert(expires - w->now <= MAX_DELTA);   /* Linux clamps; we just refuse */
    t->expires = expires;
    internal_add(w, t);
    w->pending++;
}

/* O(1): this is the whole point — most timers are cancelled, cheaply. */
static int timer_cancel(Wheel *w, Timer *t)
{
    if (!t->pprev) return 0;                 /* not pending */
    timer_unlink(t);
    w->pending--;
    return 1;
}

/* Move every timer in level[lvl] slot idx back through internal_add. Returns
   idx so the caller knows whether this level ALSO wrapped (then cascade the
   next level too) — same trick as Linux's cascade(). */
static unsigned cascade(Wheel *w, int lvl, unsigned idx)
{
    Timer *t = w->slot[lvl][idx];
    w->slot[lvl][idx] = NULL;
    while (t) {
        Timer *next = t->next;
        internal_add(w, t);                   /* lands in a finer level */
        t = next;
    }
    return idx;
}

/* Process ticks until w->now > target. Cost per tick: O(1) + timers fired
   + (rarely) one cascade. */
static void wheel_advance(Wheel *w, uint64_t target)
{
    while (w->now <= target) {
        unsigned idx = (unsigned)(w->now & MASK);
        if (idx == 0) {                       /* level 0 wrapped: pull level 1 in */
            for (int lvl = 1; lvl < LEVELS; lvl++)
                if (cascade(w, lvl, (unsigned)((w->now >> (BITS * lvl)) & MASK)) != 0)
                    break;
        }
        Timer *t = w->slot[0][idx];
        w->slot[0][idx] = NULL;
        while (t) {
            Timer *next = t->next;            /* save first: fn may re-arm t */
            t->next = NULL;
            t->pprev = NULL;
            w->pending--;
            t->fn(t, w->now);
            t = next;
        }
        w->now++;
    }
}

static void on_fire(Timer *t, uint64_t now)
{
    t->fired_at = now;
    t->fired_count++;
}

/* A periodic timer re-arms itself from its own callback. */
static Wheel *g_wheel;
static void periodic(Timer *t, uint64_t now)
{
    on_fire(t, now);
    if (t->fired_count < 5) timer_add(g_wheel, t, now + 250);   /* 5 shots */
}

int main(void)
{
    static Wheel w;                            /* zero-initialised: now = 0 */
    g_wheel = &w;

    printf("== a few timers, traced ==\n");
    static Timer a, b, c, d;
    a.fn = b.fn = c.fn = d.fn = on_fire;
    timer_add(&w, &a, 5);         /* level 0 */
    timer_add(&w, &b, 70);        /* level 1: 70-0 >= 64 */
    timer_add(&w, &c, 5000);      /* level 2: >= 4096 */
    timer_add(&w, &d, 300);
    timer_cancel(&w, &d);         /* cancelled before firing */
    wheel_advance(&w, 6000);
    printf("a: expires 5    fired at %llu\n", (unsigned long long)a.fired_at);
    printf("b: expires 70   fired at %llu (cascaded from level 1 at tick 64)\n",
           (unsigned long long)b.fired_at);
    printf("c: expires 5000 fired at %llu (level 2 -> 1 -> 0)\n", (unsigned long long)c.fired_at);
    printf("d: cancelled, fired %d times\n", d.fired_count);
    assert(a.fired_at == 5 && b.fired_at == 70 && c.fired_at == 5000 && d.fired_count == 0);
    assert(w.pending == 0);

    printf("\n== periodic timer re-arming from its callback ==\n");
    static Timer p;
    p.fn = periodic;
    uint64_t start = w.now;
    timer_add(&w, &p, start + 250);
    wheel_advance(&w, start + 2000);
    printf("periodic fired %d times, last at now0+%llu\n", p.fired_count,
           (unsigned long long)(p.fired_at - start));
    assert(p.fired_count == 5 && p.fired_at == start + 5 * 250);

    printf("\n== 5000 random timers vs exact expectation (incl. cancels) ==\n");
    enum { N = 5000 };
    static Timer ts[N];
    static int cancelled[N];
    srand(8);
    uint64_t base = w.now;
    for (int i = 0; i < N; i++) {
        ts[i].fn = on_fire;
        /* mix of near (level 0), medium and far (levels 2,3) deadlines */
        uint64_t delta;
        switch (rand() % 4) {
        case 0:  delta = (uint64_t)(rand() % 64); break;
        case 1:  delta = (uint64_t)(rand() % 4096); break;
        case 2:  delta = (uint64_t)(rand() % 262144); break;
        default: delta = (uint64_t)rand() % 2000000u; break;
        }
        timer_add(&w, &ts[i], base + delta);
    }
    /* advance in random-size steps and cancel random timers along the way */
    uint64_t horizon = base + 2000000;
    while (w.now <= horizon) {
        uint64_t step = (uint64_t)(1 + rand() % 50000);
        wheel_advance(&w, w.now + step);
        for (int k = 0; k < 50; k++) {
            int i = rand() % N;
            if (timer_cancel(&w, &ts[i])) cancelled[i] = 1;
        }
    }
    int fired = 0, ncancel = 0;
    for (int i = 0; i < N; i++) {
        if (cancelled[i]) { assert(ts[i].fired_count == 0); ncancel++; continue; }
        assert(ts[i].fired_count == 1);
        assert(ts[i].fired_at == ts[i].expires);  /* exact tick, even after cascades */
        fired++;
    }
    assert(w.pending == 0);
    printf("%d fired at their exact tick, %d cancelled and never fired\n", fired, ncancel);
    printf("OK\n");
    return 0;
}
