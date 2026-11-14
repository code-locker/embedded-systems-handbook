/*
 * 04_stack_overflow_bug.c — INTENTIONAL BUG: stack exhaustion by unbounded recursion
 *
 * Bug     : A recursive tree walk has a broken base case (it checks the
 *           wrong variable), so it recurses until the 8 MiB stack runs into
 *           the guard gap -> SIGSEGV.
 * Finds it: AddressSanitizer ("stack-overflow on address ..."), or plainly
 *           gdb: run, then 'bt -5' shows thousands of identical frames.
 *           A core dump + 'bt' shows the same after the fact.
 * Build   : make run P=dsa/03-debugging/c/04_stack_overflow_bug.c   -> ASan stack-overflow, exit 1
 * Read    : ../README.md  (section "Bug 3: stack overflow by recursion")
 */
#include <stdio.h>

/* Count nodes of a complete binary tree of given depth, by recursion. */
static unsigned long count_nodes(int depth, int level)
{
    volatile char scratch[256];          /* makes each frame bigger: overflows sooner */
    scratch[0] = (char)level;
    if (depth == 0)                       /* BUG: should test 'level == depth' */
        return 1;
    return 1 + count_nodes(depth, level + 1) + (unsigned long)scratch[0] * 0;
}

int main(void)
{
    printf("nodes = %lu\n", count_nodes(10, 0));
    return 0;
}
