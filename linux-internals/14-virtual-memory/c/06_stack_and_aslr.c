/*
 * 06_stack_and_aslr.c — how the main stack grows on demand, the guard page
 *                       under every thread stack, and address-space layout
 *                       randomisation (ASLR) seen by re-executing ourselves
 *                       with and without it
 *
 * Concept : The main thread's stack is a VMA marked to grow DOWN: touching
 *           just below it makes the kernel extend it, up to RLIMIT_STACK
 *           (8 MiB default), and it keeps a gap (stack_guard_gap, 1 MiB) to
 *           the next mapping. Thread stacks are fixed-size mmaps that glibc
 *           gives a PROT_NONE guard page at the low end, so an overflow
 *           faults instead of running into the neighbouring memory.
 *           ASLR puts the stack, heap, mmap area, shared libraries and (for
 *           PIE programs) the executable at random addresses at every
 *           exec(), so an exploit cannot hard-code addresses.
 *           `setarch -R` (personality ADDR_NO_RANDOMIZE) turns it off for
 *           one process tree — which is exactly what the Makefile does for
 *           ThreadSanitizer, whose fixed shadow layout cannot cope with the
 *           32-bit mmap randomisation of recent kernels.
 * Build   : make run P=linux-internals/14-virtual-memory/c/06_stack_and_aslr.c
 * Read    : ../README.md  (sections "3.13 Stack growth and guard pages" and
 *           "3.14 ASLR")
 */
#define _GNU_SOURCE /* personality flags, pthread_getattr_np */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define CHILD_ENV "VM14_ASLR_CHILD"

struct vma {
    uintptr_t lo, hi;
    char perms[5];
    char name[128];
};

static int find_vma(uintptr_t addr, struct vma *out)
{
    FILE *f = fopen("/proc/self/maps", "r");
    CHECK(f != NULL);
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[5] = {0};
        int pos = 0;
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &lo, &hi, perms,
                   &pos) < 3)
            continue;
        if (addr >= lo && addr < hi) {
            out->lo = lo;
            out->hi = hi;
            memcpy(out->perms, perms, sizeof perms);
            snprintf(out->name, sizeof out->name, "%s",
                     pos > 0 ? line + pos : "");
            out->name[strcspn(out->name, "\n")] = '\0';
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* ---- 1. the main stack grows down, on demand ----
 *
 *   high  +------------------+  <- top of [stack] (argv, envp, auxv above)
 *         | main's frame     |
 *         | dive(1) frame    |   each call: SP -= frame size
 *         | dive(2) frame    |
 *         |   ...            |
 *         | dive(256) frame  |  <- touching below the VMA start: page fault,
 *         +------------------+     the kernel moves the start down (grows)
 *   low   |  stack_guard_gap |     (up to RLIMIT_STACK, then SIGSEGV)
 */
static struct vma g_deep_vma;
static uintptr_t g_deepest;

static int dive(int depth)
{
    volatile char pad[4096]; /* one page per frame */
    pad[0] = (char)depth;
    pad[sizeof pad - 1] = (char)depth;
    if (depth == 256) {
        g_deepest = (uintptr_t)&pad[0];
        CHECK(find_vma(g_deepest, &g_deep_vma));
        return pad[0];
    }
    return dive(depth + 1) + pad[0] - pad[sizeof pad - 1];
}

static void demo_main_stack(void)
{
    struct rlimit rl;
    CHECK(getrlimit(RLIMIT_STACK, &rl) == 0);
    int local = 0;
    struct vma before;
    CHECK(find_vma((uintptr_t)&local, &before));
    CHECK(strcmp(before.name, "[stack]") == 0);
    dive(1);
    printf("1) RLIMIT_STACK = %llu KiB. [stack] VMA was %lu KiB; at 256 "
           "nested 4 KiB frames it is %lu KiB\n",
           (unsigned long long)rl.rlim_cur / 1024,
           (unsigned long)(before.hi - before.lo) / 1024,
           (unsigned long)(g_deep_vma.hi - g_deep_vma.lo) / 1024);
    printf("   deepest frame at %#lx is %lu KiB below main's local at %p: "
           "the stack grows DOWN\n", (unsigned long)g_deepest,
           (unsigned long)((uintptr_t)&local - g_deepest) / 1024,
           (void *)&local);
    CHECK(g_deepest < (uintptr_t)&local);
    CHECK((uintptr_t)&local - g_deepest >= 256 * 4096);
    CHECK(g_deep_vma.lo <= g_deepest && g_deep_vma.hi == before.hi);
}

/* ---- 2. a thread stack and its guard page ----
 *
 *   [ guard: ---p, 1 page ][ thread stack: rw-p, 8 MiB ... SP starts here ]
 *   overflow runs DOWN into the guard -> SIGSEGV at once
 */
struct tinfo {
    struct vma stack, below;
    size_t guard;
    int ok;
};

static void *thread_fn(void *arg)
{
    struct tinfo *ti = arg;
    int local = 0;
    pthread_attr_t a;
    CHECK(pthread_getattr_np(pthread_self(), &a) == 0);
    CHECK(pthread_attr_getguardsize(&a, &ti->guard) == 0);
    pthread_attr_destroy(&a);
    ti->ok = find_vma((uintptr_t)&local, &ti->stack) &&
             find_vma(ti->stack.lo - 1, &ti->below);
    return NULL;
}

static void demo_thread_guard(void)
{
    struct tinfo ti;
    memset(&ti, 0, sizeof ti);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, thread_fn, &ti) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(ti.ok);
    printf("2) thread stack VMA %#lx-%#lx %s (%lu KiB); just below it: "
           "%lu bytes %s = guard page (guardsize %zu)\n",
           (unsigned long)ti.stack.lo, (unsigned long)ti.stack.hi,
           ti.stack.perms, (unsigned long)(ti.stack.hi - ti.stack.lo) / 1024,
           (unsigned long)(ti.below.hi - ti.below.lo), ti.below.perms,
           ti.guard);
    CHECK(strcmp(ti.below.perms, "---p") == 0); /* no access at all */
    CHECK(ti.below.hi == ti.stack.lo);          /* directly adjacent */
    CHECK(ti.guard >= 4096);
}

/* ---- 3. ASLR ---- */

/* Child mode: print where things landed in this fresh process image. */
static int child_print(void)
{
    int local = 0;
    void *heap = malloc(64);
    void *map = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("stack=%p heap=%p mmap=%p exe=%p libc=%p\n", (void *)&local, heap,
           map, (void *)(uintptr_t)child_print, (void *)(uintptr_t)getpid);
    free(heap);
    munmap(map, 4096);
    return 0;
}

/* Re-exec ourselves with or without randomisation; capture its line. */
static int run_child(int no_random, char *out, size_t cap)
{
    int p[2];
    CHECK(pipe(p) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(10);
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        int cur = personality(0xffffffff); /* query */
        int want = no_random ? (cur | ADDR_NO_RANDOMIZE)
                             : (cur & ~ADDR_NO_RANDOMIZE);
        if (personality((unsigned long)want) == -1) /* inherited by exec */
            _exit(2);
        setenv(CHILD_ENV, "1", 1);
        char *argv[] = {(char *)"06_stack_and_aslr", NULL};
        execv("/proc/self/exe", argv);
        _exit(3);
    }
    close(p[1]);
    size_t got = 0;
    ssize_t r;
    while (got < cap - 1 && (r = read(p[0], out + got, cap - 1 - got)) > 0)
        got += (size_t)r;
    out[got] = '\0';
    out[strcspn(out, "\n")] = '\0';
    close(p[0]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 && got > 0;
}

static void demo_aslr(void)
{
    FILE *f = fopen("/proc/sys/kernel/randomize_va_space", "r");
    int level = -1;
    if (f) {
        if (fscanf(f, "%d", &level) != 1)
            level = -1;
        fclose(f);
    }
    int pers = personality(0xffffffff);
    printf("3) kernel.randomize_va_space = %d (0 off, 1 stack/mmap/vdso, 2 + "
           "heap/brk)\n", level);
    printf("   this process: ADDR_NO_RANDOMIZE %s\n",
           (pers & ADDR_NO_RANDOMIZE)
               ? "SET -> ASLR is off for us (started via `setarch -R`, as "
                 "`make run SAN=tsan` does)"
               : "clear -> normal randomised layout");

    char a[256], b[256];
    CHECK(run_child(1, a, sizeof a));
    CHECK(run_child(1, b, sizeof b));
    printf("   two execs WITHOUT ASLR:\n     %s\n     %s\n", a, b);
    CHECK(strcmp(a, b) == 0); /* identical layout, every time */

#if defined(__SANITIZE_THREAD__)
    printf("   two execs WITH ASLR: skipped under TSan (its runtime would "
           "abort: \"unexpected memory mapping\")\n");
#else
    if (!run_child(0, a, sizeof a) || !run_child(0, b, sizeof b)) {
        printf("   two execs WITH ASLR: child failed (a sanitizer runtime "
               "that needs a fixed layout?) -> skipped\n");
        return;
    }
    printf("   two execs WITH ASLR:\n     %s\n     %s\n", a, b);
    printf("   -> %s\n", strcmp(a, b) != 0
                             ? "different addresses on every exec"
                             : "same (randomize_va_space is 0?)");
    if (level > 0)
        CHECK(strcmp(a, b) != 0);
#endif
}

int main(void)
{
    if (getenv(CHILD_ENV))
        return child_print();
    alarm(20);
    demo_main_stack();
    demo_thread_guard();
    demo_aslr();
    printf("06_stack_and_aslr: all checks passed\n");
    return 0;
}
