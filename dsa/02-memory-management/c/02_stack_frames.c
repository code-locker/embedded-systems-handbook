/*
 * 02_stack_frames.c — how the stack really works: frames, frame pointers, growth, limit, guard page
 *
 * Concept : Each function call pushes a FRAME: return address, saved frame
 *           pointer, saved registers, locals. The stack grows DOWN (toward
 *           lower addresses) on x86_64 and arm64. With frame pointers, each
 *           frame starts with a 16-byte "frame record" {caller's fp, return
 *           address}, forming a linked list that debuggers and perf walk.
 *           The [stack] VMA grows on demand up to RLIMIT_STACK (ulimit -s),
 *           with a guard gap below it so overflow faults instead of
 *           silently overwriting the heap/mmap area.
 * Build   : make run P=dsa/02-memory-management/c/02_stack_frames.c
 * Read    : ../README.md  (section "3.2 Stack frames and the calling convention")
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#if defined(__SANITIZE_ADDRESS__)
/* ASan normally moves address-taken locals to a heap "fake stack" to catch
 * use-after-return. Turn that off here so locals live on the REAL stack.  */
const char *__asan_default_options(void);
const char *__asan_default_options(void) { return "detect_stack_use_after_return=0"; }
#endif

/* Return [lo, hi) of the [stack] VMA from /proc/self/maps. */
static int stack_vma(unsigned long *lo, unsigned long *hi)
{
    FILE *f = fopen("/proc/self/maps", "r");
    char line[512];
    int ok = 0;
    while (f && fgets(line, sizeof line, f))
        if (strstr(line, "[stack]") && sscanf(line, "%lx-%lx", lo, hi) == 2) { ok = 1; break; }
    if (f) fclose(f);
    return ok;
}

/* ---- 1. Frames move DOWN as calls nest ------------------------------------- */
static uintptr_t frame_addr[6];

static __attribute__((noinline)) void nest(int depth)
{
    volatile int local = depth;          /* force a real stack slot */
    frame_addr[depth] = (uintptr_t)__builtin_frame_address(0);
    printf("  depth %d: frame at %#lx, &local = %p\n", depth,
           (unsigned long)frame_addr[depth], (void *)&local);
    if (depth < 5)
        nest(depth + 1);
}

/* ---- 2. Walk the frame-pointer chain by hand (what gdb 'bt' / perf do) ---- */
static __attribute__((noinline)) int walk_chain(void)
{
#if (defined(__x86_64__) || defined(__aarch64__)) && (!defined(__OPTIMIZE__) || defined(__SANITIZE_ADDRESS__))
    /* Frame record layout on both x86_64 (rbp) and arm64 (x29):
     *      fp ──► [ saved caller fp ]   fp + 0
     *             [ return address  ]   fp + 8   (x86: pushed by call; arm64: saved x30/LR)
     */
    uintptr_t *fp = __builtin_frame_address(0);
    uintptr_t saved_fp = fp[0], ret = fp[1];
    assert(ret == (uintptr_t)__builtin_return_address(0));
    assert(saved_fp > (uintptr_t)fp);                 /* caller's frame is HIGHER */
    int frames = 0;
    printf("  frame-pointer chain from walk_chain():\n");
    /* Only 2 levels: walk_chain and main. Beyond main, libc may be built
     * without frame pointers and the chain is not guaranteed to be valid. */
    for (uintptr_t *p = fp; p && frames < 2; p = (uintptr_t *)p[0], frames++)
        printf("    fp=%p  return address=%#lx\n", (void *)p, (unsigned long)p[1]);
    return frames;
#else
    printf("  (frame-pointer walk skipped: needs -fno-omit-frame-pointer on x86_64/arm64)\n");
    return 2;
#endif
}

/* ---- 3. The stack VMA grows on demand -------------------------------------- */
static __attribute__((noinline)) unsigned long eat_stack(int depth)
{
    volatile char big[64 * 1024];        /* 64 KiB per frame */
    memset((char *)big, depth, sizeof big);   /* TOUCH it, so pages are faulted in */
    if (depth == 0) {
        unsigned long lo, hi;
        stack_vma(&lo, &hi);
        return hi - lo;                  /* measured at the deepest point */
    }
    return eat_stack(depth - 1) + (big[0] & 0);
}

/* ---- 4. Why returning the address of a local is a bug ----------------------
 *
 *   int *bad(void) { int x = 5; return &x; }   // gcc: returns address of local
 *
 *   during bad():        after return:              after the next call f():
 *   ┌──────────┐         ┌──────────┐               ┌──────────┐
 *   │ x = 5    │◄─ ret   │ x = 5 ?? │◄─ p           │ f's data │◄─ p  (*p is now
 *   └──────────┘         └──────────┘ (dead frame)  └──────────┘       f's garbage)
 *   The frame is not erased, just "free"; the next call overwrites it.
 *   (gcc actually returns NULL for this at -O0/-O2 to make it crash loudly.)
 */

int main(void)
{
    struct rlimit rl;
    assert(getrlimit(RLIMIT_STACK, &rl) == 0);
    if (rl.rlim_cur == RLIM_INFINITY)
        printf("stack limit (ulimit -s): unlimited\n");
    else
        printf("stack limit (ulimit -s): %lu KiB (soft), main thread\n", (unsigned long)rl.rlim_cur / 1024);

    unsigned long lo, hi;
    assert(stack_vma(&lo, &hi));
    printf("[stack] VMA now: %#lx-%#lx (%lu KiB mapped so far)\n\n", lo, hi, (hi - lo) / 1024);

    printf("1. nested calls (stack grows DOWN):\n");
    nest(0);
    for (int i = 1; i < 6; i++)
        assert(frame_addr[i] < frame_addr[i - 1]);
    printf("  each frame is %ld bytes below the previous one\n\n",
           (long)(frame_addr[0] - frame_addr[1]));

    printf("2. frame records:\n");
    assert(walk_chain() >= 2);

    printf("\n3. on-demand growth: recurse 24 x 64 KiB = 1.5 MiB of locals\n");
    unsigned long before = hi - lo;
    unsigned long during = eat_stack(24);
    printf("  [stack] VMA size: before %lu KiB, at deepest point %lu KiB\n", before / 1024, during / 1024);
    assert(during > before && during >= 24 * 64 * 1024);
    /* Linux never shrinks the [stack] VMA again; the pages stay resident. */

    printf("\n4. a guard gap (stack_guard_gap, 256 pages = 1 MiB by default) sits below\n"
           "   the stack. Run 03-debugging/c/04_stack_overflow_bug.c to hit it.\n");
    printf("\nAll checks passed.\n");
    return 0;
}
