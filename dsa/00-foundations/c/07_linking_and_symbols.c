/*
 * 07_linking_and_symbols.c — what the linker and the dynamic loader did for you
 *
 * Concept : Your executable references printf but does not contain it: it is
 *           DYNAMICALLY linked against libc.so.6. At start-up the dynamic loader
 *           (ld-linux) maps the shared libraries and resolves the symbols.
 *           This program asks the loader which shared objects it mapped.
 * Build   : make run P=dsa/00-foundations/c/07_linking_and_symbols.c
 * Read    : ../README.md  (sections "Object files and symbols", "Static vs dynamic linking")
 *
 * Then inspect the binary:
 *   B=build/dsa/00-foundations/c/07_linking_and_symbols
 *   file $B                    # "dynamically linked, interpreter /lib64/ld-linux-x86-64.so.2"
 *   ldd $B                     # which .so files it needs (libasan too, because of -fsanitize)
 *   nm $B | grep -E ' (T|t|U|D|B) ' | grep -E 'main|square|g_|printf'
 *   readelf -d $B | grep NEEDED
 *   objdump -d $B --disassemble=square
 * Static build for comparison (no sanitizers, they need the shared runtime):
 *   gcc -static -O2 dsa/00-foundations/c/07_linking_and_symbols.c -o /tmp/st && ls -lh /tmp/st && /tmp/st
 */
#define _GNU_SOURCE
#include <assert.h>
#include <inttypes.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int g_initialised = 5;      /* nm: D (initialised data, global)   */
int g_uninitialised;        /* nm: B (bss, global)                */
static int s_hidden = 9;    /* nm: d (lowercase = local symbol)   */

int square(int x) { return x * x; }          /* nm: T (text, global) */
static int cube(int x) { return x * x * x; } /* nm: t (local)        */

struct scan { int count; int saw_libc; };

/* Called by the loader once per loaded object (executable, vdso, each .so). */
static int on_object(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct scan *s = data;
    const char *name = info->dlpi_name[0] ? info->dlpi_name : "(main executable)";
    printf("  [%d] base=0x%012lx  %s\n", s->count, (unsigned long)info->dlpi_addr, name);
    if (strstr(info->dlpi_name, "libc.so"))
        s->saw_libc = 1;
    s->count++;
    return 0;   /* 0 = keep iterating */
}

int main(void)
{
    printf("Shared objects mapped into this process:\n");
    struct scan s = {0, 0};
    dl_iterate_phdr(on_object, &s);

    if (s.saw_libc)
        printf("=> dynamically linked: printf lives in libc.so, resolved at load time\n");
    else
        printf("=> no libc.so mapped: this binary was linked statically\n");

    /* The symbol addresses below are ADDRESSES IN THIS PROCESS. With PIE
     * (default on Ubuntu) they change every run because of ASLR.          */
    /* ISO C forbids converting a function pointer to void * (-Wpedantic says
     * so), so we print function addresses through uintptr_t.               */
    printf("\n  &square  = 0x%" PRIxPTR " (our .text)\n", (uintptr_t)square);
    printf("  &printf  = 0x%" PRIxPTR " (far away: in a shared library; under ASan it is\n"
           "                             ASan's printf interceptor in libasan.so)\n", (uintptr_t)printf);
    printf("  &g_initialised = %p, &g_uninitialised = %p\n",
           (void *)&g_initialised, (void *)&g_uninitialised);

    assert(s.count >= 2);
    assert(square(g_initialised) == 25 && cube(2) == 8 && s_hidden == 9);
    assert(g_uninitialised == 0);
    printf("\nAll checks passed. Now run the nm/ldd/readelf commands in the header.\n");
    return 0;
}
