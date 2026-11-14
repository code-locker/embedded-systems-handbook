/*
 * 01_process_layout.c — where text, rodata, data, bss, heap, mmap and stack really are
 *
 * Concept : A process sees one big virtual address space split into regions
 *           (VMAs). This program takes the address of one object of each
 *           kind, then looks each address up in /proc/self/maps so you can
 *           see WHICH region (and with which permissions) holds it.
 * Build   : make run P=dsa/02-memory-management/c/01_process_layout.c
 *           For the classic [heap] (brk) picture, build WITHOUT sanitizers,
 *           because ASan replaces malloc with its own mmap-based allocator:
 *             make SAN=0 run P=dsa/02-memory-management/c/01_process_layout.c
 * Read    : ../README.md  (section "3.1 Process memory layout")
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

int g_data = 42;                         /* .data : initialised, writable           */
int g_bss;                               /* .bss  : zero-initialised, writable      */
const char g_rodata[] = "read-only";     /* .rodata: read-only                      */

/* Find the /proc/self/maps line whose range contains addr.
 * Line format: start-end perms offset dev inode [path]                         */
static int lookup(uintptr_t addr, char *perms, char *path, size_t cap)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char p[5] = "", name[256] = "";
        int n = sscanf(line, "%lx-%lx %4s %*s %*s %*s %255[^\n]", &lo, &hi, p, name);
        if (n >= 3 && addr >= lo && addr < hi) {
            snprintf(perms, 5, "%s", p);
            snprintf(path, cap, "%s", n == 4 ? name : "(anonymous)");
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void show(const char *what, const void *addr, char *perms_out)
{
    char perms[5] = "?", path[256] = "?";
    lookup((uintptr_t)addr, perms, path, sizeof path);
    const char *base = strrchr(path, '/');
    printf("  %-26s %18p  %s  %s\n", what, addr, perms, base ? base + 1 : path);
    if (perms_out)
        memcpy(perms_out, perms, 5);
}

int main(int argc, char **argv)
{
    (void)argc;
    int local = 1;                                   /* stack */
    int *heap_small = malloc(sizeof *heap_small);    /* heap (brk) — or ASan's allocator */
    void *anon = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);   /* its own anonymous mapping */
    assert(heap_small && anon != MAP_FAILED);
    *heap_small = 7;

    /* Function addresses cannot portably become void*; go through uintptr_t. */
    const void *text = (const void *)(uintptr_t)main;
    const void *libc_fn = (const void *)(uintptr_t)strlen;

    char p_text[5], p_ro[5], p_data[5], p_bss[5], p_stack[5], p_anon[5];
    printf("  %-26s %18s  perm  region\n", "object", "address");
    show("main()          (.text)", text, p_text);
    show("string literal  (.rodata)", "literal", NULL);
    show("g_rodata        (.rodata)", g_rodata, p_ro);
    show("g_data          (.data)", &g_data, p_data);
    show("g_bss           (.bss)", &g_bss, p_bss);
    show("malloc(4)       (heap)", heap_small, NULL);
    show("mmap(1 MiB)     (anon)", anon, p_anon);
    show("strlen()        (libc)", libc_fn, NULL);
    show("local variable  (stack)", &local, p_stack);
    show("argv[0] string  (stack)", argv[0], NULL);
    printf("  program break sbrk(0)      %18p\n", sbrk(0));

    /* Permissions tell the story: code r-x, constants r--, variables rw-. */
    assert(strchr(p_text, 'x') && !strchr(p_text, 'w'));
    assert(p_ro[1] == '-');                   /* not writable */
    assert(p_data[1] == 'w' && p_bss[1] == 'w');
    assert(p_anon[0] == 'r' && p_anon[1] == 'w' && p_anon[2] == '-');
    assert(p_stack[1] == 'w' && p_stack[2] == '-');   /* NX stack: not executable */

    /* Classic ordering (low -> high): text < data < bss < ... < stack. */
    assert((uintptr_t)text < (uintptr_t)&g_data);
    assert((uintptr_t)&g_data < (uintptr_t)&local);

    if (HAVE_ASAN)
        printf("\n(ASan build: malloc comes from ASan's own mmap'd regions, not [heap].\n"
               " Rebuild with SAN=0 to see malloc(4) land in [heap] near sbrk(0).\n"
               " ASan may also move address-taken locals to a \"fake stack\" (an anonymous\n"
               " mapping) to detect use-after-return; argv[0] still shows the real [stack].)\n");

    printf("\nThe full map, for comparison (cat /proc/self/maps):\n");
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int shown = 0;
    while (f && fgets(line, sizeof line, f))
        if (!HAVE_ASAN || strchr(line, '/') || strchr(line, '['))   /* ASan adds hundreds of anon lines */
            printf("  %s", line), shown++;
    if (f) fclose(f);
    assert(shown > 5);

    munmap(anon, 1 << 20);
    free(heap_small);
    printf("\nAll checks passed.\n");
    return 0;
}
