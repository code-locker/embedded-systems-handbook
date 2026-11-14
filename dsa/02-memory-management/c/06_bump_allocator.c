/*
 * 06_bump_allocator.c — the simplest allocator: a bump (arena / linear) allocator
 *
 * Concept : Take one big buffer and an offset. To allocate n bytes: round
 *           the offset up to the alignment, hand out [offset, offset+n),
 *           move the offset. Free individual blocks? You can't. You free
 *           EVERYTHING at once by resetting the offset to 0 — O(1).
 *           Perfect for "per-frame" or "per-request" memory: parse a packet,
 *           process a video frame, compile a function, then reset.
 * Build   : make run P=dsa/02-memory-management/c/06_bump_allocator.c
 * Read    : ../README.md  (section "3.6 Writing allocators: bump / arena")
 */
#include <assert.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct arena {
    unsigned char *base;   /* start of the backing buffer (NOT owned: caller provides) */
    size_t cap;            /* buffer size in bytes */
    size_t off;            /* next free byte = the "bump pointer" */
    size_t peak;           /* high-water mark: how much we ever needed */
};

static void arena_init(struct arena *a, void *buf, size_t cap)
{
    a->base = buf;
    a->cap = cap;
    a->off = 0;
    a->peak = 0;
}

/* Round x up to a multiple of align (align must be a power of two).
 *   x = 13, align = 8:  (13 + 7) & ~7 = 20 & ...11111000 = 16          */
static size_t align_up(size_t x, size_t align)
{
    return (x + align - 1) & ~(align - 1);
}

/* O(1): one add, one compare. No headers, no per-block metadata at all. */
static void *arena_alloc(struct arena *a, size_t n, size_t align)
{
    assert(align && (align & (align - 1)) == 0);
    /* Align the ADDRESS, not just the offset (base itself may be odd). */
    uintptr_t cur = (uintptr_t)a->base + a->off;
    uintptr_t aligned = align_up(cur, align);
    size_t new_off = (size_t)(aligned - (uintptr_t)a->base) + n;
    if (new_off > a->cap)
        return NULL;                      /* out of arena: caller decides */
    a->off = new_off;
    if (a->off > a->peak)
        a->peak = a->off;
    return (void *)aligned;
}

#define ARENA_NEW(a, T, count) ((T *)arena_alloc((a), sizeof(T) * (count), alignof(T)))

/* Free everything in O(1). Every pointer handed out is now invalid. */
static void arena_reset(struct arena *a) { a->off = 0; }

/* Save/restore: a "scratch" scope inside an arena (stack-like discipline). */
static size_t arena_mark(const struct arena *a) { return a->off; }
static void arena_release(struct arena *a, size_t mark) { a->off = mark; }

struct packet { uint16_t len; uint8_t type; uint64_t timestamp; };

/*
 *  base                                                             base+cap
 *  ┌──────┬──┬─────────────────┬────┬──────────────┬──────────────────────┐
 *  │ A    │p │ B (aligned 8)   │pad │ C (aligned 64)│      free            │
 *  └──────┴──┴─────────────────┴────┴──────────────┴──────────────────────┘
 *                                                   ^ off (bump pointer)
 *  p/pad = alignment padding (wasted).  reset(): off = 0, everything gone.
 */
int main(void)
{
    static alignas(64) unsigned char backing[4096];   /* could be SRAM, a DMA pool, mmap... */
    struct arena a;
    arena_init(&a, backing, sizeof backing);

    char *name = arena_alloc(&a, 5, 1);
    strcpy(name, "cam0");
    struct packet *pk = ARENA_NEW(&a, struct packet, 4);
    void *cache_line = arena_alloc(&a, 64, 64);
    assert(name && pk && cache_line);
    assert((uintptr_t)pk % alignof(struct packet) == 0);
    assert((uintptr_t)cache_line % 64 == 0);
    printf("name @+%td, packets @+%td (align %zu), cache_line @+%td (align 64), off=%zu\n",
           (unsigned char *)name - backing, (unsigned char *)pk - backing, alignof(struct packet),
           (unsigned char *)cache_line - backing, a.off);
    assert((unsigned char *)pk - backing == 8);       /* 5 bytes + 3 padding */

    /* Scratch scope: temporary work, then roll back. */
    size_t m = arena_mark(&a);
    int *tmp = ARENA_NEW(&a, int, 100);
    assert(tmp);
    for (int i = 0; i < 100; i++) tmp[i] = i;
    arena_release(&a, m);
    assert(a.off == m);

    /* Exhaustion returns NULL instead of crashing. */
    assert(arena_alloc(&a, 1 << 20, 1) == NULL);

    /* A frame loop: allocate freely per frame, reset once. No leaks possible. */
    for (int frame = 0; frame < 1000; frame++) {
        arena_reset(&a);
        int *row = ARENA_NEW(&a, int, 256);
        assert(row);
        row[255] = frame;
    }
    printf("1000 frames processed, peak arena use = %zu of %zu bytes, 0 frees called\n", a.peak, a.cap);
    assert(a.peak <= sizeof backing);

    printf("All checks passed.\n");
    return 0;
}
