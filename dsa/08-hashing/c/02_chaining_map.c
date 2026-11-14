/*
 * 02_chaining_map.c — separate-chaining hash map (string -> int) with rehash
 *
 * Concept : An array of buckets; each bucket is a singly linked list of
 *           entries whose keys hashed there. Put/get/delete are O(1) on
 *           average while the load factor (n / buckets) stays bounded, so
 *           the table doubles and re-distributes when n / buckets > 0.75.
 * Build   : make run P=dsa/08-hashing/c/02_chaining_map.c
 * Read    : ../README.md  (section "3.3 Separate chaining")
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                    __LINE__);                                                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/*
 * Memory picture (4 buckets, 3 entries; "b" and "z" collided in bucket 2):
 *
 *   map.buckets ──► [0] NULL
 *                   [1] ──► {"a",1,next}──► NULL
 *                   [2] ──► {"z",26,next}──► {"b",2,next}──► NULL
 *                   [3] NULL
 *
 * Ownership: the map owns every entry AND a private copy of every key
 * (strdup-like). Callers' strings may die right after map_put().
 */
struct entry {
    char *key;           /* owned copy */
    int value;
    uint32_t hash;       /* cached: rehash never recomputes, compare is cheap */
    struct entry *next;
};

struct map {
    struct entry **buckets; /* array of list heads, owned */
    size_t nbuckets;        /* always a power of two -> index = hash & (n-1) */
    size_t size;            /* number of entries */
};

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h;
}

static char *dup_str(const char *s) /* strdup is POSIX, not C11 */
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    CHECK(p != NULL);
    memcpy(p, s, n);
    return p;
}

static void map_init(struct map *m, size_t nbuckets)
{
    m->nbuckets = nbuckets;
    m->size = 0;
    m->buckets = calloc(nbuckets, sizeof *m->buckets); /* all NULL */
    CHECK(m->buckets != NULL);
}

static void map_free(struct map *m) /* O(n + buckets) */
{
    for (size_t b = 0; b < m->nbuckets; b++) {
        struct entry *e = m->buckets[b];
        while (e) {
            struct entry *next = e->next; /* save BEFORE free */
            free(e->key);
            free(e);
            e = next;
        }
    }
    free(m->buckets);
    m->buckets = NULL;
    m->size = m->nbuckets = 0;
}

/*
 * Rehash: allocate 2x buckets and MOVE every node (no new mallocs; only
 * the next pointers change). O(n). Amortised over the n/2 inserts since the
 * last resize this is O(1) per insert — same argument as vector growth.
 *
 *   old [2]: {"z"}──►{"b"}──►NULL
 *   new: "z".hash & 7 = 6, "b".hash & 7 = 2
 *   new [2]: {"b"}──►NULL       new [6]: {"z"}──►NULL
 */
static void map_rehash(struct map *m, size_t new_n)
{
    struct entry **nb = calloc(new_n, sizeof *nb);
    CHECK(nb != NULL);
    for (size_t b = 0; b < m->nbuckets; b++) {
        struct entry *e = m->buckets[b];
        while (e) {
            struct entry *next = e->next;
            size_t i = e->hash & (new_n - 1);
            e->next = nb[i]; /* push at head of new chain: O(1) */
            nb[i] = e;
            e = next;
        }
    }
    free(m->buckets);
    m->buckets = nb;
    m->nbuckets = new_n;
}

/* Insert or overwrite. Average O(1), worst O(n) (all keys in one chain). */
static void map_put(struct map *m, const char *key, int value)
{
    uint32_t h = fnv1a(key);
    size_t i = h & (m->nbuckets - 1);
    for (struct entry *e = m->buckets[i]; e; e = e->next) {
        /* compare cached hash first: a 4-byte compare filters most misses
         * before the expensive strcmp */
        if (e->hash == h && strcmp(e->key, key) == 0) {
            e->value = value; /* existing key: update in place */
            return;
        }
    }
    /*
     * New key: push at head of the chain (O(1), no tail walk needed).
     *
     *   before: [i] ──► A ──► B
     *   after : [i] ──► NEW ──► A ──► B
     */
    struct entry *e = malloc(sizeof *e);
    CHECK(e != NULL);
    e->key = dup_str(key);
    e->value = value;
    e->hash = h;
    e->next = m->buckets[i];
    m->buckets[i] = e;
    m->size++;
    /* load factor > 0.75 -> grow. Integer form avoids floating point:
     * size/nbuckets > 3/4  <=>  4*size > 3*nbuckets */
    if (4 * m->size > 3 * m->nbuckets) map_rehash(m, m->nbuckets * 2);
}

/* Returns pointer to the value or NULL. Average O(1). */
static int *map_get(const struct map *m, const char *key)
{
    uint32_t h = fnv1a(key);
    for (struct entry *e = m->buckets[h & (m->nbuckets - 1)]; e; e = e->next)
        if (e->hash == h && strcmp(e->key, key) == 0) return &e->value;
    return NULL;
}

/*
 * Delete with the pointer-to-pointer trick: `pp` points at the link that
 * points at the current node (either the bucket head or some e->next), so
 * unlinking the head and unlinking a middle node are the SAME code.
 *
 *   pp ──► [i] ──► A ──► B ──► C        delete B:
 *                  pp ──► A.next ──► B
 *   *pp = B->next:   [i] ──► A ──► C     then free(B)
 */
static int map_del(struct map *m, const char *key)
{
    uint32_t h = fnv1a(key);
    struct entry **pp = &m->buckets[h & (m->nbuckets - 1)];
    for (; *pp; pp = &(*pp)->next) {
        struct entry *e = *pp;
        if (e->hash == h && strcmp(e->key, key) == 0) {
            *pp = e->next;
            free(e->key);
            free(e);
            m->size--;
            return 1;
        }
    }
    return 0;
}

static size_t longest_chain(const struct map *m)
{
    size_t best = 0;
    for (size_t b = 0; b < m->nbuckets; b++) {
        size_t len = 0;
        for (struct entry *e = m->buckets[b]; e; e = e->next) len++;
        if (len > best) best = len;
    }
    return best;
}

int main(void)
{
    struct map m;
    map_init(&m, 4);

    map_put(&m, "uart0", 115200);
    map_put(&m, "uart1", 9600);
    map_put(&m, "i2c0", 400000);
    CHECK(m.size == 3 && m.nbuckets == 4); /* 3/4 is not > 0.75: no grow */
    map_put(&m, "spi0", 48000000);
    CHECK(m.nbuckets == 8);                /* 4/4 > 0.75 -> doubled */

    CHECK(*map_get(&m, "uart0") == 115200);
    CHECK(*map_get(&m, "spi0") == 48000000);
    CHECK(map_get(&m, "can0") == NULL);

    map_put(&m, "uart1", 921600); /* overwrite, size unchanged */
    CHECK(m.size == 4 && *map_get(&m, "uart1") == 921600);

    CHECK(map_del(&m, "uart0") == 1);
    CHECK(map_del(&m, "uart0") == 0);
    CHECK(map_get(&m, "uart0") == NULL && m.size == 3);

    /* Stress: 10k keys. Load factor stays <= 0.75, chains stay tiny. */
    char buf[32];
    for (int i = 0; i < 10000; i++) {
        snprintf(buf, sizeof buf, "key%d", i);
        map_put(&m, buf, i);
    }
    for (int i = 0; i < 10000; i++) {
        snprintf(buf, sizeof buf, "key%d", i);
        int *v = map_get(&m, buf);
        CHECK(v && *v == i);
    }
    for (int i = 0; i < 10000; i += 2) {
        snprintf(buf, sizeof buf, "key%d", i);
        CHECK(map_del(&m, buf));
    }
    CHECK(m.size == 3 + 5000);
    printf("size=%zu buckets=%zu load=%.2f longest chain=%zu\n", m.size,
           m.nbuckets, (double)m.size / (double)m.nbuckets, longest_chain(&m));
    CHECK(4 * m.size <= 3 * m.nbuckets);
    CHECK(longest_chain(&m) < 12);

    map_free(&m);
    printf("all chaining-map checks passed\n");
    return 0;
}
