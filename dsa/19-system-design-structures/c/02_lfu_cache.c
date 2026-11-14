/*
 * 02_lfu_cache.c — LFU cache in O(1): list of frequency buckets, each a list of items
 *
 * Concept : Evict the entry used the FEWEST times (ties broken by least
 *           recently used). The O(1) trick (Shah, Mitra, Matani 2010): keep
 *           a sorted doubly linked list of frequency buckets; a touched item
 *           only ever moves from bucket f to bucket f+1, which is either the
 *           next bucket or a new one inserted right after — never a search.
 * Build   : make run P=dsa/19-system-design-structures/c/02_lfu_cache.c
 * Read    : ../README.md  (section "LFU cache")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *  freq list (ascending)   items inside a bucket (front = most recent)
 *
 *  [f=1] ----> [k=7] <-> [k=2]          <- evict from here: min freq, LRU (k=2)
 *    |
 *  [f=2] ----> [k=4]
 *    |
 *  [f=5] ----> [k=9] <-> [k=1]
 *
 *  touch(k=4): f=2 -> needs f=3. Next bucket is f=5, so create [f=3]
 *  between them, move k=4 there, and free [f=2] because it became empty.
 */
typedef struct FreqNode FreqNode;
typedef struct Item {
    int key, value;
    FreqNode *parent;              /* which bucket I am in */
    struct Item *prev, *next;      /* list inside the bucket */
    struct Item *hnext;            /* hash chain */
} Item;

struct FreqNode {
    unsigned long freq;
    Item *first, *last;
    FreqNode *prev, *next;
};

typedef struct {
    size_t capacity, size, nbuckets;
    Item **map;
    FreqNode *lowest;              /* head of freq list = minimum frequency */
} LFU;

static size_t hb(const LFU *c, int key) { return ((unsigned)key * 2654435761u) % c->nbuckets; }

static void lfu_init(LFU *c, size_t cap)
{
    memset(c, 0, sizeof *c);
    c->capacity = cap;
    c->nbuckets = 2 * cap + 1;
    c->map = calloc(c->nbuckets, sizeof *c->map);
    assert(c->map);
}

static Item *map_find(const LFU *c, int key)
{
    for (Item *it = c->map[hb(c, key)]; it; it = it->hnext)
        if (it->key == key) return it;
    return NULL;
}

static void map_remove(LFU *c, Item *it)
{
    Item **pp = &c->map[hb(c, it->key)];
    while (*pp != it) pp = &(*pp)->hnext;
    *pp = it->hnext;
}

/* --- item list inside one bucket ---------------------------------------- */
static void bucket_push_front(FreqNode *f, Item *it)
{
    it->parent = f;
    it->prev = NULL;
    it->next = f->first;
    if (f->first) f->first->prev = it; else f->last = it;
    f->first = it;
}

static void bucket_unlink(FreqNode *f, Item *it)
{
    if (it->prev) it->prev->next = it->next; else f->first = it->next;
    if (it->next) it->next->prev = it->prev; else f->last = it->prev;
}

/* --- frequency list ------------------------------------------------------ */
/* New bucket with frequency `freq`, inserted after `after` (NULL = at head). */
static FreqNode *freq_insert_after(LFU *c, FreqNode *after, unsigned long freq)
{
    FreqNode *f = calloc(1, sizeof *f);
    assert(f);
    f->freq = freq;
    f->prev = after;
    f->next = after ? after->next : c->lowest;
    if (f->next) f->next->prev = f;
    if (after) after->next = f; else c->lowest = f;
    return f;
}

static void freq_remove_if_empty(LFU *c, FreqNode *f)
{
    if (f->first) return;
    if (f->prev) f->prev->next = f->next; else c->lowest = f->next;
    if (f->next) f->next->prev = f->prev;
    free(f);
}

/* O(1): move item from bucket f to bucket f+1. */
static void touch(LFU *c, Item *it)
{
    FreqNode *cur = it->parent;
    FreqNode *nxt = cur->next;
    if (!nxt || nxt->freq != cur->freq + 1)
        nxt = freq_insert_after(c, cur, cur->freq + 1);
    bucket_unlink(cur, it);
    bucket_push_front(nxt, it);
    freq_remove_if_empty(c, cur);   /* must happen AFTER we used cur->next */
}

static int lfu_get(LFU *c, int key, int *out)
{
    Item *it = map_find(c, key);
    if (!it) return 0;
    touch(c, it);
    *out = it->value;
    return 1;
}

static void lfu_put(LFU *c, int key, int value)
{
    if (c->capacity == 0) return;
    Item *it = map_find(c, key);
    if (it) { it->value = value; touch(c, it); return; }

    if (c->size == c->capacity) {
        /* Victim: lowest-frequency bucket, least recent item = its tail. */
        FreqNode *f = c->lowest;
        Item *victim = f->last;
        bucket_unlink(f, victim);
        map_remove(c, victim);
        free(victim);
        freq_remove_if_empty(c, f);
        c->size--;
    }
    it = calloc(1, sizeof *it);
    assert(it);
    it->key = key;
    it->value = value;
    size_t b = hb(c, key);
    it->hnext = c->map[b];
    c->map[b] = it;
    FreqNode *one = c->lowest;
    if (!one || one->freq != 1)
        one = freq_insert_after(c, NULL, 1);   /* new items always have freq 1 */
    bucket_push_front(one, it);
    c->size++;
}

static void lfu_free(LFU *c)
{
    FreqNode *f = c->lowest;
    while (f) {
        Item *it = f->first;
        while (it) { Item *n = it->next; free(it); it = n; }
        FreqNode *nf = f->next;
        free(f);
        f = nf;
    }
    free(c->map);
}

static void lfu_print(const LFU *c)
{
    for (const FreqNode *f = c->lowest; f; f = f->next) {
        printf("  [f=%lu]", f->freq);
        for (const Item *it = f->first; it; it = it->next) printf(" %d", it->key);
        printf("\n");
    }
}

/* ---- brute-force reference --------------------------------------------- */
typedef struct { int key, value, used; unsigned long freq, stamp; } Slot;

static int ref_get(Slot *s, size_t cap, unsigned long *clk, int key, int *out)
{
    for (size_t i = 0; i < cap; i++)
        if (s[i].used && s[i].key == key) {
            s[i].freq++; s[i].stamp = ++*clk; *out = s[i].value; return 1;
        }
    return 0;
}

static void ref_put(Slot *s, size_t cap, unsigned long *clk, int key, int value)
{
    size_t victim = cap, freeslot = cap;
    for (size_t i = 0; i < cap; i++) {
        if (s[i].used && s[i].key == key) {
            s[i].value = value; s[i].freq++; s[i].stamp = ++*clk; return;
        }
        if (!s[i].used) { if (freeslot == cap) freeslot = i; continue; }
        if (victim == cap || s[i].freq < s[victim].freq ||
            (s[i].freq == s[victim].freq && s[i].stamp < s[victim].stamp))
            victim = i;
    }
    size_t at = freeslot != cap ? freeslot : victim;
    s[at] = (Slot){ key, value, 1, 1, ++*clk };
}

int main(void)
{
    LFU c;
    int v;
    printf("== LFU walkthrough, capacity 3 ==\n");
    lfu_init(&c, 3);
    lfu_put(&c, 1, 10);
    lfu_put(&c, 2, 20);
    lfu_put(&c, 3, 30);
    assert(lfu_get(&c, 1, &v) && v == 10);
    assert(lfu_get(&c, 1, &v));
    assert(lfu_get(&c, 2, &v));
    printf("after put 1,2,3; get 1,1,2:\n");
    lfu_print(&c);
    lfu_put(&c, 4, 40);
    printf("put(4): evicts 3 (freq 1)\n");
    lfu_print(&c);
    assert(!lfu_get(&c, 3, &v));
    lfu_put(&c, 5, 50);
    printf("put(5): evicts 4 (the only item with freq 1)\n");
    lfu_print(&c);
    assert(!lfu_get(&c, 4, &v));
    assert(lfu_get(&c, 1, &v) && v == 10);
    lfu_free(&c);

    printf("\n== randomised check against brute force ==\n");
    srand(7);
    for (int trial = 0; trial < 50; trial++) {
        size_t cap = (size_t)(rand() % 12);    /* include capacity 0 */
        lfu_init(&c, cap);
        Slot *s = calloc(cap + 1, sizeof *s);
        assert(s);
        unsigned long clk = 0;
        for (int op = 0; op < 3000; op++) {
            int key = rand() % 30;
            if (rand() % 3) {
                int a = 0, b = 0;
                int ha = lfu_get(&c, key, &a), hb2 = ref_get(s, cap, &clk, key, &b);
                assert(ha == hb2);
                if (ha) assert(a == b);
            } else {
                int val = rand();
                lfu_put(&c, key, val);
                if (cap) ref_put(s, cap, &clk, key, val);
            }
        }
        free(s);
        lfu_free(&c);
    }
    printf("50 trials x 3000 ops agree (incl. capacity 0)\n");
    printf("OK\n");
    return 0;
}
