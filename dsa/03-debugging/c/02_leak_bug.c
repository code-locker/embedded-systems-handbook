/*
 * 02_leak_bug.c — INTENTIONAL BUG: memory leak
 *
 * Bug     : make_label() returns a malloc'd string; the caller overwrites the
 *           pointer in a loop and never frees it. Also an early return path
 *           skips free(). Classic leaks: lost pointer + missing free on error path.
 * Finds it: LeakSanitizer (part of ASan, runs automatically at exit),
 *           valgrind --leak-check=full (build with SAN=0).
 * Build   : make run P=dsa/03-debugging/c/02_leak_bug.c          -> LSan report, exit 1
 * Read    : ../README.md  (section "Bug 1: memory leak")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *make_label(int id)
{
    char *s = malloc(32);              /* caller must free */
    if (!s)
        return NULL;
    snprintf(s, 32, "frame-%04d", id);
    return s;
}

static int process(int id)
{
    char *tmp = malloc(64);
    if (!tmp)
        return -1;
    if (id % 2)                         /* BUG #2: error path forgets free(tmp) */
        return -1;
    strcpy(tmp, "ok");
    free(tmp);
    return 0;
}

int main(void)
{
    char *label = NULL;
    for (int i = 0; i < 5; i++) {
        label = make_label(i);          /* BUG #1: previous label is lost, never freed */
        printf("%s\n", label);
    }
    free(label);                        /* only the LAST one is freed */

    int errors = 0;
    for (int i = 0; i < 4; i++)
        errors += process(i) != 0;
    printf("%d errors\n", errors);
    return 0;
}
