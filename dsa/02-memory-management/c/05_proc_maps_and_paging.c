/*
 * 05_proc_maps_and_paging.c — read /proc/self/maps, smaps and status like a kernel engineer
 *
 * Concept : /proc/self/maps lists every VMA (virtual memory area) of this
 *           process. A VMA is only a PROMISE of addresses: physical pages are
 *           allocated lazily, on the first touch (demand paging, via a page
 *           fault). So "virtual size" (VmSize) can be huge while "resident
 *           size" (VmRSS) stays small. This program annotates the map,
 *           reads smaps/status, and proves demand paging with a 1 GiB mapping.
 * Build   : make run P=dsa/02-memory-management/c/05_proc_maps_and_paging.c
 * Read    : ../README.md  (section "3.5 The memory map in depth")
 *           Page tables, TLBs, page faults in detail:
 *           linux-internals/03-virtual-memory/README.md (repo root)
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#else
#define HAVE_ASAN 0
#endif

/* Read one "Key:   12345 kB" line from /proc/self/status. Returns kB or -1. */
static long status_kb(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    long v = -1;
    size_t klen = strlen(key);
    while (f && fgets(line, sizeof line, f))
        if (strncmp(line, key, klen) == 0 && line[klen] == ':') {
            v = strtol(line + klen + 1, NULL, 10);
            break;
        }
    if (f) fclose(f);
    return v;
}

/* Sum a field (e.g. "Rss") over the smaps entry whose range starts at 'start'
 * (start == 0: match by name instead).                                     */
static long smaps_field(uintptr_t start, const char *name, const char *field)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    char line[512];
    int in = 0;
    long v = -1;
    size_t flen = strlen(field);
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2 && strchr(line, ' ') - line > 8 &&
            line[strspn(line, "0123456789abcdef")] == '-') {          /* a VMA header line */
            in = start ? (lo == start) : (name && strstr(line, name) != NULL);
            continue;
        }
        if (in && strncmp(line, field, flen) == 0 && line[flen] == ':') {
            v = strtol(line + flen + 1, NULL, 10);
            break;
        }
    }
    if (f) fclose(f);
    return v;
}

static const char *classify(const char *perms, const char *path, int prev_was_file_rw)
{
    if (strcmp(path, "[heap]") == 0) return "brk heap (glibc malloc small blocks)";
    if (strcmp(path, "[stack]") == 0) return "main thread stack (grows down)";
    if (strcmp(path, "[vdso]") == 0) return "vDSO: kernel code for fast gettimeofday etc.";
    if (strncmp(path, "[vvar", 5) == 0) return "vvar: kernel data read by the vDSO";
    if (strcmp(path, "[vsyscall]") == 0) return "legacy vsyscall page (x86_64)";
    if (path[0] == '/') {
        if (perms[2] == 'x') return "code (.text) of this file";
        if (perms[1] == 'w') return ".data (+ GOT) of this file";
        return "read-only: ELF headers, .rodata, or RELRO after relocation";
    }
    if (prev_was_file_rw && perms[1] == 'w') return ".bss continuation (anonymous, zero-filled)";
    if (perms[0] == '-') return "no access: guard region / reserved address space";
    return "anonymous mmap (malloc arenas, thread stacks, big blocks...)";
}

static void annotate_maps(void)
{
    printf("start-end         perms offset   dev   inode     path   -> meaning\n");
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int prev_file_rw = 0, anon_skipped = 0;
    while (f && fgets(line, sizeof line, f)) {
        unsigned long lo, hi, off, inode;
        char perms[5], dev[16], path[256] = "";
        int n = sscanf(line, "%lx-%lx %4s %lx %15s %lu %255[^\n]", &lo, &hi, perms, &off, dev, &inode, path);
        if (n < 6)
            continue;
        const char *meaning = classify(perms, path, prev_file_rw);
        prev_file_rw = (path[0] == '/' && perms[1] == 'w');
        if (HAVE_ASAN && path[0] == '\0') { anon_skipped++; continue; }  /* ASan shadow: hundreds */
        const char *base = strrchr(path, '/');
        printf("%012lx-%012lx %s %08lx %-5s %-9lu %-22.22s -> %s\n", lo, hi, perms, off, dev, inode,
               base ? base + 1 : path, meaning);
    }
    if (f) fclose(f);
    if (anon_skipped)
        printf("(%d anonymous lines hidden: ASan shadow memory and allocator regions)\n", anon_skipped);
}

int main(void)
{
    printf("=== 1. /proc/self/maps, annotated ===\n");
    printf("columns: address range | r w x p(rivate COW)/s(hared) | file offset | major:minor | inode | path\n\n");
    annotate_maps();

    printf("\n=== 2. /proc/self/status (whole process) ===\n");
    const char *keys[] = {"VmPeak", "VmSize", "VmRSS", "RssAnon", "RssFile", "VmData", "VmStk", "VmExe", "VmLib"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        printf("  %-8s %10ld kB\n", keys[i], status_kb(keys[i]));
    if (HAVE_ASAN)
        printf("  (VmSize is terabytes under ASan: it RESERVES shadow memory, touching almost none)\n");

    printf("\n=== 3. /proc/self/smaps for [stack] ===\n");
    const char *fields[] = {"Size", "Rss", "Pss", "Private_Dirty", "Swap"};
    for (size_t i = 0; i < 5; i++)
        printf("  %-14s %6ld kB\n", fields[i], smaps_field(0, "[stack]", fields[i]));
    assert(smaps_field(0, "[stack]", "Rss") > 0);

    printf("\n=== 4. Demand paging: map 1 GiB, touch only 64 MiB ===\n");
    const size_t GiB = 1UL << 30, touch = 64UL << 20;
    long size0 = status_kb("VmSize"), rss0 = status_kb("VmRSS");
    char *big = mmap(NULL, GiB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(big != MAP_FAILED);
    long size1 = status_kb("VmSize"), rss1 = status_kb("VmRSS");
    printf("  after mmap(1 GiB):   VmSize %+8ld kB   VmRSS %+6ld kB   (address space only!)\n",
           size1 - size0, rss1 - rss0);
    assert(size1 - size0 >= (long)(GiB / 1024) - 1024);
    assert(rss1 - rss0 < 4096);

    long page = sysconf(_SC_PAGESIZE);
    for (size_t off = 0; off < touch; off += (size_t)page)
        big[off] = 1;                    /* first write to each page -> page fault -> zero page allocated */
    long rss2 = status_kb("VmRSS");
    long vma_rss = smaps_field((uintptr_t)big, NULL, "Rss");
    printf("  after touching 64 MiB: VmRSS %+6ld kB   smaps Rss of the region = %ld kB\n", rss2 - rss0, vma_rss);
    assert(rss2 - rss0 >= 48 * 1024);
    assert(vma_rss >= 60 * 1024 && vma_rss <= 70 * 1024);

    assert(madvise(big, touch, MADV_DONTNEED) == 0);   /* give the pages back, keep the VMA */
    printf("  after madvise(DONTNEED): smaps Rss of the region = %ld kB\n", smaps_field((uintptr_t)big, NULL, "Rss"));
    assert(big[0] == 0);                               /* re-touch: a fresh ZERO page */
    munmap(big, GiB);

    printf("\n=== 5. The heap grows with brk/sbrk ===\n");
    char *brk0 = sbrk(0);
    void *got = sbrk(64 * 1024);                       /* raw: move the program break up 64 KiB */
    char *brk1 = sbrk(0);
    assert(got == brk0 && brk1 == brk0 + 64 * 1024);
    memset(brk0, 0xAB, 64 * 1024);                     /* the new memory is ours */
    assert(sbrk(-64 * 1024) != (void *)-1);            /* and give it back (nothing else used it) */
    printf("  sbrk(0) = %p, sbrk(64 KiB) -> break %p, then shrunk back to %p\n",
           (void *)brk0, (void *)brk1, sbrk(0));
    printf("  (malloc does exactly this for small blocks; see 04_glibc_malloc_internals.c)\n");

    printf("\n=== 6. ASLR ===\n");
    FILE *f = fopen("/proc/sys/kernel/randomize_va_space", "r");
    int aslr = -1;
    if (f) { if (fscanf(f, "%d", &aslr) != 1) aslr = -1; fclose(f); }
    int on_stack = 0;
    printf("  randomize_va_space = %d (2 = stack, mmap, heap, PIE all randomised)\n", aslr);
    printf("  this run: main=%#lx  stack=%p  libc(strlen)=%#lx\n",
           (unsigned long)(uintptr_t)main, (void *)&on_stack, (unsigned long)(uintptr_t)strlen);
    printf("  run me twice: these change. 'setarch -R <prog>' (or setarch $(uname -m) -R)\n"
           "  disables randomisation for one run — gdb does that by default.\n");

    printf("\nAll checks passed.\n");
    return 0;
}
