/*
 * 09_null_dangling_wild.c — the three kinds of bad pointer, and the habits that prevent them
 *
 * Concept : NULL     = points nowhere ON PURPOSE; dereferencing it faults (page 0
 *                      is never mapped), which is the GOOD failure: loud and early.
 *           dangling = used to point at a live object, the object died
 *                      (freed, or a local whose function returned).
 *           wild     = never initialised; holds stack garbage.
 *           This program runs only the SAFE patterns. The crashing versions are
 *           in dsa/03-debugging (*_bug.c) with the gdb/ASan sessions that find them.
 * Build   : make run P=dsa/01-pointers/c/09_null_dangling_wild.c
 * Read    : ../README.md  (section "3.9 NULL, dangling and wild pointers")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Habit 1: free + set to NULL in one step. A later double free becomes
 * free(NULL) (a no-op), a later use becomes a clean NULL fault.          */
#define FREE_AND_NULL(p) do { free(p); (p) = NULL; } while (0)

/* Habit 2: never return the address of a local. Return by value, take an
 * output buffer from the caller, or allocate and document who frees.     */
/* BAD (gcc: "function returns address of local variable"):
 *   static char *make_name(void) { char buf[16] = "cam0"; return buf; }   */
static void make_name(char *out, size_t cap) { snprintf(out, cap, "cam%d", 0); }
static char *make_name_heap(void)               /* caller must free() */
{
    char *s = malloc(8);
    if (s) strcpy(s, "cam1");
    return s;
}

/* Habit 3: check every pointer that can legitimately be NULL at the boundary. */
static size_t safe_len(const char *s) { return s ? strlen(s) : 0; }

int main(void)
{
    /* ---- NULL ---- */
    int *p = NULL;                   /* initialise EVERY pointer */
    assert(p == NULL && !p);         /* NULL compares equal to 0, is "false" */
    if (p)                           /* guard before use */
        *p = 1;
    assert(safe_len(NULL) == 0 && safe_len("abc") == 3);
    free(NULL);                      /* defined: does nothing */

    /* ---- dangling: heap ----
     *
     *  before free:   q ──► [ 42 ]  (heap block, owned)
     *  after free:    q ──► [ ?? ]  block returned to malloc; may be reused
     *                                for something else -> reads/writes corrupt it
     *  after q=NULL:  q ──► (nothing) — any use now faults immediately
     */
    int *q = malloc(sizeof *q);
    assert(q);
    *q = 42;
    int *alias = q;                  /* SECOND pointer to the same block */
    FREE_AND_NULL(q);
    assert(q == NULL);
    /* alias is STILL dangling: setting q to NULL cannot fix other copies.
     * This is why ownership must be clear: exactly one owner frees.     */
    (void)alias;                     /* never dereference it again */

    /* ---- dangling: stack ---- */
    char name[16];
    make_name(name, sizeof name);
    assert(strcmp(name, "cam0") == 0);
    char *h = make_name_heap();
    assert(h && strcmp(h, "cam1") == 0);
    free(h);

    /* ---- dangling: realloc moves the block ---- */
    int *arr = malloc(4 * sizeof *arr);
    assert(arr);
    /* Keep the old address only as a NUMBER for printing: even comparing a
     * freed pointer is technically UB, and gcc warns (-Wuse-after-free).   */
    uintptr_t old_addr = (uintptr_t)arr;
    int *bigger = realloc(arr, 1024 * 1024 * sizeof *arr);
    assert(bigger);
    /* arr and every pointer INTO the old block may now be dangling: the
     * block probably moved. Re-derive interior pointers from the new base. */
    printf("realloc: old base 0x%jx, new base %p (%s)\n", (uintmax_t)old_addr, (void *)bigger,
           old_addr == (uintptr_t)bigger ? "same" : "MOVED: old pointers are dangling");
    arr = bigger;
    free(arr);

    /* ---- wild ----
     *   int *w;  *w = 5;   // w holds whatever bytes were on the stack
     * -Wall gives "is used uninitialized" for this. Always initialise. */
    int *w = NULL;
    (void)w;

    printf("All checks passed (only the safe patterns were executed).\n");
    return 0;
}
