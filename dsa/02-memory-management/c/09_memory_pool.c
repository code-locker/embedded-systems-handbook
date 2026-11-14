/*
 * 09_memory_pool.c — fixed-size block pool: O(1) alloc/free, zero fragmentation
 *
 * Concept : Pre-allocate N blocks of the SAME size. Thread every free block
 *           into a singly linked free list, storing the "next" pointer INSIDE
 *           the free block itself (it is unused anyway) — no extra memory.
 *           alloc = pop the head, free = push it back. Both O(1), deterministic,
 *           no syscalls: exactly what RTOSes (FreeRTOS, Zephyr k_mem_slab),
 *           network stacks (skb caches) and the kernel's slab allocator do.
 * Build   : make run P=dsa/02-memory-management/c/09_memory_pool.c
 * Read    : ../README.md  (section "3.9 Memory pools and arenas")
 */
#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A free block is reinterpreted as this node. */
union pool_node {
    union pool_node *next;
    max_align_t _align;        /* forces the block to be aligned for anything */
};

struct pool {
    unsigned char *mem;        /* backing storage (caller-owned) */
    size_t block_size;         /* >= sizeof(union pool_node), multiple of alignment */
    size_t nblocks;
    union pool_node *free_list;
    size_t in_use, high_water;
};

static void pool_init(struct pool *p, void *mem, size_t block_size, size_t nblocks)
{
    size_t a = alignof(union pool_node);
    if (block_size < sizeof(union pool_node)) block_size = sizeof(union pool_node);
    block_size = (block_size + a - 1) / a * a;
    p->mem = mem;
    p->block_size = block_size;
    p->nblocks = nblocks;
    p->in_use = p->high_water = 0;
    /* Thread the free list through the blocks: 0 -> 1 -> 2 -> ... -> NULL */
    p->free_list = NULL;
    for (size_t i = nblocks; i-- > 0;) {
        union pool_node *n = (union pool_node *)(p->mem + i * block_size);
        n->next = p->free_list;
        p->free_list = n;
    }
}

/*
 *  free_list ──► [blk0|next●]──► [blk1|next●]──► [blk2|next●]──► NULL
 *
 *  pool_alloc():  take blk0, free_list = blk0->next        O(1)
 *  pool_free(x):  x->next = free_list, free_list = x       O(1)
 */
static void *pool_alloc(struct pool *p)
{
    union pool_node *n = p->free_list;
    if (!n)
        return NULL;                         /* exhausted: deterministic failure */
    p->free_list = n->next;
    if (++p->in_use > p->high_water) p->high_water = p->in_use;
    return n;
}

static int pool_owns(const struct pool *p, const void *ptr)
{
    const unsigned char *b = ptr;
    return b >= p->mem && b < p->mem + p->nblocks * p->block_size &&
           (size_t)(b - p->mem) % p->block_size == 0;
}

static void pool_free(struct pool *p, void *ptr)
{
    if (!ptr) return;
    assert(pool_owns(p, ptr) && "pointer does not belong to this pool");
    union pool_node *n = ptr;
    n->next = p->free_list;
    p->free_list = n;
    p->in_use--;
}

struct msg { uint32_t id; uint8_t payload[40]; };

int main(void)
{
    enum { N = 8 };
    static alignas(max_align_t) unsigned char storage[N * 64];
    struct pool pool;
    pool_init(&pool, storage, sizeof(struct msg), N);
    printf("pool: %zu blocks of %zu bytes (struct msg is %zu)\n", pool.nblocks, pool.block_size, sizeof(struct msg));

    struct msg *m[N];
    for (int i = 0; i < N; i++) {
        m[i] = pool_alloc(&pool);
        assert(m[i] && pool_owns(&pool, m[i]));
        assert((uintptr_t)m[i] % alignof(max_align_t) == 0);
        m[i]->id = (uint32_t)i;
        memset(m[i]->payload, i, sizeof m[i]->payload);
    }
    assert(pool_alloc(&pool) == NULL);                 /* full: no hidden malloc */
    printf("allocated all %d blocks; next alloc returns NULL (bounded memory)\n", N);

    pool_free(&pool, m[3]);
    struct msg *again = pool_alloc(&pool);
    assert(again == m[3]);                             /* LIFO: hot in cache */
    assert(m[2]->id == 2 && m[4]->payload[39] == 4);   /* neighbours untouched */

    for (int i = 0; i < N; i++) pool_free(&pool, m[i]);
    assert(pool.in_use == 0 && pool.high_water == N);

    /* Churn: a million alloc/free pairs, constant time each, no fragmentation. */
    for (int i = 0; i < 1000000; i++) {
        void *x = pool_alloc(&pool), *y = pool_alloc(&pool);
        assert(x && y);
        pool_free(&pool, x);
        pool_free(&pool, y);
    }
    printf("1,000,000 alloc/free pairs done; in_use=%zu high_water=%zu\n", pool.in_use, pool.high_water);
    printf("All checks passed.\n");
    return 0;
}
