/*
 * 04_object_pool.c — object pool / slab-like allocator with an intrusive free list
 *
 * Concept : When you allocate and free many objects of ONE size (packets,
 *           buffers, timers, inodes), carve big chunks ("slabs") into equal
 *           slots and keep the free slots on a singly linked list threaded
 *           THROUGH the free slots themselves. alloc = pop, free = push:
 *           O(1), no fragmentation, no per-object header, cache-hot reuse.
 *           This is the idea behind the Linux kmem_cache (SLAB/SLUB),
 *           GStreamer's GstBufferPool and every RTOS memory pool.
 * Build   : make run P=dsa/19-system-design-structures/c/04_object_pool.c
 * Read    : ../README.md  (section "Object pool / slab allocator")
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *  one slab of 6 slots, object size 32 bytes
 *
 *  +--------+--------+--------+--------+--------+--------+
 *  | USED   | free * | USED   | free * | free * | USED   |
 *  +--------+---|----+--------+---|--^-+---|--^-+--------+
 *               |                 |  |     |  |
 *  free_list ---+                 +--|-----+  |
 *               +--------->-------+  +---->---+--> NULL
 *
 *  A free slot's first bytes hold the "next free" pointer. A used slot holds
 *  user data. The same bytes, two meanings — that is why the slot size must
 *  be >= sizeof(void*) and suitably aligned.
 */
typedef struct FreeSlot { struct FreeSlot *next; } FreeSlot;

typedef struct Slab {
    struct Slab *next;                 /* list of slabs, freed at destroy */
    /* slot memory follows this header (flexible array member) */
    max_align_t data[];
} Slab;

typedef struct {
    size_t obj_size;                   /* rounded up for alignment */
    size_t per_slab;
    Slab *slabs;
    FreeSlot *free_list;
    size_t in_use, nslabs;
} Pool;

static size_t round_up(size_t n, size_t a) { return (n + a - 1) / a * a; }

static void pool_init(Pool *p, size_t obj_size, size_t per_slab)
{
    memset(p, 0, sizeof *p);
    size_t min = obj_size < sizeof(FreeSlot) ? sizeof(FreeSlot) : obj_size;
    p->obj_size = round_up(min, _Alignof(max_align_t));
    p->per_slab = per_slab;
}

/* Grow by one slab: O(per_slab) once, amortised O(1) per alloc. */
static int pool_grow(Pool *p)
{
    Slab *s = malloc(sizeof *s + p->obj_size * p->per_slab);
    if (!s) return 0;
    s->next = p->slabs;
    p->slabs = s;
    p->nslabs++;
    unsigned char *base = (unsigned char *)s->data;
    /* Push slots in reverse so the first alloc returns the lowest address. */
    for (size_t i = p->per_slab; i-- > 0; ) {
        FreeSlot *f = (FreeSlot *)(base + i * p->obj_size);
        f->next = p->free_list;
        p->free_list = f;
    }
    return 1;
}

/* O(1): pop the free list head. */
static void *pool_alloc(Pool *p)
{
    if (!p->free_list && !pool_grow(p))
        return NULL;
    FreeSlot *f = p->free_list;
    p->free_list = f->next;
    p->in_use++;
    return f;
}

/* O(1): push back. The CALLER guarantees obj came from this pool. */
static void pool_free(Pool *p, void *obj)
{
    if (!obj) return;
    FreeSlot *f = obj;
    f->next = p->free_list;            /* LIFO: the hottest cache line is reused next */
    p->free_list = f;
    p->in_use--;
}

/* Releases all slabs at once — individual objects need not be freed first.
   (Arena-style teardown: one of the big wins of pools.) */
static void pool_destroy(Pool *p)
{
    Slab *s = p->slabs;
    while (s) { Slab *n = s->next; free(s); s = n; }
    memset(p, 0, sizeof *p);
}

/* Debug helper: is ptr a slot boundary inside one of our slabs? O(slabs). */
static int pool_owns(const Pool *p, const void *ptr)
{
    for (const Slab *s = p->slabs; s; s = s->next) {
        const unsigned char *b = (const unsigned char *)s->data;
        const unsigned char *c = ptr;
        if (c >= b && c < b + p->obj_size * p->per_slab)
            return (size_t)(c - b) % p->obj_size == 0;
    }
    return 0;
}

/* A typical pooled object: a network packet descriptor. */
typedef struct {
    uint32_t id;
    uint16_t len;
    uint8_t  payload[50];
} Packet;

int main(void)
{
    Pool pool;
    pool_init(&pool, sizeof(Packet), 8);
    printf("sizeof(Packet)=%zu, slot size=%zu (aligned to %zu)\n",
           sizeof(Packet), pool.obj_size, _Alignof(max_align_t));

    printf("\n== LIFO reuse ==\n");
    Packet *a = pool_alloc(&pool), *b = pool_alloc(&pool);
    assert(a && b && a != b);
    printf("a=%p b=%p\n", (void *)a, (void *)b);
    pool_free(&pool, a);
    Packet *c = pool_alloc(&pool);
    printf("free(a) then alloc -> %p (same slot as a: still in cache)\n", (void *)c);
    assert(c == a);
    pool_free(&pool, b);
    pool_free(&pool, c);
    assert(pool.in_use == 0);

    printf("\n== random alloc/free stress, checked for overlap & integrity ==\n");
    enum { MAXLIVE = 300, OPS = 20000 };
    Packet *live[MAXLIVE];
    size_t nlive = 0;
    srand(4);
    for (int op = 0; op < OPS; op++) {
        if (nlive < MAXLIVE && (nlive == 0 || rand() % 3 != 0)) {
            Packet *pk = pool_alloc(&pool);
            assert(pk && pool_owns(&pool, pk));
            pk->id = (uint32_t)op;
            pk->len = (uint16_t)(op % 50);
            memset(pk->payload, (int)(op & 0xff), sizeof pk->payload);
            live[nlive++] = pk;
        } else {
            size_t k = (size_t)rand() % nlive;
            Packet *pk = live[k];
            /* Payload intact? If two live objects shared a slot, the second
               writer would have overwritten the first. */
            for (size_t i = 0; i < sizeof pk->payload; i++)
                assert(pk->payload[i] == (uint8_t)(pk->id & 0xff));
            pool_free(&pool, pk);
            live[k] = live[--nlive];
        }
        assert(pool.in_use == nlive);
    }
    for (size_t i = 0; i < nlive; i++)
        for (size_t j = 0; j < sizeof live[i]->payload; j++)
            assert(live[i]->payload[j] == (uint8_t)(live[i]->id & 0xff));
    printf("%d ops: %zu live objects, %zu slabs of %zu slots (capacity %zu)\n",
           OPS, nlive, pool.nslabs, pool.per_slab, pool.nslabs * pool.per_slab);
    assert(pool.nslabs * pool.per_slab >= MAXLIVE / 2);
    assert(pool.nslabs * pool.per_slab < MAXLIVE + pool.per_slab);  /* never grew past peak */

    /* Arena teardown: drop everything, live objects included. */
    pool_destroy(&pool);
    printf("pool destroyed with %zu objects still 'live' — no leak, slabs own the memory\n", nlive);
    printf("OK\n");
    return 0;
}
