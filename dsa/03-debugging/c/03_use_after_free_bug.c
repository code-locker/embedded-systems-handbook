/*
 * 03_use_after_free_bug.c — INTENTIONAL BUG: heap use-after-free
 *
 * Bug     : A camera "session" is freed when it closes, but a stale pointer
 *           kept in a global "current" variable is used afterwards. Without
 *           a tool, this usually "works" (the memory still holds old data),
 *           or silently reads data of whoever malloc'd the block next.
 * Finds it: AddressSanitizer (heap-use-after-free, with all three stacks:
 *           the bad access, the free, the original malloc); valgrind
 *           ("Invalid read of size 4 ... inside a block of size 16 free'd").
 * Build   : make run P=dsa/03-debugging/c/03_use_after_free_bug.c   -> ASan report, exit 1
 * Read    : ../README.md  (section "Bug 2: use-after-free")
 */
#include <stdio.h>
#include <stdlib.h>

struct session {
    int id;
    int frames;
    struct session *next;
};

static struct session *current;          /* a second, non-owning pointer */

static struct session *open_session(int id)
{
    struct session *s = calloc(1, sizeof *s);
    s->id = id;
    current = s;                          /* stale copy is born here */
    return s;
}

static void close_session(struct session *s)
{
    printf("closing session %d after %d frames\n", s->id, s->frames);
    free(s);                              /* 'current' is NOT cleared: BUG */
}

int main(void)
{
    struct session *s = open_session(7);
    for (int i = 0; i < 3; i++)
        s->frames++;
    close_session(s);

    /* ... much later, a timer callback uses the global: */
    printf("timer: current session has %d frames\n", current->frames);   /* <-- use after free */
    return 0;
}
