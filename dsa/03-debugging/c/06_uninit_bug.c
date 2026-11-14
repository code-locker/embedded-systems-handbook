/*
 * 06_uninit_bug.c — INTENTIONAL BUG: reading uninitialised memory
 *
 * Bug     : A config struct is malloc'd (not calloc'd) and one field (fps) is
 *           never set, then used in a decision and a calculation. The value is
 *           whatever was in that memory: 0 on a fresh glibc heap, 0xbebebebe
 *           under ASan (it fills new blocks with 0xbe), old data after reuse.
 * Finds it: valgrind memcheck ("Conditional jump or move depends on
 *           uninitialised value(s)", with --track-origins=yes pointing at the
 *           malloc). NOT ASan or UBSan. clang's MemorySanitizer (-fsanitize=memory)
 *           also finds it. For uninitialised LOCALS, gcc -Wall -O2 often warns
 *           at compile time (-Wmaybe-uninitialized).
 * Build   : make run P=dsa/03-debugging/c/06_uninit_bug.c   -> prints a garbage fps, exit 1
 *           valgrind: make SAN=0 build/dsa/03-debugging/c/06_uninit_bug
 *                     valgrind --track-origins=yes ./build/dsa/03-debugging/c/06_uninit_bug
 * Read    : ../README.md  (section "Bug 5: uninitialised read")
 */
#include <stdio.h>
#include <stdlib.h>

struct config {
    int width;
    int height;
    int fps;
};

static struct config *load_config(void)
{
    struct config *c = malloc(sizeof *c);   /* malloc: contents are garbage */
    if (!c)
        return NULL;
    c->width = 1920;
    c->height = 1080;
    /* BUG: forgot c->fps = 30; */
    return c;
}

int main(void)
{
    struct config *c = load_config();
    if (!c)
        return 2;
    if (c->fps > 60)                          /* decision on garbage */
        printf("high frame-rate mode\n");
    long frames_per_min = (long)c->fps * 60;
    printf("%dx%d @ %d fps -> %ld frames per minute\n", c->width, c->height, c->fps, frames_per_min);
    int ok = c->fps == 30;
    if (!ok)
        printf("WRONG: expected 30 fps (0x%08x is %s)\n", (unsigned)c->fps,
               (unsigned)c->fps == 0xbebebebeu ? "ASan's malloc fill pattern" : "leftover memory");
    free(c);
    return ok ? 0 : 1;
}
