/*
 * 05_storage_and_qualifiers.c — const, volatile, static, extern in one place
 *
 * Concept : static on a LOCAL  = lives for the whole program (stored in .data/.bss).
 *           static at FILE scope = internal linkage (invisible to the linker's
 *           other objects). extern = "defined somewhere else". const = "I promise
 *           not to write through this name". volatile = "every access really
 *           happens; do not cache in a register" (MMIO, ISR-shared flags).
 * Build   : make run P=dsa/00-foundations/c/05_storage_and_qualifiers.c
 * Read    : ../README.md  (section "const, volatile, static, extern")
 *
 * After building, look at the symbols:
 *   nm build/dsa/00-foundations/c/05_storage_and_qualifiers | grep -E 'counter|g_|helper'
 *   'T'/'D'/'B' uppercase = global (external linkage); 't'/'d'/'b' lowercase = static.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* extern declaration: no storage is created here, only a promise.
 * In a real project this line lives in a header and the definition in one .c. */
extern int g_boot_count;

/* The one DEFINITION (creates storage in .data because it is initialised). */
int g_boot_count = 1;

/* Uninitialised globals go to .bss (zero-filled by the loader, costs no file space). */
int g_zeroed;

/* File-scope static: internal linkage. Another .c can have its own g_private. */
static int g_private = 7;

/* const global: placed in .rodata (read-only page — writing it segfaults). */
const int g_table[3] = {1, 2, 3};

/* static function: only callable from this file; lets the compiler inline it
 * and drop the symbol. Default for every helper that is not part of an API. */
static int helper(int x) { return x * 2; }

/* static local: initialised once, keeps its value between calls. */
static int next_id(void)
{
    static int counter = 100;   /* NOT on the stack: lives in .data */
    return counter++;
}

/* volatile: pretend this is a hardware status register or a flag an ISR sets.
 * Without volatile, the compiler at -O2 may read it once and loop forever.   */
static volatile uint32_t fake_status_reg = 0;

static void fake_isr(void) { fake_status_reg = 1; }

static int wait_ready(int max_polls)
{
    for (int i = 0; i < max_polls; i++) {
        if (fake_status_reg & 1u)        /* re-read from memory EVERY time */
            return i;
        if (i == 3)
            fake_isr();                  /* "hardware" becomes ready */
    }
    return -1;
}

/* const and pointers (deep dive in dsa/01-pointers):
 *   const int *p   -> data is read-only through p, p can move
 *   int *const p   -> p cannot move, data is writable
 * Function parameters that are only read should be pointer-to-const.       */
static int sum(const int *v, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += v[i];          /* v[i] = 0; would be a compile error */
    return s;
}

int main(void)
{
    printf("g_boot_count (.data) = %d at %p\n", g_boot_count, (void *)&g_boot_count);
    printf("g_zeroed     (.bss)  = %d at %p\n", g_zeroed, (void *)&g_zeroed);
    printf("g_table   (.rodata)  = {%d,%d,%d} at %p\n", g_table[0], g_table[1], g_table[2],
           (void *)g_table);
    assert(g_zeroed == 0);      /* guaranteed: static storage is zero-initialised */

    int a = next_id(), b = next_id(), c = next_id();
    printf("next_id() x3 = %d %d %d (static local remembers)\n", a, b, c);
    assert(a == 100 && b == 101 && c == 102);

    assert(helper(g_private) == 14);
    assert(sum(g_table, 3) == 6);

    int polls = wait_ready(10);
    printf("volatile status register became ready after %d polls\n", polls);
    assert(polls == 4);

    /* const local: compiler error if you assign. A const object is not a
     * compile-time constant in C (it is in C++): you cannot write
     * "const int n = 4; int arr[n];" at file scope in C. Use enum or #define. */
    enum { N = 4 };
    int arr[N] = {0};
    assert(sizeof arr == N * sizeof(int));

    printf("All checks passed.\n");
    return 0;
}
