/*
 * 03_open_addressing.c — open addressing: linear, quadratic, double hashing,
 *                        tombstones, load factor and rehash
 *
 * Concept : No linked lists: every entry lives directly in one flat array.
 *           On collision, PROBE other slots in a fixed sequence. Deletion
 *           must leave a TOMBSTONE, or later lookups stop too early. The
 *           three probe sequences differ in how badly entries clump.
 * Build   : make run P=dsa/08-hashing/c/03_open_addressing.c
 * Read    : ../README.md  (section "3.4 Open addressing")
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

/*
 * Each slot is in one of three states:
 *
 *   EMPTY      never used         -> a lookup can STOP here: key not present
 *   FULL       holds key/value
 *   TOMBSTONE  was FULL, deleted  -> lookup must KEEP GOING; insert may reuse
 *
 * Why tombstones? Keys A, B, C all hash to slot 2 (linear probing):
 *
 *   slot:  1     2     3     4     5
 *         [ ]  [ A ] [ B ] [ C ] [ ]
 *   delete B by marking EMPTY:
 *         [ ]  [ A ] [   ] [ C ] [ ]
 *   get(C): start at 2 (A, no) -> 3 EMPTY -> "not found"   WRONG!
 *   delete B by marking TOMBSTONE (†):
 *         [ ]  [ A ] [ † ] [ C ] [ ]
 *   get(C): 2 (A) -> 3 (†, keep going) -> 4 (C) found      RIGHT
 */
enum state { EMPTY = 0, FULL, TOMBSTONE };
enum probe { LINEAR, QUADRATIC, DOUBLE };

struct slot {
    uint32_t key;
    int value;
    unsigned char state;
};

struct table {
    struct slot *slots; /* owned */
    size_t cap;         /* power of two */
    size_t used;        /* FULL slots */
    size_t tombs;       /* TOMBSTONE slots (they still lengthen probes!) */
    enum probe mode;
    size_t total_probes; /* statistics for the comparison at the end */
};

/* Integer mixer (from MurmurHash3's finaliser): spreads every input bit
 * over every output bit, so "& (cap-1)" is safe. O(1). */
static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x85ebca6bu;
    x ^= x >> 13; x *= 0xc2b2ae35u;
    x ^= x >> 16;
    return x;
}

/* Second, independent hash for double hashing. Must be ODD so that
 * gcd(step, cap) == 1 for a power-of-two cap -> the sequence visits
 * EVERY slot before repeating (an even step would skip half the table). */
static size_t step2(uint32_t key) { return (size_t)(mix(key ^ 0x9e3779b9u) | 1u); }

/*
 * i-th slot to try for this key (i = 0, 1, 2, ...):
 *
 *   linear    : h + i               -> 5,6,7,8...  cache friendly, but
 *               runs of FULL slots merge into long "primary clusters".
 *   quadratic : h + i(i+1)/2        -> 5,6,8,11,15... jumps grow, breaks
 *               primary clusters. Triangular numbers on a power-of-two
 *               table visit every slot (plain h+i^2 would NOT).
 *   double    : h + i*step2(key)    -> keys that collide at h still follow
 *               DIFFERENT paths: no secondary clustering either.
 */
static size_t probe_at(const struct table *t, uint32_t key, size_t i)
{
    size_t h = mix(key);
    switch (t->mode) {
    case LINEAR:    return (h + i) & (t->cap - 1);
    case QUADRATIC: return (h + i * (i + 1) / 2) & (t->cap - 1);
    case DOUBLE:    return (h + i * step2(key)) & (t->cap - 1);
    }
    return 0;
}

static void table_init(struct table *t, size_t cap, enum probe mode)
{
    t->slots = calloc(cap, sizeof *t->slots); /* calloc -> all EMPTY */
    CHECK(t->slots != NULL);
    t->cap = cap;
    t->used = t->tombs = t->total_probes = 0;
    t->mode = mode;
}

static void table_put(struct table *t, uint32_t key, int value);

/* Rebuild into a fresh array: reinsert FULL slots, DROP tombstones. O(cap). */
static void table_rehash(struct table *t, size_t new_cap)
{
    struct slot *old = t->slots;
    size_t old_cap = t->cap;
    size_t probes = t->total_probes;
    table_init(t, new_cap, t->mode);
    for (size_t i = 0; i < old_cap; i++)
        if (old[i].state == FULL) table_put(t, old[i].key, old[i].value);
    t->total_probes = probes; /* keep stats about user ops only */
    free(old);
}

/* Average O(1/(1-α)) probes for a miss with linear probing (α = load). */
static void table_put(struct table *t, uint32_t key, int value)
{
    /* Grow when FULL + TOMBSTONE exceeds 1/2. Open addressing needs a lower
     * max load than chaining: probe lengths explode as α -> 1. Counting
     * tombstones matters: they occupy slots that lookups must walk over. */
    if (2 * (t->used + t->tombs + 1) > t->cap)
        table_rehash(t, (t->used + 1) * 2 > t->cap / 2 ? t->cap * 2 : t->cap);

    size_t first_tomb = SIZE_MAX;
    for (size_t i = 0; i < t->cap; i++) {
        size_t s = probe_at(t, key, i);
        t->total_probes++;
        struct slot *sl = &t->slots[s];
        if (sl->state == FULL && sl->key == key) { sl->value = value; return; }
        if (sl->state == TOMBSTONE && first_tomb == SIZE_MAX) first_tomb = s;
        if (sl->state == EMPTY) {
            /* Key is definitely absent. Reuse the first tombstone we passed
             * (keeps probe paths short) or take this empty slot. */
            if (first_tomb != SIZE_MAX) { sl = &t->slots[first_tomb]; t->tombs--; }
            sl->key = key;
            sl->value = value;
            sl->state = FULL;
            t->used++;
            return;
        }
    }
    CHECK(0 && "table full: cannot happen because load is kept <= 1/2");
}

static struct slot *table_find(struct table *t, uint32_t key)
{
    for (size_t i = 0; i < t->cap; i++) {
        struct slot *sl = &t->slots[probe_at(t, key, i)];
        t->total_probes++;
        if (sl->state == EMPTY) return NULL;          /* stop: absent */
        if (sl->state == FULL && sl->key == key) return sl;
        /* TOMBSTONE or other key: keep probing */
    }
    return NULL;
}

static int table_del(struct table *t, uint32_t key)
{
    struct slot *sl = table_find(t, key);
    if (!sl) return 0;
    sl->state = TOMBSTONE; /* NOT EMPTY — see the picture at the top */
    t->used--;
    t->tombs++;
    return 1;
}

static void tombstone_demo(void)
{
    /* Force a collision chain with LINEAR probing on a tiny table by
     * finding three keys that hash to the same home slot. */
    struct table t;
    table_init(&t, 16, LINEAR);
    uint32_t same[3];
    int n = 0;
    uint32_t home = mix(1) & 15;
    for (uint32_t k = 1; n < 3; k++)
        if ((mix(k) & 15) == home) same[n++] = k;

    table_put(&t, same[0], 10); /* home   */
    table_put(&t, same[1], 20); /* home+1 */
    table_put(&t, same[2], 30); /* home+2 */
    CHECK(table_del(&t, same[1]));           /* middle becomes a tombstone */
    CHECK(t.slots[(home + 1) & 15].state == TOMBSTONE);
    struct slot *c = table_find(&t, same[2]); /* must walk past the † */
    CHECK(c && c->value == 30);
    table_put(&t, same[1], 21);               /* reuses the tombstone slot */
    CHECK(t.slots[(home + 1) & 15].state == FULL && t.tombs == 0);
    printf("tombstone demo: keys %u,%u,%u share home slot %u -> OK\n",
           same[0], same[1], same[2], home);
    free(t.slots);
}

static void compare_modes(void)
{
    const char *names[] = {"linear", "quadratic", "double"};
    for (int mode = LINEAR; mode <= DOUBLE; mode++) {
        struct table t;
        table_init(&t, 8, (enum probe)mode);
        /* keys with structure (multiples of 8) — common in practice */
        for (uint32_t k = 0; k < 20000; k++) table_put(&t, k * 8, (int)k);
        t.total_probes = 0;
        for (uint32_t k = 0; k < 20000; k++) {
            struct slot *s = table_find(&t, k * 8);
            CHECK(s && s->value == (int)k);
        }
        for (uint32_t k = 20000; k < 40000; k++) /* misses are the slow case */
            CHECK(table_find(&t, k * 8) == NULL);
        /* delete half, then look up the rest: tombstones in the way */
        for (uint32_t k = 0; k < 20000; k += 2) CHECK(table_del(&t, k * 8));
        for (uint32_t k = 1; k < 20000; k += 2) CHECK(table_find(&t, k * 8));
        printf("%-9s cap=%-6zu used=%-5zu tombs=%-5zu avg probes/op=%.2f\n",
               names[mode], t.cap, t.used, t.tombs,
               (double)t.total_probes / 50000.0);
        CHECK(t.used == 10000);
        free(t.slots);
    }
}

int main(void)
{
    tombstone_demo();
    compare_modes();
    printf("all open-addressing checks passed\n");
    return 0;
}
