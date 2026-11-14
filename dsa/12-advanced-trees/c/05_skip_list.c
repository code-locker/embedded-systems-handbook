/*
 * 05_skip_list.c — skip list: a sorted linked list with randomised "express lanes"
 *
 * Concept : Level 0 is an ordinary sorted linked list. Each node is also
 *           promoted to level 1 with probability 1/2, to level 2 with 1/4,
 *           ... Searching starts on the top (sparsest) lane and drops down a
 *           lane whenever the next node would overshoot. Expected O(log n)
 *           search/insert/delete with NO rotations or rebalancing — which is
 *           why Redis (sorted sets) and LevelDB/RocksDB (memtable) use it.
 * Build   : make run P=dsa/12-advanced-trees/c/05_skip_list.c
 * Read    : ../README.md  (section "Skip list")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_LEVEL 16 /* enough for ~2^16 = 65536 elements with p = 1/2 */

/*
 * Picture (search for 17: follow the arrows, drop when next > 17):
 *
 *  L3  HEAD ------------------------------------------> 21 ----------> NIL
 *  L2  HEAD ---------------> 9 -----------------------> 21 ----------> NIL
 *  L1  HEAD -------> 4 ----> 9 ---------> 14 ---------> 21 ----------> NIL
 *  L0  HEAD -> 1 --> 4 -> 7 -> 9 -> 12 -> 14 -> 17 ---> 21 -> 25 ----> NIL
 *
 *  L3: HEAD->21? overshoot, drop.  L2: ->9 ok, 9->21? overshoot, drop.
 *  L1: 9->14 ok, 14->21? drop.     L0: 14->17 found.
 */
typedef struct SLNode {
    int key;
    int level;                   /* number of lanes this node is on (1..MAX_LEVEL) */
    struct SLNode *next[];       /* flexible array member: next[0..level-1]        */
} SLNode;

typedef struct {
    SLNode *head;                /* sentinel with MAX_LEVEL lanes, holds no key */
    int level;                   /* highest lane currently in use               */
    int size;
} SkipList;

static uint32_t rng_state = 1u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Coin flips: P(level >= k) = 1/2^(k-1). Expected level = 2, so ~2 pointers/node. */
static int random_level(void)
{
    int lvl = 1;
    while (lvl < MAX_LEVEL && (rnd() & 1u))
        lvl++;
    return lvl;
}

static SLNode *node_new(int key, int level)
{
    /* one allocation holds the key AND its variable-length array of next pointers */
    SLNode *n = calloc(1, sizeof *n + (size_t)level * sizeof(SLNode *));
    assert(n);
    n->key = key;
    n->level = level;
    return n;
}

static void sl_init(SkipList *s)
{
    s->head = node_new(0, MAX_LEVEL);
    s->level = 1;
    s->size = 0;
}

/* O(n): walk lane 0 (every node is on it) and free each node, then the head. */
static void sl_free(SkipList *s)
{
    SLNode *x = s->head->next[0];
    while (x) {
        SLNode *nx = x->next[0];
        free(x);
        x = nx;
    }
    free(s->head);
    s->head = NULL;
}

/* Expected O(log n): ~2 steps per lane, ~log2(n) lanes. */
static bool sl_contains(const SkipList *s, int key)
{
    const SLNode *x = s->head;
    for (int i = s->level - 1; i >= 0; i--)
        while (x->next[i] && x->next[i]->key < key)
            x = x->next[i];          /* move right on this lane */
    x = x->next[0];                  /* candidate on lane 0     */
    return x && x->key == key;
}

/*
 * Insert, expected O(log n). update[i] remembers the last node on lane i that
 * is < key: the new node gets spliced in right after it on every lane it joins.
 */
static bool sl_insert(SkipList *s, int key)
{
    SLNode *update[MAX_LEVEL];
    SLNode *x = s->head;
    for (int i = s->level - 1; i >= 0; i--) {
        while (x->next[i] && x->next[i]->key < key)
            x = x->next[i];
        update[i] = x;
    }
    if (x->next[0] && x->next[0]->key == key)
        return false;                /* no duplicates */
    int lvl = random_level();
    if (lvl > s->level) {            /* new lanes start at the head */
        for (int i = s->level; i < lvl; i++)
            update[i] = s->head;
        s->level = lvl;
    }
    SLNode *n = node_new(key, lvl);
    for (int i = 0; i < lvl; i++) {  /* ordinary linked-list splice, once per lane */
        n->next[i] = update[i]->next[i];
        update[i]->next[i] = n;
    }
    s->size++;
    return true;
}

/* Delete, expected O(log n): unlink from each lane it is on, then free it. */
static bool sl_erase(SkipList *s, int key)
{
    SLNode *update[MAX_LEVEL];
    SLNode *x = s->head;
    for (int i = s->level - 1; i >= 0; i--) {
        while (x->next[i] && x->next[i]->key < key)
            x = x->next[i];
        update[i] = x;
    }
    x = x->next[0];
    if (!x || x->key != key)
        return false;
    for (int i = 0; i < x->level; i++)
        update[i]->next[i] = x->next[i];
    free(x);
    while (s->level > 1 && !s->head->next[s->level - 1])
        s->level--;                  /* drop lanes that became empty */
    s->size--;
    return true;
}

static void sl_print(const SkipList *s)
{
    for (int i = s->level - 1; i >= 0; i--) {
        printf("  L%d HEAD", i);
        for (const SLNode *x = s->head->next[0]; x; x = x->next[0])
            if (x->level > i)
                printf(" -> %2d", x->key);
            else
                printf(" ------");
        printf(" -> NIL\n");
    }
}

/* Check: every lane is sorted, and lane i+1 is a subsequence of lane i. */
static void sl_check(const SkipList *s)
{
    int count = 0;
    for (const SLNode *x = s->head->next[0]; x; x = x->next[0]) {
        count++;
        if (x->next[0]) assert(x->key < x->next[0]->key);
    }
    assert(count == s->size);
    for (int i = 1; i < s->level; i++)
        for (const SLNode *x = s->head->next[i]; x; x = x->next[i]) {
            assert(x->level > i);
            if (x->next[i]) assert(x->key < x->next[i]->key);
        }
}

int main(void)
{
    SkipList s;
    sl_init(&s);
    int demo[] = { 9, 21, 4, 14, 1, 7, 12, 17, 25 };
    for (size_t i = 0; i < sizeof demo / sizeof demo[0]; i++)
        sl_insert(&s, demo[i]);
    printf("skip list (lanes chosen by coin flips):\n");
    sl_print(&s);
    assert(sl_contains(&s, 17) && !sl_contains(&s, 18));
    assert(!sl_insert(&s, 9)); /* duplicate rejected */
    sl_erase(&s, 9);
    printf("after erase(9):\n");
    sl_print(&s);
    sl_check(&s);
    sl_free(&s);

    /* randomised test vs a presence table */
    enum { RANGE = 5000, OPS = 50000 };
    static bool present[RANGE];
    sl_init(&s);
    for (int op = 0; op < OPS; op++) {
        int k = (int)(rnd() % RANGE);
        switch (rnd() % 3) {
        case 0: assert(sl_insert(&s, k) == !present[k]); present[k] = true; break;
        case 1: assert(sl_erase(&s, k) == present[k]); present[k] = false; break;
        default: assert(sl_contains(&s, k) == present[k]); break;
        }
        if (op % 1000 == 0)
            sl_check(&s);
    }
    sl_check(&s);
    printf("random test: %d ops, final size %d, top lane %d — matches reference\n", OPS, s.size, s.level);
    sl_free(&s);
    printf("OK\n");
    return 0;
}
