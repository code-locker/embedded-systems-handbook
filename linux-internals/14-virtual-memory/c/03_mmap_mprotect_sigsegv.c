/*
 * 03_mmap_mprotect_sigsegv.c — the mmap flavours, page protections with
 *                              mprotect(), and turning a SIGSEGV into
 *                              information: guard pages, SEGV_ACCERR vs
 *                              SEGV_MAPERR, recovery with siglongjmp, and
 *                              write tracking by un-protecting on fault
 *
 * Concept : Every mapping (VMA) has permissions: PROT_READ, PROT_WRITE,
 *           PROT_EXEC or PROT_NONE. An access the permissions forbid, or an
 *           access to an address with no mapping at all, makes the MMU
 *           fault; the kernel finds no way to fix it and sends SIGSEGV with
 *           si_addr = the faulting address and si_code = SEGV_ACCERR
 *           (mapped, wrong permission) or SEGV_MAPERR (nothing mapped).
 *           A PROT_NONE page next to a buffer is a GUARD PAGE: overflowing
 *           into it crashes at once instead of corrupting memory. A handler
 *           can recover (siglongjmp back to a safe point) or FIX the cause
 *           (mprotect the page writable and return: the instruction is
 *           retried) — that is how garbage collectors, checkpointing and
 *           some databases track which pages were written.
 * Build   : make run P=linux-internals/14-virtual-memory/c/03_mmap_mprotect_sigsegv.c
 * Read    : ../README.md  (sections "3.6 mmap flavours" and "3.7 mprotect,
 *           guard pages and SIGSEGV")
 */
#define _GNU_SOURCE /* MAP_ANONYMOUS, SEGV_ACCERR, mkstemp */
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

static long g_page;

/* ---- the SIGSEGV handler: two personalities ---- */
enum mode { RECOVER, TRACK_WRITES };
static volatile sig_atomic_t g_mode = RECOVER;
static sigjmp_buf g_env;
static void *volatile g_fault_addr;
static volatile int g_fault_code;

/* write tracking state */
#define TRACK_PAGES 16
static char *volatile g_track_base;
static volatile sig_atomic_t g_dirty[TRACK_PAGES];
static volatile sig_atomic_t g_track_faults;

static void on_segv(int sig, siginfo_t *si, void *uctx)
{
    (void)sig;
    (void)uctx;
    if (g_mode == TRACK_WRITES) {
        uintptr_t a = (uintptr_t)si->si_addr;
        uintptr_t base = (uintptr_t)g_track_base;
        if (a >= base && a < base + TRACK_PAGES * (uintptr_t)g_page) {
            size_t i = (a - base) / (size_t)g_page;
            g_dirty[i] = 1;
            g_track_faults++;
            /* Fix the cause and RETURN: the CPU re-executes the store,
             * which now succeeds. (mprotect is a plain syscall; Linux
             * handles it fine here, though POSIX does not list it as
             * async-signal-safe.) */
            mprotect((void *)(base + i * (size_t)g_page), (size_t)g_page,
                     PROT_READ | PROT_WRITE);
            return;
        }
        /* not ours: fall through and recover so the test fails cleanly */
    }
    g_fault_addr = si->si_addr;
    g_fault_code = si->si_code;
    /* Jump back to the sigsetjmp point. siglongjmp (not longjmp) also
     * restores the signal mask, so SIGSEGV is unblocked again. */
    siglongjmp(g_env, 1);
}

/* Try one store; return 1 if it faulted. The volatile pointer stops the
 * compiler from moving or dropping the store. */
static int store_faults(volatile char *p, char v)
{
    g_fault_addr = NULL;
    g_fault_code = 0;
    if (sigsetjmp(g_env, 1) == 0) {
        *p = v;
        return 0;
    }
    return 1; /* came back from the handler */
}

static int load_faults(volatile char *p, char *out)
{
    if (sigsetjmp(g_env, 1) == 0) {
        *out = *p;
        return 0;
    }
    return 1;
}

static const char *code_name(int c)
{
    return c == SEGV_ACCERR ? "SEGV_ACCERR" : c == SEGV_MAPERR ? "SEGV_MAPERR"
                                                               : "other";
}

/* ---- 1. private vs shared file mappings, read-only mappings ---- */
static void demo_flavours(void)
{
    char path[64];
    snprintf(path, sizeof path, "/tmp/vm_mmap_%d_XXXXXX", (int)getpid());
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(ftruncate(fd, g_page) == 0);
    CHECK(pwrite(fd, "original", 8, 0) == 8);

    /* MAP_PRIVATE: reads come from the file, the first write makes a
     * private copy of the page (COW). The file never changes. */
    char *priv = mmap(NULL, (size_t)g_page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE, fd, 0);
    /* MAP_SHARED: the page-cache page itself; writes reach the file. */
    char *shrd = mmap(NULL, (size_t)g_page, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    CHECK(priv != MAP_FAILED && shrd != MAP_FAILED);
    memcpy(priv, "PRIVATE!", 8);
    char back[9] = {0};
    CHECK(pread(fd, back, 8, 0) == 8);
    printf("1) MAP_PRIVATE write  -> file still \"%s\"\n", back);
    CHECK(memcmp(back, "original", 8) == 0);
    memcpy(shrd, "SHARED!!", 8);
    CHECK(pread(fd, back, 8, 0) == 8);
    printf("   MAP_SHARED  write  -> file now   \"%s\" (msync for "
           "durability; lesson 11 has more)\n", back);
    CHECK(memcmp(back, "SHARED!!", 8) == 0);
    /* The private copy is detached: it keeps its own bytes. */
    CHECK(memcmp(priv, "PRIVATE!", 8) == 0);

    /* PROT_READ mapping: loads fine, stores fault with SEGV_ACCERR. */
    char *ro = mmap(NULL, (size_t)g_page, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(ro != MAP_FAILED);
    char c = 0;
    CHECK(load_faults(ro, &c) == 0 && c == 'S');
    CHECK(store_faults(ro, 'x') == 1);
    printf("   PROT_READ mapping: load ok ('%c'), store -> SIGSEGV %s at "
           "%p\n", c, code_name(g_fault_code), g_fault_addr);
    CHECK(g_fault_code == SEGV_ACCERR && g_fault_addr == ro);

    munmap(priv, (size_t)g_page);
    munmap(shrd, (size_t)g_page);
    munmap(ro, (size_t)g_page);
    close(fd);
    CHECK(unlink(path) == 0);
}

/* ---- 2. a guard page and a hole ----
 *
 *   base         +1 page        +2 pages       +3 pages
 *   [ RW data   ][ PROT_NONE   ][ (unmapped)  ]
 *                  guard page     hole: munmap'd
 *   store here ->  SEGV_ACCERR    SEGV_MAPERR
 */
static void demo_guard_and_hole(void)
{
    char *base = mmap(NULL, 3 * (size_t)g_page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(base != MAP_FAILED);
    char *guard = base + g_page, *hole = base + 2 * g_page;
    CHECK(mprotect(guard, (size_t)g_page, PROT_NONE) == 0);
    CHECK(munmap(hole, (size_t)g_page) == 0);

    /* Overflow a "buffer" that ends at the guard page, byte by byte. */
    int written = 0;
    for (int i = 0; i < 2 * g_page; i++) {
        if (store_faults(base + i, 'A'))
            break;
        written++;
    }
    printf("2) overflowing a %ld-byte buffer: %d bytes written, then SIGSEGV "
           "%s at %p (= guard page %p)\n", g_page, written,
           code_name(g_fault_code), g_fault_addr, (void *)guard);
    CHECK(written == g_page);
    CHECK(g_fault_code == SEGV_ACCERR && g_fault_addr == guard);

    CHECK(store_faults(hole + 100, 'B') == 1);
    printf("   store into the munmap'd hole: SIGSEGV %s at %p\n",
           code_name(g_fault_code), g_fault_addr);
    CHECK(g_fault_code == SEGV_MAPERR && g_fault_addr == hole + 100);

    CHECK(munmap(base, 2 * (size_t)g_page) == 0);
}

/* ---- 3. write tracking: protect, fault, record, unprotect, retry ----
 *
 *   mprotect(all, PROT_READ)
 *   store to page 3 -> #PF -> SIGSEGV -> handler: dirty[3]=1,
 *                      mprotect(page 3, RW) -> return -> store retried, ok
 *   store to page 3 again -> no fault (already writable)
 */
static void demo_track_writes(void)
{
    size_t len = TRACK_PAGES * (size_t)g_page;
    char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(buf != MAP_FAILED);
    memset(buf, 0, len);
    for (int i = 0; i < TRACK_PAGES; i++)
        g_dirty[i] = 0;
    g_track_faults = 0;
    g_track_base = buf;
    CHECK(mprotect(buf, len, PROT_READ) == 0); /* "clean" snapshot point */
    g_mode = TRACK_WRITES;

    const int pages_written[] = {3, 7, 3, 12, 7, 3};
    for (size_t k = 0; k < sizeof pages_written / sizeof *pages_written; k++)
        ((volatile char *)buf)[pages_written[k] * g_page + 17] = (char)(k + 1);
    g_mode = RECOVER;

    printf("3) 6 stores to pages {3,7,3,12,7,3}: %d faults; dirty pages:",
           (int)g_track_faults);
    int ndirty = 0;
    for (int i = 0; i < TRACK_PAGES; i++)
        if (g_dirty[i]) {
            printf(" %d", i);
            ndirty++;
        }
    printf("\n");
    CHECK(g_track_faults == 3 && ndirty == 3);
    CHECK(g_dirty[3] && g_dirty[7] && g_dirty[12]);
    CHECK(buf[3 * g_page + 17] == 6 && buf[12 * g_page + 17] == 4);
    printf("   only the first store to each page faulted; the data is all "
           "there (soft-dirty bits and userfaultfd do this in the kernel)\n");
    CHECK(munmap(buf, len) == 0);
}

int main(void)
{
    alarm(10);
    g_page = sysconf(_SC_PAGESIZE);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO; /* we need si_addr and si_code */
    sigemptyset(&sa.sa_mask);
    CHECK(sigaction(SIGSEGV, &sa, NULL) == 0);

    demo_flavours();
    demo_guard_and_hole();
    demo_track_writes();

    signal(SIGSEGV, SIG_DFL);
    printf("03_mmap_mprotect_sigsegv: all checks passed\n");
    return 0;
}
