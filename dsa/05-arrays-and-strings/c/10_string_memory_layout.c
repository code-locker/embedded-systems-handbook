/*
 * 10_string_memory_layout.c — where strings live: .rodata, .data, stack, heap
 *
 * Concept : A C string is just bytes ending in '\0'. WHERE those bytes live
 *           decides whether you may write them and how long they survive.
 *           We print the address of each kind of string, look it up in
 *           /proc/self/maps to see the permissions of that region, show that
 *           identical literals may be merged, contrast sizeof with strlen,
 *           and prove (in a throw-away child process) that writing to a
 *           string literal crashes.
 * Build   : make run P=dsa/05-arrays-and-strings/c/10_string_memory_layout.c
 * Read    : ../notes/strings.md  (section "Where strings live in memory")
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/*
 *  Typical layout of a Linux process (addresses grow upwards):
 *
 *   high  +----------------------+
 *         |  stack  (rw-)        |  char local[] = "hi";   <- a COPY lives here
 *         |          v           |
 *         |                      |
 *         |          ^           |
 *         |  heap   (rw-)        |  malloc'd strings
 *         +----------------------+
 *         |  .data / .bss (rw-)  |  char g_buf[] = "hello"; static char s[16];
 *         +----------------------+
 *         |  .rodata (r--)       |  "hello"  <- every string LITERAL
 *         +----------------------+
 *         |  .text   (r-x)       |  machine code
 *   low   +----------------------+
 *
 *  .rodata is mapped READ-ONLY by the loader. The MMU enforces it: a store
 *  to a read-only page raises a page fault, the kernel finds no write
 *  permission on that VMA and delivers SIGSEGV.
 */

char g_array[] = "global array"; /* .data : 13 writable bytes, a COPY        */
const char *g_ptr = "global literal"; /* g_ptr in .data, the bytes in .rodata */
static char g_zeroed[32];        /* .bss  : zero-filled at start, writable    */

/* Look up which line of /proc/self/maps contains address p.
 * Each line looks like:  55d0c2a00000-55d0c2a01000 r--p 00002000 ... /path
 * O(lines) — we just scan the text file the kernel generates on demand.
 * Writes the 4-char permission string ("r--p", "rw-p", ...) into perms. */
static int find_mapping(const void *p, char perms[5], char *name, size_t namesz)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return 0;
    char line[512];
    uintptr_t a = (uintptr_t)p;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char pr[5] = {0};
        int off = 0;
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s%n", &lo, &hi, pr, &off) < 3)
            continue;
        if (a >= lo && a < hi) {
            memcpy(perms, pr, 5);
            /* the rest of the line (after inode) is the path, maybe empty */
            const char *path = line + off;
            while (*path == ' ')
                path++;
            snprintf(name, namesz, "%s", path);
            name[strcspn(name, "\n")] = '\0';
            if (name[0] == '\0')
                snprintf(name, namesz, "(anonymous)");
            /* shorten long paths to the basename for readability */
            const char *slash = strrchr(name, '/');
            if (slash)
                memmove(name, slash + 1, strlen(slash + 1) + 1);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void show(const char *label, const void *p, char perms_out[5])
{
    char perms[5] = "????";
    char name[256] = "?";
    find_mapping(p, perms, name, sizeof name);
    printf("  %-34s %p  %s  %s\n", label, p, perms, name);
    if (perms_out)
        memcpy(perms_out, perms, 5);
}

/* Child process body: try to write into a string literal.
 * `char *p = "..."` compiles in C (literal type is char[N], not const char[N],
 * for historical reasons) — but the bytes are still in .rodata. */
static void write_to_literal(void)
{
    char *p = (char *)(uintptr_t)"read-only!";
    p[0] = 'R'; /* <- page fault -> SIGSEGV (ASan: "SEGV on unknown address") */
    printf("unreachable: %s\n", p);
}

int main(void)
{
    puts("== 1. where each kind of string lives ==");
    char stack_array[] = "stack array";          /* 12 bytes COPIED onto stack */
    const char *lit = "a string literal";        /* pointer to .rodata bytes   */
    static char static_local[] = "static local"; /* .data, lives forever       */
    char *heap = malloc(16);                      /* heap; we own it, we free it */
    CHECK(heap != NULL);
    strcpy(heap, "heap string");

    char p_lit[5], p_stack[5], p_heap[5], p_data[5];
    show("string literal \"a string literal\"", lit, p_lit);
    show("g_ptr -> \"global literal\"", g_ptr, NULL);
    show("char g_array[] (global)", g_array, p_data);
    show("static char static_local[]", static_local, NULL);
    show("static char g_zeroed[32] (.bss)", g_zeroed, NULL);
    show("char stack_array[] (local)", stack_array, p_stack);
    show("malloc(16)", heap, p_heap);
    /* Note: built with ASan, "stack" arrays may show as (anonymous) instead
     * of [stack] and malloc memory is ASan's own heap region — ASan moves
     * locals into a "fake stack" with redzones. Build without sanitizers to
     * see the classic [stack] and [heap] names (make does not track flags,
     * so a plain `make SAN=0` would reuse the ASan binary):
     *   gcc -std=c11 -O0 -g c/10_string_memory_layout.c -o /tmp/layout && /tmp/layout */
    puts("  (under ASan the stack/heap rows show ASan's own regions;"
         " build without -fsanitize to see [stack]/[heap])");

    /* The permission column is the whole story: literals are not writable. */
    CHECK(p_lit[0] == 'r' && p_lit[1] == '-');   /* r--p : read-only        */
    CHECK(p_data[1] == 'w');                     /* rw-p : writable copy    */
    CHECK(p_stack[1] == 'w');
    CHECK(p_heap[1] == 'w');

    puts("\n== 2. char s[] copies, char *p points ==");
    /*
     *   char s[] = "hi";            const char *p = "hi";
     *
     *   stack: s +---+---+----+     stack: p +----------+
     *            | h | i | \0 |              | 0x55..a0 |---+
     *            +---+---+----+              +----------+   |
     *   (initialised by copying     .rodata: +---+---+----+ |
     *    from .rodata at entry)              | h | i | \0 |<+
     *                                        +---+---+----+
     *   s[0] = 'H';  OK              p[0] = 'H';  SIGSEGV (and UB)
     */
    char s[] = "hi";
    s[0] = 'H'; /* fine: our own copy */
    CHECK(strcmp(s, "Hi") == 0);
    printf("  char s[] modified to \"%s\" (our private stack copy)\n", s);

    puts("\n== 3. identical literals may be merged ==");
    const char *a = "merge me";
    const char *b = "merge me";
    /* The standard says this comparison is UNSPECIFIED: the compiler/linker
     * MAY fold identical literals into one copy (gcc does, -fmerge-constants,
     * and the linker merges across files via SHF_MERGE .rodata.str sections).
     * So never compare strings with ==, always strcmp. */
    printf("  \"merge me\" #1 at %p, #2 at %p -> %s\n", (const void *)a,
           (const void *)b, a == b ? "SAME address (merged)" : "different");
    CHECK(strcmp(a, b) == 0); /* the only comparison that is guaranteed */

    puts("\n== 4. sizeof vs strlen ==");
    char buf[64] = "abc";
    const char *ptr = "abc";
    /*  buf: [a][b][c][\0][\0]...[\0]   64 bytes reserved, 3 used
     *  sizeof buf  = 64  (compile time, size of the ARRAY object)
     *  strlen(buf) = 3   (run time, walks until '\0': O(n))
     *  sizeof ptr  = 8   (size of a POINTER on a 64-bit machine!)        */
    printf("  sizeof buf = %zu, strlen(buf) = %zu\n", sizeof buf, strlen(buf));
    printf("  sizeof ptr = %zu, strlen(ptr) = %zu\n", sizeof ptr, strlen(ptr));
    printf("  sizeof \"abc\" = %zu (the literal is char[4]: 3 + '\\0')\n",
           sizeof "abc");
    CHECK(sizeof buf == 64 && strlen(buf) == 3);
    CHECK(sizeof ptr == sizeof(void *));
    CHECK(sizeof "abc" == 4);

    /* Embedded NUL: strlen stops at the FIRST zero byte; the rest is invisible
     * to every str* function. Binary data needs (pointer, length), not '\0'. */
    char embedded[] = "abc\0def";
    CHECK(sizeof embedded == 8 && strlen(embedded) == 3);
    printf("  \"abc\\0def\": sizeof = %zu, strlen = %zu  (strlen stops at the "
           "first NUL)\n", sizeof embedded, strlen(embedded));

    puts("\n== 5. writing to a literal crashes (in a child process) ==");
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* Child: silence the crash report (ASan prints a big SEGV report
         * here; without ASan the shell would just print "Segmentation
         * fault"). This output is EXPECTED, so we throw it away. */
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0)
            dup2(devnull, STDERR_FILENO);
        write_to_literal();
        _exit(0); /* only reached if the write somehow succeeded */
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    /* Without ASan: killed by SIGSEGV. With ASan: its SEGV handler catches
     * the signal, prints a report and exits with code 1. Both = "crashed". */
    int crashed = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
    if (WIFSIGNALED(status))
        printf("  child killed by signal %d (SIGSEGV = 11)\n", WTERMSIG(status));
    else
        printf("  child exited with status %d (sanitizer caught the SEGV)\n",
               WEXITSTATUS(status));
    CHECK(crashed);

    free(heap); /* we malloc'd it, we free it */
    puts("\nall checks passed");
    return 0;
}
