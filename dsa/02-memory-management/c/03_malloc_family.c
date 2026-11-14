/*
 * 03_malloc_family.c — exact semantics of malloc, calloc, realloc, free, aligned_alloc
 *
 * Concept : malloc(n) gives n UNINITIALISED bytes (or NULL), aligned for any
 *           type (16 bytes on x86_64/arm64). calloc(n, sz) gives ZEROED bytes
 *           and checks n*sz for overflow. realloc may MOVE the block and
 *           returns NULL on failure while the old block stays valid.
 *           free(NULL) is a no-op; freeing anything not from malloc is UB.
 * Build   : make run P=dsa/02-memory-management/c/03_malloc_family.c
 * Read    : ../README.md  (section "3.3 malloc, calloc, realloc, free")
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <malloc.h>     /* malloc_usable_size (glibc, also provided by ASan) */
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SANITIZE_ADDRESS__)
/* By default ASan ABORTS on an impossible allocation (it assumes it is a bug).
 * We want the C-standard behaviour (return NULL) for the overflow demo below. */
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "allocator_may_return_null=1"; }
#endif

struct sample { uint32_t id; double v; };

/* The correct realloc pattern: never write p = realloc(p, n) — on failure
 * that overwrites your only pointer with NULL and leaks the block.        */
static int grow(int **arr, size_t *cap)
{
    size_t new_cap = *cap ? *cap * 2 : 4;
    int *tmp = realloc(*arr, new_cap * sizeof **arr);
    if (!tmp)
        return -1;             /* *arr is still valid and still owned by caller */
    *arr = tmp;
    *cap = new_cap;
    return 0;
}

int main(void)
{
    /* malloc: sizeof *p, never sizeof(type) repeated by hand. */
    struct sample *s = malloc(10 * sizeof *s);
    assert(s);
    assert((uintptr_t)s % alignof(max_align_t) == 0);
    printf("malloc(10 * %zu) -> %p, aligned to %zu (max_align_t)\n", sizeof *s, (void *)s,
           alignof(max_align_t));
    printf("malloc_usable_size = %zu (allocators round up)\n", malloc_usable_size(s));
    assert(malloc_usable_size(s) >= 10 * sizeof *s);
    free(s);

    /* calloc: zeroed, and it detects n * size overflow for you. */
    int *z = calloc(1000, sizeof *z);
    assert(z);
    for (int i = 0; i < 1000; i++) assert(z[i] == 0);
    free(z);
    /* The overflow check: SIZE_MAX/2 * 4 wraps around in malloc(n * size). */
    size_t huge = SIZE_MAX / 2;
    volatile size_t elem = 4;                 /* volatile: keep the compiler from warning */
    errno = 0;
    void *ovf = calloc(huge, elem);
    printf("calloc(SIZE_MAX/2, 4) -> %p (overflow detected, errno=%d ENOMEM=%d)\n", ovf, errno, ENOMEM);
    assert(ovf == NULL);

    /* realloc: grow a dynamic array. Contents preserved up to the old size. */
    int *arr = NULL;
    size_t cap = 0, len = 0;
    for (int i = 0; i < 100; i++) {
        if (len == cap)
            assert(grow(&arr, &cap) == 0);
        arr[len++] = i;
    }
    assert(cap == 128 && arr[99] == 99);
    /* Special cases: realloc(NULL, n) == malloc(n). Shrinking keeps the prefix. */
    int *shrunk = realloc(arr, 10 * sizeof *arr);
    assert(shrunk && shrunk[9] == 9);
    free(shrunk);
    /* realloc(p, 0): implementation-defined (glibc frees and returns NULL; C23
     * made it UB). Never use it — call free() instead.                       */

    /* aligned_alloc / posix_memalign: for DMA buffers, SIMD, cache-line alignment. */
    void *dma = aligned_alloc(4096, 8192);        /* size must be a multiple of alignment */
    assert(dma && (uintptr_t)dma % 4096 == 0);
    printf("aligned_alloc(4096, 8192) -> %p (page-aligned)\n", dma);
    free(dma);                                    /* ordinary free() */
    void *cl = NULL;
    assert(posix_memalign(&cl, 64, 100) == 0 && (uintptr_t)cl % 64 == 0);
    free(cl);

    /* malloc(0): returns NULL or a unique pointer you must free. Do not deref. */
    void *zero = malloc(0);
    free(zero);
    free(NULL);                                   /* always a no-op */

    /* malloc does not zero memory. memset or calloc if you need zeros. */
    char *buf = malloc(32);
    assert(buf);
    memset(buf, 0, 32);
    strcpy(buf, "owned by main, freed by main");
    printf("%s\n", buf);
    free(buf);

    printf("All checks passed.\n");
    return 0;
}
