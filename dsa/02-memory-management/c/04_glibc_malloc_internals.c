/*
 * 04_glibc_malloc_internals.c — look inside glibc malloc (ptmalloc2): chunks, bins, tcache, mmap
 *
 * Concept : glibc's malloc carves memory into CHUNKS. Each chunk has a size
 *           header right BEFORE the pointer you get; the low 3 bits of that
 *           size are flags (P = previous chunk in use, M = mmapped, A = not
 *           main arena). Freed chunks go to per-thread tcache bins (LIFO),
 *           then fast/small/large/unsorted bins; neighbours coalesce. Small
 *           requests come from the brk [heap]; requests >= 128 KiB (the
 *           M_MMAP_THRESHOLD) get their own mmap.
 *
 *  !! GLIBC-SPECIFIC AND EDUCATIONAL ONLY. Reading the bytes before a malloc'd
 *  !! pointer is outside the C standard; real code must never depend on it.
 *  !! It is read-only peeking, nothing is modified.
 *
 * Build   : The full demo needs glibc's allocator, i.e. NO AddressSanitizer
 *           (ASan replaces malloc and puts poisoned redzones before every block):
 *             make SAN=0 run P=dsa/02-memory-management/c/04_glibc_malloc_internals.c
 *           The default (ASan) build runs a reduced, allocator-neutral version:
 *             make run P=dsa/02-memory-management/c/04_glibc_malloc_internals.c
 * Read    : ../README.md  (section "3.4 Inside glibc malloc")
 */
#define _GNU_SOURCE
#include <assert.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#else
#define HAVE_ASAN 0
#endif

#define FLAG_P 0x1u   /* PREV_INUSE      */
#define FLAG_M 0x2u   /* IS_MMAPPED      */
#define FLAG_A 0x4u   /* NON_MAIN_ARENA  */

/* Find the /proc/self/maps region name containing addr. */
static void region_of(const void *addr, char *out, size_t cap)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    snprintf(out, cap, "?");
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char name[256] = "";
        int n = sscanf(line, "%lx-%lx %*s %*s %*s %*s %255[^\n]", &lo, &hi, name);
        if (n >= 2 && (uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            snprintf(out, cap, "%s", n == 3 ? name : "(anonymous mmap)");
            break;
        }
    }
    if (f) fclose(f);
}

#if !HAVE_ASAN
/*  Chunk layout (64-bit glibc):
 *
 *   chunk ──► +-------------------------------+
 *             | prev_size (only meaningful if |  8 bytes: size of the previous chunk
 *             |   the previous chunk is FREE) |           when it is free (else its data)
 *             +-------------------------------+
 *             | size            |  A | M | P |  8 bytes: chunk size, multiple of 16
 *   mem   ──► +-------------------------------+  <── pointer returned by malloc = chunk + 16
 *             | user data ...                 |
 *             |  (when FREE: fd, bk pointers, |
 *             |   tcache 'next' + 'key')      |
 *   next  ──► +-------------------------------+  next chunk's prev_size field: usable by
 *             | ...                           |  THIS chunk while it is allocated (+8 bytes)
 */
static size_t chunk_size_field(const void *mem)
{
    size_t v;
    memcpy(&v, (const char *)mem - sizeof(size_t), sizeof v);   /* read-only peek */
    return v;
}

static void peek(const char *what, const void *mem)
{
    size_t raw = chunk_size_field(mem);
    printf("  %-22s mem=%p  size field=0x%-6zx -> chunk %6zu bytes  flags[%c%c%c]  usable %zu\n",
           what, mem, raw, raw & ~(size_t)7,
           raw & FLAG_A ? 'A' : '-', raw & FLAG_M ? 'M' : '-', raw & FLAG_P ? 'P' : '-',
           malloc_usable_size((void *)mem));
}

/* At -O2 gcc may delete a malloc/free pair whose memory is never used, which
 * would ruin the address experiments. M() makes every block "escape".     */
static void *volatile g_sink;
static __attribute__((noinline)) void *M(size_t n)
{
    void *p = malloc(n);
    g_sink = p;
    return p;
}

static void full_demo(void)
{
    printf("1. Chunk headers and size rounding (request -> chunk = max(32, align16(req + 8))):\n");
    size_t reqs[] = {0, 1, 24, 25, 40, 100, 1000};
    void *blocks[7];
    for (int i = 0; i < 7; i++) {
        blocks[i] = M(reqs[i]);
        char label[32];
        snprintf(label, sizeof label, "malloc(%zu)", reqs[i]);
        peek(label, blocks[i]);
        size_t chunk = chunk_size_field(blocks[i]) & ~(size_t)7;
        size_t expect = reqs[i] + 8 < 32 ? 32 : (reqs[i] + 8 + 15) & ~(size_t)15;
        assert(chunk == expect);
        assert(malloc_usable_size(blocks[i]) == chunk - 8);   /* borrows next prev_size */
        assert(((uintptr_t)blocks[i] & 15) == 0);
    }
    for (int i = 0; i < 7; i++)
        free(blocks[i]);

    printf("\n2. tcache: free then malloc of the SAME size returns the SAME address (LIFO):\n");
    void *a = M(40);
    uintptr_t a_addr = (uintptr_t)a;
    free(a);                                   /* -> tcache bin for 48-byte chunks */
    void *b = M(40);                      /* <- popped from the same bin */
    printf("  malloc(40)=%#lx, free, malloc(40)=%p -> %s\n", (unsigned long)a_addr, b,
           (uintptr_t)b == a_addr ? "same block (tcache hit)" : "different");
    assert((uintptr_t)b == a_addr);
    void *x = M(40), *y = M(40);
    uintptr_t xa = (uintptr_t)x, ya = (uintptr_t)y;
    free(x);
    free(y);                                   /* bin: y -> x */
    void *r1 = M(40), *r2 = M(40);
    assert((uintptr_t)r1 == ya && (uintptr_t)r2 == xa);   /* last freed, first reused */
    printf("  freed x then y; next two mallocs return y then x (stack order)\n");
    free(r1); free(r2); free(b);

    printf("\n3. Coalescing: two adjacent freed 2000-byte chunks merge into one:\n");
    void *c1 = M(2000), *c2 = M(2000), *fence = M(16);  /* fence: keeps top away */
    uintptr_t c1a = (uintptr_t)c1;
    peek("c1 = M(2000)", c1);
    peek("c2 = M(2000)", c2);
    free(c1);       /* too big for tcache (max 1032 bytes) -> unsorted bin */
    free(c2);       /* its neighbour c1 is free -> merged into one 4032-byte chunk */
    void *c3 = M(4000);
    printf("  after free(c1), free(c2): malloc(4000) = %p (%s c1)\n", c3,
           (uintptr_t)c3 == c1a ? "==" : "!=");
    assert((uintptr_t)c3 == c1a);
    free(c3); free(fence);

    printf("\n4. Large requests come from mmap, not the heap:\n");
    void *brk_before = sbrk(0);
    void *big = M(1 << 20);               /* 1 MiB >= 128 KiB threshold */
    char where[256];
    region_of(big, where, sizeof where);
    peek("malloc(1 MiB)", big);
    printf("  region: %s, sbrk(0) unchanged: %s\n", where, sbrk(0) == brk_before ? "yes" : "no");
    assert(chunk_size_field(big) & FLAG_M);
    assert(strcmp(where, "[heap]") != 0);
    assert(malloc_usable_size(big) == (chunk_size_field(big) & ~(size_t)7) - 16);
    void *small = M(64);
    region_of(small, where, sizeof where);
    printf("  malloc(64) region: %s\n", where);
    assert(strcmp(where, "[heap]") == 0);
    free(small);
    free(big);                                  /* munmap: memory really returns to the OS */

    printf("\n5. The heap grows with brk, and mostly does NOT shrink on free:\n");
    enum { N = 400 };
    static void *blk[N];
    char *top0 = sbrk(0);
    for (int i = 0; i < N; i++) blk[i] = M(2000);          /* ~800 KiB, all from brk */
    char *top1 = sbrk(0);
    for (int i = 0; i < N - 1; i++) free(blk[i]);               /* keep the LAST one */
    char *top2 = sbrk(0);
    printf("  sbrk(0): start %p, after 400 x malloc(2000) +%ld KiB, after freeing all but\n"
           "  the topmost block +%ld KiB (the live top block pins the heap end)\n",
           (void *)top0, (long)(top1 - top0) / 1024, (long)(top2 - top0) / 1024);
    assert(top1 > top0 && top2 == top1);
    int trimmed = malloc_trim(0);              /* madvise(MADV_DONTNEED) free pages inside the heap */
    printf("  malloc_trim(0) released memory back to the OS: %s\n", trimmed ? "yes" : "no");
    free(blk[N - 1]);
    malloc_trim(0);
    printf("  after freeing the top block + malloc_trim: heap end moved %ld KiB vs start\n",
           (long)((char *)sbrk(0) - top0) / 1024);   /* negative = shrunk below where we began */

    printf("\n6. mallopt: lower the mmap threshold to 64 KiB:\n");
    assert(mallopt(M_MMAP_THRESHOLD, 64 * 1024) == 1);
    void *mid = M(100 * 1024);
    peek("malloc(100 KiB)", mid);
    assert(chunk_size_field(mid) & FLAG_M);
    free(mid);

    printf("\n7. Statistics: mallinfo2() and malloc_stats():\n");
    void *keep1 = M(5000), *keep2 = M(300 * 1024);
    struct mallinfo2 mi = mallinfo2();
    printf("  arena (brk bytes)   = %zu\n  hblkhd (mmap bytes) = %zu\n"
           "  uordblks (in use)   = %zu\n  fordblks (free)     = %zu\n",
           mi.arena, mi.hblkhd, mi.uordblks, mi.fordblks);
    assert(mi.hblkhd >= 300 * 1024 && mi.uordblks >= 5000);
    fflush(stdout);
    malloc_stats();                            /* prints per-arena totals to stderr */
    free(keep1);
    free(keep2);
}
#endif /* !HAVE_ASAN */

#if HAVE_ASAN
static void neutral_demo(void)
{
    /* What we can say about ANY allocator, including ASan's. */
    void *p = malloc(25);
    assert(p && ((uintptr_t)p & 15) == 0);
    printf("  malloc(25): usable size %zu (>= 25; glibc would say 40)\n", malloc_usable_size(p));
    assert(malloc_usable_size(p) >= 25);
    char where[256];
    region_of(p, where, sizeof where);
    printf("  region: %s  (ASan's allocator, not glibc's [heap])\n", where);
    free(p);
    /* ASan QUARANTINES freed blocks (so use-after-free is detectable), which
     * is exactly why the tcache "same address" trick does not happen here.  */
    void *a = malloc(40);
    uintptr_t aa = (uintptr_t)a;
    free(a);
    void *b = malloc(40);
    printf("  free(a); malloc(40) again: %s (quarantine)\n", (uintptr_t)b == aa ? "same" : "different address");
    free(b);
}
#endif

int main(void)
{
#if HAVE_ASAN
    printf("Built with AddressSanitizer: malloc is ASan's allocator, so glibc chunk\n"
           "headers cannot be inspected. Running the allocator-neutral part only.\n"
           "For the full tour:\n"
           "  make SAN=0 run P=dsa/02-memory-management/c/04_glibc_malloc_internals.c\n\n");
    neutral_demo();
#else
    full_demo();
#endif
    printf("\nAll checks passed.\n");
    return 0;
}
