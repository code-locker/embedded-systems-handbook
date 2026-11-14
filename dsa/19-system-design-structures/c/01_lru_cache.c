/*
 * 01_lru_cache.c — LRU cache: hash map (chaining) + doubly linked list, O(1) get/put
 *
 * Concept : The hash map answers "where is key K?" in O(1). The doubly linked
 *           list keeps entries in recency order so the least-recently-used one
 *           is always at the tail and can be evicted in O(1). Every get/put
 *           moves the touched node to the head.
 * Build   : make run P=dsa/19-system-design-structures/c/01_lru_cache.c
 * Read    : ../README.md  (section "LRU cache")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *   hash buckets                     recency list (head = most recent)
 *   +---+                            head                         tail
 *   | 0 |-> [k=8] -> [k=3]            |                             |
 *   | 1 |-> NULL                      v                             v
 *   | 2 |-> [k=5]                   [k=5] <-> [k=8] <-> [k=3] <-> [k=1]
 *   +---+                                                      evict here
 *
 * Each node lives in TWO structures at once: a bucket chain (hnext) and the
 * recency list (prev/next). One malloc per entry, owned by the cache.
 */
typedef struct Node {
    int key, value;
    struct Node *prev, *next;  /* recency list */
    struct Node *hnext;        /* bucket chain */
} Node;

typedef struct {
    size_t capacity, size, nbuckets;
    Node **buckets;
    Node head, tail;           /* sentinels: no NULL checks when linking */
    unsigned long hits, misses, evictions;
} LRU;

static size_t bucket_of(const LRU *c, int key)
{
    /* Multiplicative (Knuth) hash spreads consecutive keys across buckets. */
    return ((unsigned)key * 2654435761u) % c->nbuckets;
}

static void lru_init(LRU *c, size_t capacity)
{
    memset(c, 0, sizeof *c);
    c->capacity = capacity;
    c->nbuckets = capacity * 2 + 1;           /* load factor <= 0.5 */
    c->buckets = calloc(c->nbuckets, sizeof *c->buckets);
    assert(c->buckets);
    c->head.next = &c->tail;
    c->tail.prev = &c->head;
}

/* O(1): unlink from recency list. */
static void list_remove(Node *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
}

/* O(1): insert right after the head sentinel (= most recently used). */
static void list_push_front(LRU *c, Node *n)
{
    n->next = c->head.next;
    n->prev = &c->head;
    c->head.next->prev = n;
    c->head.next = n;
}

/* O(1) average: walk one short chain. */
static Node *map_find(const LRU *c, int key)
{
    for (Node *n = c->buckets[bucket_of(c, key)]; n; n = n->hnext)
        if (n->key == key)
            return n;
    return NULL;
}

static void map_remove(LRU *c, Node *victim)
{
    Node **pp = &c->buckets[bucket_of(c, victim->key)];
    while (*pp != victim)          /* pointer-to-pointer: no special case for the first node */
        pp = &(*pp)->hnext;
    *pp = victim->hnext;
}

/* get: returns 1 and fills *out on hit; a hit makes the entry "most recent". */
static int lru_get(LRU *c, int key, int *out)
{
    Node *n = map_find(c, key);
    if (!n) { c->misses++; return 0; }
    list_remove(n);
    list_push_front(c, n);
    c->hits++;
    *out = n->value;
    return 1;
}

static void lru_put(LRU *c, int key, int value)
{
    Node *n = map_find(c, key);
    if (n) {                       /* update in place + touch */
        n->value = value;
        list_remove(n);
        list_push_front(c, n);
        return;
    }
    if (c->size == c->capacity) {  /* full: evict the tail (least recent) */
        Node *victim = c->tail.prev;
        list_remove(victim);
        map_remove(c, victim);
        free(victim);
        c->size--;
        c->evictions++;
    }
    n = malloc(sizeof *n);
    assert(n);
    n->key = key;
    n->value = value;
    size_t b = bucket_of(c, key);
    n->hnext = c->buckets[b];
    c->buckets[b] = n;
    list_push_front(c, n);
    c->size++;
}

static void lru_free(LRU *c)
{
    Node *n = c->head.next;
    while (n != &c->tail) {
        Node *next = n->next;
        free(n);
        n = next;
    }
    free(c->buckets);
}

static void lru_print(const LRU *c)
{
    printf("  recency (MRU -> LRU):");
    for (const Node *n = c->head.next; n != &c->tail; n = n->next)
        printf(" %d:%d", n->key, n->value);
    printf("\n");
}

/* ---- reference model: arrays + timestamps, O(capacity) per op ---------- */
typedef struct { int key, value; unsigned long stamp; int used; } Slot;
typedef struct { Slot *s; size_t cap; unsigned long clock; } Ref;

static int ref_get(Ref *r, int key, int *out)
{
    for (size_t i = 0; i < r->cap; i++)
        if (r->s[i].used && r->s[i].key == key) {
            r->s[i].stamp = ++r->clock;
            *out = r->s[i].value;
            return 1;
        }
    return 0;
}

static void ref_put(Ref *r, int key, int value)
{
    size_t oldest = 0, freeslot = r->cap;
    for (size_t i = 0; i < r->cap; i++) {
        if (r->s[i].used && r->s[i].key == key) {
            r->s[i].value = value;
            r->s[i].stamp = ++r->clock;
            return;
        }
        if (!r->s[i].used && freeslot == r->cap) freeslot = i;
        if (r->s[i].used && r->s[i].stamp < r->s[oldest].stamp) oldest = i;
    }
    size_t at = freeslot != r->cap ? freeslot : oldest;
    r->s[at] = (Slot){ key, value, ++r->clock, 1 };
}

int main(void)
{
    printf("== LRU walkthrough, capacity 3 ==\n");
    LRU c;
    lru_init(&c, 3);
    int v;
    lru_put(&c, 1, 10); lru_print(&c);
    lru_put(&c, 2, 20); lru_print(&c);
    lru_put(&c, 3, 30); lru_print(&c);
    assert(lru_get(&c, 1, &v) && v == 10);
    printf("get(1) -> 10, 1 becomes most recent\n"); lru_print(&c);
    lru_put(&c, 4, 40);
    printf("put(4): full, evict LRU = 2\n"); lru_print(&c);
    assert(!lru_get(&c, 2, &v));
    assert(lru_get(&c, 3, &v) && v == 30);
    lru_put(&c, 5, 50);
    printf("put(5): evict LRU = 1\n"); lru_print(&c);
    assert(!lru_get(&c, 1, &v));
    lru_free(&c);

    printf("\n== randomised check against a brute-force reference ==\n");
    srand(19);
    for (int trial = 0; trial < 50; trial++) {
        size_t cap = 1 + (size_t)(rand() % 16);
        lru_init(&c, cap);
        Ref r = { calloc(cap, sizeof(Slot)), cap, 0 };
        assert(r.s);
        for (int op = 0; op < 2000; op++) {
            int key = rand() % 40;
            if (rand() % 2) {
                int val = rand();
                lru_put(&c, key, val);
                ref_put(&r, key, val);
            } else {
                int a = -1, b = -1;
                int ha = lru_get(&c, key, &a), hb = ref_get(&r, key, &b);
                assert(ha == hb);
                if (ha) assert(a == b);
            }
            assert(c.size <= cap);
        }
        if (trial == 0)
            printf("trial 0: cap=%zu hits=%lu misses=%lu evictions=%lu\n",
                   cap, c.hits, c.misses, c.evictions);
        free(r.s);
        lru_free(&c);
    }
    printf("50 random trials x 2000 ops agree with reference\n");
    printf("OK\n");
    return 0;
}
