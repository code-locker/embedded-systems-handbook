/*
 * 07_freelist_allocator.c — a real malloc/free in ~150 lines: headers, first fit, split, coalesce
 *
 * Concept : Every block carries a small HEADER (its size + a free flag) just
 *           before the pointer the user gets. Free blocks are linked into a
 *           FREE LIST. alloc = walk the list for the first block big enough
 *           (first fit), SPLIT off the rest. free = mark free, MERGE with free
 *           neighbours (coalescing) so fragmentation does not grow forever.
 *           glibc's malloc is the industrial version of exactly this.
 * Build   : make run P=dsa/02-memory-management/c/07_freelist_allocator.c
 * Read    : ../README.md  (section "3.6 Writing allocators: free list" and "3.7 Fragmentation")
 */
#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HEAP_SIZE (64 * 1024)
#define ALIGN 16

/* Header in front of every block (free or used). 32 bytes, keeps 16-alignment. */
struct block {
    size_t size;              /* payload bytes (excluding this header) */
    bool free;
    struct block *next;       /* next block in ADDRESS order (all blocks) */
    struct block *prev;       /* previous block in address order */
};
#define HDR (sizeof(struct block))
_Static_assert(sizeof(struct block) % ALIGN == 0, "header must preserve alignment");

static alignas(ALIGN) unsigned char heap[HEAP_SIZE];
static struct block *first;

/*
 *  heap[]:  one contiguous region, blocks laid end to end
 *
 *  ┌─────┬──────────┬─────┬──────┬─────┬────────────────────────────┐
 *  │ hdr │ used 64  │ hdr │free 32│ hdr │ free (rest of heap)        │
 *  └─────┴──────────┴─────┴──────┴─────┴────────────────────────────┘
 *    ▲ next/prev link headers in address order, so free() can find and
 *      merge its physical neighbours in O(1).
 *  (Simplification: we walk ALL blocks for first-fit; a real allocator keeps
 *   a separate list of FREE blocks only, or many lists by size = "bins".)
 */
static void heap_init(void)
{
    first = (struct block *)heap;
    first->size = HEAP_SIZE - HDR;
    first->free = true;
    first->next = first->prev = NULL;
}

static size_t round_up(size_t n) { return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1); }

/* Split b so that it keeps exactly 'size' bytes; the remainder becomes a new
 * free block — but only if the remainder can hold a header + 16 bytes.     */
static void split(struct block *b, size_t size)
{
    if (b->size < size + HDR + ALIGN)
        return;                                   /* too small to split: give it all */
    struct block *rest = (struct block *)((unsigned char *)(b + 1) + size);
    rest->size = b->size - size - HDR;
    rest->free = true;
    rest->next = b->next;
    rest->prev = b;
    if (b->next) b->next->prev = rest;
    b->next = rest;
    b->size = size;
}

/* O(number of blocks): first fit. */
static void *my_malloc(size_t n)
{
    if (n == 0) return NULL;
    size_t size = round_up(n);
    for (struct block *b = first; b; b = b->next)
        if (b->free && b->size >= size) {
            split(b, size);
            b->free = false;
            return b + 1;              /* user pointer = just past the header */
        }
    return NULL;                       /* no block big enough (maybe fragmented!) */
}

/* Merge b with b->next if both free. */
static void merge_next(struct block *b)
{
    struct block *n = b->next;
    if (n && n->free && b->free) {
        b->size += HDR + n->size;      /* absorb the neighbour AND its header */
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
}

/* O(1): mark free, coalesce with both physical neighbours.
 *
 *   before:  [free A][hdr|USED B][free C]       free(B)
 *   after :  [free A + hdr + B + hdr + C ]      one big block
 */
static void my_free(void *p)
{
    if (!p) return;
    struct block *b = (struct block *)p - 1;   /* step back over the header */
    assert(!b->free && "double free detected");
    b->free = true;
    merge_next(b);                     /* with the following block */
    if (b->prev && b->prev->free)
        merge_next(b->prev);           /* with the preceding block */
}

static void stats(const char *label, size_t *nblocks, size_t *largest_free, size_t *total_free)
{
    *nblocks = *largest_free = *total_free = 0;
    for (struct block *b = first; b; b = b->next) {
        (*nblocks)++;
        if (b->free) {
            *total_free += b->size;
            if (b->size > *largest_free) *largest_free = b->size;
        }
    }
    printf("  %-34s blocks=%3zu  free total=%6zu  largest free=%6zu\n", label, *nblocks, *total_free, *largest_free);
}

int main(void)
{
    heap_init();
    size_t nb, largest, total;
    stats("fresh heap", &nb, &largest, &total);
    assert(nb == 1 && largest == HEAP_SIZE - HDR);

    char *a = my_malloc(10), *b = my_malloc(100), *c = my_malloc(1000);
    assert(a && b && c);
    assert((uintptr_t)a % ALIGN == 0 && (uintptr_t)b % ALIGN == 0);
    assert(b == a + 16 + HDR);        /* 10 rounds to 16, then the next header */
    memset(a, 'a', 10); memset(b, 'b', 100); memset(c, 'c', 1000);
    stats("after a(10) b(100) c(1000)", &nb, &largest, &total);

    my_free(b);
    char *b2 = my_malloc(50);          /* first fit reuses b's hole, splits it */
    assert(b2 == b);
    stats("free(b), malloc(50) reuses its hole", &nb, &largest, &total);
    my_free(b2);
    my_free(a);
    my_free(c);
    stats("free everything -> coalesced", &nb, &largest, &total);
    assert(nb == 1 && largest == HEAP_SIZE - HDR);   /* back to ONE block: no fragmentation */

    /* Fragmentation demo: allocate 100 x 256, free every OTHER one. */
    printf("\nFragmentation:\n");
    void *ptrs[100];
    for (int i = 0; i < 100; i++) ptrs[i] = my_malloc(256);
    for (int i = 0; i < 100; i += 2) my_free(ptrs[i]);
    stats("100 x 256, every 2nd freed", &nb, &largest, &total);
    void *big = my_malloc(4096);
    /* Plenty of free bytes in total, but each hole is only 256: a 4096 request
     * must come from the untouched tail of the heap.                         */
    assert(big && (unsigned char *)big > (unsigned char *)ptrs[99]);
    printf("  malloc(4096) had to use the tail: %zu bytes free in total, but holes of only %zu\n",
           total, (size_t)256);
    my_free(big);
    for (int i = 1; i < 100; i += 2) my_free(ptrs[i]);
    stats("all freed again", &nb, &largest, &total);
    assert(nb == 1);

    printf("All checks passed.\n");
    return 0;
}
