/*
 * 06_token_bucket.c — rate limiter: token bucket (+ GCRA equivalence, fixed-window flaw)
 *
 * Concept : A bucket holds at most B tokens and refills at r tokens/second.
 *           A request takes a token or is rejected. Result: long-run rate
 *           <= r, short bursts up to B allowed. No timer is needed — refill
 *           is computed lazily from the elapsed time on each request.
 *           Integer trick: store credit in nanoseconds (1 token = T ns, T=1/r)
 *           so there is no floating-point drift.
 * Build   : make run P=dsa/19-system-design-structures/c/06_token_bucket.c
 * Read    : ../README.md  (section "Rate limiter: token bucket")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 *   tokens
 *   B=4 |####           ###            refill slope = r tokens/s
 *       |    #         #   #
 *       |     #      ##     #
 *       |      ######        #####     flat at 0: requests rejected
 *     0 +-----------------------------> time
 *         burst    idle     burst
 *
 *  on request at time now:
 *      credit = min(B*T, credit + (now - last));  last = now
 *      if credit >= T: credit -= T, ALLOW  else REJECT
 */
typedef struct {
    uint64_t T;            /* ns per token = 1e9 / rate */
    uint64_t cap;          /* B * T  (bucket size in ns of credit) */
    uint64_t credit;       /* current tokens * T */
    uint64_t last;         /* time of last update, ns */
} TokenBucket;

static void tb_init(TokenBucket *b, uint64_t ns_per_token, uint64_t burst, uint64_t now)
{
    b->T = ns_per_token;
    b->cap = burst * ns_per_token;
    b->credit = b->cap;    /* start full: allow an initial burst */
    b->last = now;
}

/* O(1), no background thread. `now` must be monotonic (CLOCK_MONOTONIC in
   real code — wall clock can jump backwards with NTP). */
static int tb_allow(TokenBucket *b, uint64_t now, uint64_t cost)
{
    uint64_t elapsed = now - b->last;
    b->last = now;
    /* Saturating add: careful with overflow if idle for a very long time. */
    b->credit = (elapsed >= b->cap - b->credit) ? b->cap : b->credit + elapsed;
    uint64_t need = cost * b->T;
    if (b->credit >= need) { b->credit -= need; return 1; }
    return 0;
}

/*
 * GCRA (Generic Cell Rate Algorithm, from ATM networks; used by many API
 * gateways). Stores ONE number: TAT, the "theoretical arrival time" of the
 * next request if traffic were perfectly paced. It is mathematically the
 * same limiter as the token bucket — we check that below.
 */
typedef struct { uint64_t T, tau, tat; } Gcra;

static int gcra_allow(Gcra *g, uint64_t now)
{
    uint64_t tat = g->tat > now ? g->tat : now;
    if (tat > now + g->tau) return 0;          /* too early: would exceed burst */
    g->tat = tat + g->T;
    return 1;
}

/* Fixed-window counter: simplest limiter, but lets 2x the limit through
   around a window boundary. */
typedef struct { uint64_t window, limit, start, count; } FixedWindow;

static int fw_allow(FixedWindow *f, uint64_t now)
{
    if (now - f->start >= f->window) { f->start = now - (now % f->window); f->count = 0; }
    if (f->count < f->limit) { f->count++; return 1; }
    return 0;
}

#define MS 1000000ull
#define SEC 1000000000ull

int main(void)
{
    printf("== rate 10/s (T=100ms), burst 4 ==\n");
    TokenBucket b;
    tb_init(&b, 100 * MS, 4, 0);
    int allowed = 0;
    for (int i = 0; i < 6; i++) {             /* 6 requests at t=0 */
        int ok = tb_allow(&b, 0, 1);
        printf("t=0ms req %d -> %s  (tokens left %.1f)\n", i, ok ? "ALLOW" : "reject",
               (double)b.credit / (double)b.T);
        allowed += ok;
    }
    assert(allowed == 4);                     /* exactly the burst */
    assert(!tb_allow(&b, 50 * MS, 1));        /* half a token: not enough */
    assert(tb_allow(&b, 100 * MS, 1));        /* one full token refilled */
    assert(!tb_allow(&b, 100 * MS, 1));
    assert(tb_allow(&b, 10 * SEC, 4));        /* long idle: refills to cap only */
    assert(!tb_allow(&b, 10 * SEC, 1));
    printf("half-token reject, exact refill, cap after idle: checked\n");

    printf("\n== flood at 1000 req/s for 5 s ==\n");
    tb_init(&b, 100 * MS, 4, 0);
    allowed = 0;
    for (uint64_t t = 0; t < 5 * SEC; t += 1 * MS) allowed += tb_allow(&b, t, 1);
    printf("allowed %d of 5000 (burst 4 + one refill every 100 ms at t=100..4900 ms = 4 + 49 = 53)\n", allowed);
    assert(allowed == 4 + 49);   /* t=5000 ms is outside the loop */

    printf("\n== token bucket == GCRA on random traffic, and the burst bound ==\n");
    srand(6);
    for (int trial = 0; trial < 200; trial++) {
        uint64_t T = (uint64_t)(1 + rand() % 50) * MS;
        uint64_t B = (uint64_t)(1 + rand() % 8);
        tb_init(&b, T, B, 0);
        Gcra g = { T, (B - 1) * T, 0 };
        uint64_t now = 0, stamps[600];
        int n = 0;
        for (int i = 0; i < 600; i++) {
            now += (uint64_t)(rand() % 3 == 0 ? rand() % 200 : rand() % 5) * MS;
            int x = tb_allow(&b, now, 1), y = gcra_allow(&g, now);
            assert(x == y);
            if (x) stamps[n++] = now;
        }
        /* In ANY interval [ti, tj], allowed count <= B + (tj - ti) / T. */
        for (int i = 0; i < n; i++)
            for (int j = i; j < n; j++)
                assert((uint64_t)(j - i + 1) <= B + (stamps[j] - stamps[i]) / T);
    }
    printf("200 random traces: identical decisions; burst bound holds for every window\n");

    printf("\n== why not a fixed window? limit 10 per 1s window ==\n");
    FixedWindow f = { SEC, 10, 0, 0 };
    int in_200ms = 0;
    for (int i = 0; i < 10; i++) in_200ms += fw_allow(&f, 900 * MS + (uint64_t)i * MS);
    for (int i = 0; i < 10; i++) in_200ms += fw_allow(&f, 1000 * MS + (uint64_t)i * MS);
    printf("fixed window let %d requests through within ~110 ms (limit is 10/s!)\n", in_200ms);
    assert(in_200ms == 20);
    tb_init(&b, 100 * MS, 10, 0);
    int tb_cnt = 0;
    for (int i = 0; i < 10; i++) tb_cnt += tb_allow(&b, 900 * MS + (uint64_t)i * MS, 1);
    for (int i = 0; i < 10; i++) tb_cnt += tb_allow(&b, 1000 * MS + (uint64_t)i * MS, 1);
    printf("token bucket (burst 10) in the same pattern: %d\n", tb_cnt);
    assert(tb_cnt == 11);   /* 10 burst + one token refilled during the 110 ms */
    printf("OK\n");
    return 0;
}
