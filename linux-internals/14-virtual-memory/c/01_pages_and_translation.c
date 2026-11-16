/*
 * 01_pages_and_translation.c — virtual addresses, pages, the 4-level page
 *                              table walk, /proc/self/pagemap, and the
 *                              cost of TLB misses
 *
 * Concept : Every address your program uses is VIRTUAL. The MMU translates
 *           it to a physical address one PAGE (4 KiB here; 4/16/64 KiB on
 *           arm64) at a time by walking a tree of page tables: on x86-64 and
 *           on arm64 with 4 KiB pages and 48-bit addresses, 4 levels of 512
 *           entries each, indexed by 9 bits of the address per level. The
 *           TLB caches recent translations; a miss costs a table walk (up
 *           to 4 memory reads). /proc/self/pagemap lets a process see, per
 *           virtual page, whether it is present in RAM and (with
 *           CAP_SYS_ADMIN only) which physical frame (PFN) backs it.
 * Build   : make run P=linux-internals/14-virtual-memory/c/01_pages_and_translation.c
 * Read    : ../README.md  (sections "3.1 Virtual and physical addresses",
 *           "3.2 Page tables" and "3.3 The TLB")
 */
#define _GNU_SOURCE /* MAP_ANONYMOUS, MADV_NOHUGEPAGE, getauxval */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static long g_page;

/* ---- 1. split a virtual address the way the MMU does ----
 *
 *  4 KiB granule, 48-bit VA (x86-64 and arm64 defaults):
 *
 *   47      39 38      30 29      21 20      12 11          0
 *  +----------+----------+----------+----------+-------------+
 *  | L0 / PGD | L1 / PUD | L2 / PMD | L3 / PTE |   offset    |
 *  |  9 bits  |  9 bits  |  9 bits  |  9 bits  |   12 bits   |
 *  +----------+----------+----------+----------+-------------+
 *       |          |          |          |
 *  CR3 / TTBR0 -> [512] --> [512] --> [512] --> [512] --> page frame + offset
 *
 *  64 KiB granule (an arm64 kernel option): 13-bit indices, 16-bit offset,
 *  3 levels for 48 bits. Fewer levels, fewer TLB misses, more waste.
 */
static void demo_split(void *addr)
{
    uintptr_t va = (uintptr_t)addr;
    printf("1) page size %ld (sysconf) = %lu (auxv AT_PAGESZ)\n", g_page,
           getauxval(AT_PAGESZ));
    CHECK((unsigned long)g_page == getauxval(AT_PAGESZ));
    if (g_page == 4096) {
        unsigned l0 = (va >> 39) & 511, l1 = (va >> 30) & 511;
        unsigned l2 = (va >> 21) & 511, l3 = (va >> 12) & 511;
        unsigned off = va & 4095;
        printf("   va %p -> L0 %3u | L1 %3u | L2 %3u | L3 %3u | offset "
               "0x%03x\n", addr, l0, l1, l2, l3, off);
        /* Rebuild the address from the pieces: nothing lost. */
        uintptr_t back = ((uintptr_t)l0 << 39) | ((uintptr_t)l1 << 30) |
                         ((uintptr_t)l2 << 21) | ((uintptr_t)l3 << 12) | off;
        CHECK(back == (va & ((1ULL << 48) - 1)));
    } else if (g_page == 65536) {
        unsigned l1 = (va >> 42) & 63, l2 = (va >> 29) & 8191;
        unsigned l3 = (va >> 16) & 8191, off = va & 65535;
        printf("   va %p (64K granule) -> L1 %u | L2 %u | L3 %u | offset "
               "0x%04x\n", addr, l1, l2, l3, off);
    } else {
        printf("   page size %ld: split not shown\n", g_page);
    }
    printf("   one page-table page holds %ld 8-byte entries; one L3 table "
           "maps %ld KiB, one L2 entry %ld MiB (a huge page)\n", g_page / 8,
           (g_page / 8) * g_page / 1024, (g_page / 8) * g_page >> 20);
}

/* ---- 2. /proc/self/pagemap: one 64-bit entry per virtual page ----
 *
 *   bit 63 present in RAM   bit 62 swapped   bit 61 file/shared-anon
 *   bit 56 exclusively mapped   bit 55 soft-dirty   bits 0-54 PFN
 *   (PFN reads as 0 unless CAP_SYS_ADMIN: physical addresses help
 *    Rowhammer-style attacks, so they are hidden since Linux 4.0)
 */
static uint64_t pagemap_entry(int fd, const void *addr)
{
    uint64_t e = 0;
    off_t off = (off_t)((uintptr_t)addr / (uintptr_t)g_page) * 8;
    CHECK(pread(fd, &e, sizeof e, off) == (ssize_t)sizeof e);
    return e;
}

static void demo_pagemap(void)
{
    enum { N = 8 };
    char *buf = mmap(NULL, N * (size_t)g_page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(buf != MAP_FAILED);
    /* With THP "always" one touch could map a whole 2 MiB page: keep this
     * region on 4 KiB pages so the picture is deterministic. */
    madvise(buf, N * (size_t)g_page, MADV_NOHUGEPAGE);
    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        printf("2) /proc/self/pagemap not readable (%s): skipped\n",
               strerror(errno));
        munmap(buf, N * (size_t)g_page);
        return;
    }

    int before = 0, after = 0;
    for (int i = 0; i < N; i++)
        before += (int)(pagemap_entry(fd, buf + i * g_page) >> 63);
    for (int i = 0; i < N; i += 2) /* touch only the even pages */
        buf[i * g_page] = 1;
    uint64_t pfn_or = 0;
    printf("2) pagemap for an 8-page mmap, even pages written:\n     ");
    for (int i = 0; i < N; i++) {
        uint64_t e = pagemap_entry(fd, buf + i * g_page);
        int present = (int)(e >> 63);
        after += present;
        pfn_or |= e & ((1ULL << 55) - 1);
        printf("[%d:%s] ", i, present ? "RAM" : "---");
        CHECK(present == (i % 2 == 0)); /* demand paging, see 02 */
    }
    printf("\n   present before touching: %d, after: %d\n", before, after);
    CHECK(before == 0 && after == N / 2);
    if (pfn_or == 0)
        printf("   PFNs read as 0: no CAP_SYS_ADMIN (run as root to see "
               "physical frame numbers)\n");
    else
        printf("   PFN of page 0 = 0x%llx -> physical address 0x%llx\n",
               (unsigned long long)(pagemap_entry(fd, buf) &
                                    ((1ULL << 55) - 1)),
               (unsigned long long)((pagemap_entry(fd, buf) &
                                     ((1ULL << 55) - 1)) *
                                    (uint64_t)g_page));
    close(fd);
    CHECK(munmap(buf, N * (size_t)g_page) == 0);
}

/* ---- 3. the TLB: same number of loads, few pages vs many pages ----
 *
 *  A: 1M loads inside 16 pages        -> every translation hits the TLB
 *  B: 1M loads, one per page, 16384 pages (64 MiB) -> TLB (and cache) misses
 *  C: like B but with a 2 MiB transparent huge page -> 1 entry covers 512 pages
 */
static double walk(volatile char *buf, size_t span_pages, size_t loads)
{
    size_t stride = (size_t)g_page + 64; /* new page AND new cache line */
    size_t span = span_pages * (size_t)g_page;
    size_t idx = 0;
    unsigned sum = 0;
    double t0 = now_ns();
    for (size_t i = 0; i < loads; i++) {
        sum += (unsigned char)buf[idx];
        idx += stride;
        if (idx >= span)
            idx -= span;
    }
    double t = (now_ns() - t0) / (double)loads;
    (void)sum;
    return t;
}

static void demo_tlb(void)
{
    const size_t pages = 16384, loads = 1 << 20;
    size_t len = pages * (size_t)g_page;
    char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(buf != MAP_FAILED);
    madvise(buf, len, MADV_NOHUGEPAGE); /* force 4 KiB translations */
    memset(buf, 1, len);                /* fault everything in first */
    double few = walk(buf, 16, loads);
    double many = walk(buf, pages, loads);
    CHECK(munmap(buf, len) == 0);

    /* Same again with a THP-backed region (if THP is enabled). */
    size_t hlen = 64u << 20;
    char *raw = mmap(NULL, hlen + (2u << 20), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(raw != MAP_FAILED);
    char *h = (char *)(((uintptr_t)raw + (2u << 20) - 1) & ~((uintptr_t)(2u << 20) - 1));
    int thp = madvise(h, hlen, MADV_HUGEPAGE) == 0;
    memset(h, 1, hlen);
    double huge = walk(h, hlen / (size_t)g_page, loads);
    CHECK(munmap(raw, hlen + (2u << 20)) == 0);

    printf("3) 1M loads, printed not asserted:\n");
    printf("     16 pages (TLB hits)                  %6.2f ns/load\n", few);
    printf("     16384 4K pages (TLB + cache misses)  %6.2f ns/load\n", many);
    printf("     64 MiB with MADV_HUGEPAGE%s %6.2f ns/load\n",
           thp ? "            " : " (refused)  ", huge);
}

int main(void)
{
    alarm(20);
    g_page = sysconf(_SC_PAGESIZE);
    CHECK(g_page > 0);
    int local = 0;
    demo_split(&local);
    demo_pagemap();
    demo_tlb();
    printf("01_pages_and_translation: all checks passed\n");
    return 0;
}
