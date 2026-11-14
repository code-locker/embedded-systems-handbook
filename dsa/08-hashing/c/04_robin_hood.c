/*
 * 04_robin_hood.c — Robin Hood hashing (linear probing that "steals from the
 *                   rich") with backward-shift deletion (no tombstones)
 *
 * Concept : Every entry remembers its DISTANCE from its home slot (how far
 *           it had to probe). While inserting, if the entry sitting in a slot
 *           is CLOSER to home than the one we carry ("richer"), we swap and
 *           carry the evicted one onward. This evens out probe lengths: the
 *           worst-case lookup at 90% load stays short. Used by Rust's old
 *           HashMap, many game engines, and databases.
 * Build   : make run P=dsa/08-hashing/c/04_robin_hood.c
 * Read    : ../README.md  (section "3.6 Robin Hood hashing")
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                    __LINE__);                                                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* dist == -1 means EMPTY; otherwise slots away from the home slot. */
struct slot {
    uint32_t key;
    int value;
    int dist;
};

struct rh {
    struct slot *s; /* owned */
    size_t cap;     /* power of two */
    size_t size;
    int robin;      /* 1 = Robin Hood, 0 = plain linear probing (to compare) */
};

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x85ebca6bu;
    x ^= x >> 13; x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

static void rh_init(struct rh *t, size_t cap, int robin)
{
    t->s = malloc(cap * sizeof *t->s);
    CHECK(t->s != NULL);
    for (size_t i = 0; i < cap; i++) t->s[i].dist = -1;
    t->cap = cap;
    t->size = 0;
    t->robin = robin;
}

/*
 * Insert (no resize here: the demo fixes the load factor on purpose).
 * Example, cap 8, numbers are distances; we insert X whose home is 2:
 *
 *   slot:    2     3     4     5
 *          [A:0] [B:1] [C:0] [   ]
 *   X at 2: dist 0 vs A 0 -> not richer, move on   (X dist 1)
 *   X at 3: dist 1 vs B 1 -> not richer, move on   (X dist 2)
 *   X at 4: dist 2 vs C 0 -> C is RICHER: swap.
 *          X lands in 4 with dist 2; now carry C (dist 0 -> 1 at slot 5)
 *   C at 5: empty -> place.
 *          [A:0] [B:1] [X:2] [C:1]     max dist 2 instead of 3
 *
 * Average O(1), and the VARIANCE of probe length is much lower.
 */
static void rh_put(struct rh *t, uint32_t key, int value)
{
    CHECK(t->size < t->cap);
    struct slot cur = {key, value, 0};
    size_t i = mix(key) & (t->cap - 1);
    for (;;) {
        struct slot *sl = &t->s[i];
        if (sl->dist < 0) { *sl = cur; t->size++; return; }
        if (sl->key == cur.key) { sl->value = cur.value; return; }
        if (t->robin && sl->dist < cur.dist) {
            struct slot tmp = *sl; /* evict the richer one, carry it on */
            *sl = cur;
            cur = tmp;
        }
        cur.dist++;
        i = (i + 1) & (t->cap - 1);
    }
}

/*
 * Lookup with EARLY EXIT — Robin Hood's second gift: if the slot we look at
 * holds an entry with dist < our current distance, our key would have
 * evicted it during insert, so our key cannot be further on. Misses stop
 * early instead of scanning the whole cluster.
 */
static struct slot *rh_find(struct rh *t, uint32_t key, int *probes)
{
    size_t i = mix(key) & (t->cap - 1);
    for (int d = 0;; d++) {
        struct slot *sl = &t->s[i];
        (*probes)++;
        if (sl->dist < 0) return NULL;
        if (t->robin && sl->dist < d) return NULL;
        if (sl->key == key) return sl;
        i = (i + 1) & (t->cap - 1);
    }
}

/*
 * Backward-shift deletion (Robin Hood only): remove, then slide following
 * entries one slot back while they are not at home (dist > 0). No
 * tombstones, and distances get SHORTER.
 *
 *   slot:   2     3     4     5     6
 *         [A:0] [X:1] [B:2] [C:1] [   ]      delete X
 *         [A:0] [B:1] [C:0] [   ] [   ]      B and C slid left, dist-1
 */
static int rh_del(struct rh *t, uint32_t key)
{
    int dummy = 0;
    struct slot *sl = rh_find(t, key, &dummy);
    if (!sl) return 0;
    size_t i = (size_t)(sl - t->s);
    for (;;) {
        size_t next = (i + 1) & (t->cap - 1);
        if (t->s[next].dist <= 0) { t->s[i].dist = -1; break; } /* empty or at home */
        t->s[i] = t->s[next];
        t->s[i].dist--;
        i = next;
    }
    t->size--;
    return 1;
}

static int max_dist(const struct rh *t)
{
    int m = 0;
    for (size_t i = 0; i < t->cap; i++)
        if (t->s[i].dist > m) m = t->s[i].dist;
    return m;
}

int main(void)
{
    const size_t cap = 1u << 14;            /* 16384 slots */
    const uint32_t n = (uint32_t)(cap * 9 / 10); /* 90% full: stress case */

    struct rh lin, rob;
    rh_init(&lin, cap, 0);
    rh_init(&rob, cap, 1);
    for (uint32_t k = 0; k < n; k++) {
        rh_put(&lin, k, (int)k);
        rh_put(&rob, k, (int)k);
    }

    int pl_hit = 0, pr_hit = 0, pl_miss = 0, pr_miss = 0;
    for (uint32_t k = 0; k < n; k++) {
        struct slot *a = rh_find(&lin, k, &pl_hit);
        struct slot *b = rh_find(&rob, k, &pr_hit);
        CHECK(a && b && a->value == (int)k && b->value == (int)k);
    }
    for (uint32_t k = n; k < n + 5000; k++) {
        CHECK(rh_find(&lin, k, &pl_miss) == NULL);
        CHECK(rh_find(&rob, k, &pr_miss) == NULL);
    }
    printf("load 0.90, %u keys\n", n);
    printf("               max dist   avg probes(hit)  avg probes(miss)\n");
    printf("linear         %8d   %15.2f  %16.2f\n", max_dist(&lin),
           (double)pl_hit / n, pl_miss / 5000.0);
    printf("robin hood     %8d   %15.2f  %16.2f\n", max_dist(&rob),
           (double)pr_hit / n, pr_miss / 5000.0);
    /* Same total displacement (same avg hit cost) but a far smaller worst
     * case, and misses exit early. */
    CHECK(max_dist(&rob) < max_dist(&lin));
    CHECK(pr_miss < pl_miss);

    /* Deletion: remove every 3rd key, check the rest, no tombstones. */
    for (uint32_t k = 0; k < n; k += 3) CHECK(rh_del(&rob, k));
    for (uint32_t k = 0; k < n; k++) {
        int p = 0;
        struct slot *s = rh_find(&rob, k, &p);
        CHECK((k % 3 == 0) ? s == NULL : (s != NULL && s->value == (int)k));
    }
    /* invariant: every entry's dist really is its distance from home */
    for (size_t i = 0; i < cap; i++) {
        if (rob.s[i].dist < 0) continue;
        size_t home = mix(rob.s[i].key) & (cap - 1);
        CHECK(((i - home) & (cap - 1)) == (size_t)rob.s[i].dist);
    }
    printf("after deleting 1/3: size=%zu max dist=%d\n", rob.size, max_dist(&rob));

    free(lin.s);
    free(rob.s);
    printf("all robin-hood checks passed\n");
    return 0;
}
