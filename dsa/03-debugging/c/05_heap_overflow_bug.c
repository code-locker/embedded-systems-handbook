/*
 * 05_heap_overflow_bug.c — INTENTIONAL BUG: off-by-one heap buffer overflow
 *
 * Bug     : strdup written by hand forgets the +1 for the '\0' terminator,
 *           so strcpy writes one byte past the end of the block. Without a
 *           tool, that byte lands in malloc's padding or the NEXT chunk's
 *           header, and the program crashes much later inside malloc/free
 *           ("malloc(): corrupted top size") — far away from the real bug.
 * Finds it: AddressSanitizer (heap-buffer-overflow, "0 bytes after
 *           11-byte region"); valgrind ("Invalid write of size 1").
 * Build   : make run P=dsa/03-debugging/c/05_heap_overflow_bug.c   -> ASan report, exit 1
 * Read    : ../README.md  (section "Bug 4: off-by-one heap overflow")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *my_strdup(const char *s)
{
    size_t n = strlen(s);
    char *copy = malloc(n);              /* BUG: needs n + 1 for the '\0' */
    if (copy)
        strcpy(copy, s);                  /* writes n + 1 bytes */
    return copy;
}

int main(void)
{
    char *name = my_strdup("camera-left");   /* 11 chars -> 11-byte block, 12 written */
    printf("copied: %s\n", name);
    free(name);
    return 0;
}
