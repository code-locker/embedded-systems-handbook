/*
 * 07_use_after_munmap_bug.c — INTENTIONAL BUG: a pointer into a mapping is
 *                             used after munmap(); first it crashes, then —
 *                             worse — the address is reused by a new mapping
 *                             and the stale write silently corrupts it
 *
 * Concept : munmap() removes the VMA and its page-table entries. A stale
 *           pointer into that range now hits one of two cases:
 *             (a) nothing is mapped there -> SIGSEGV (SEGV_MAPERR). Lucky.
 *             (b) a LATER mmap() got the same address (the kernel happily
 *                 reuses the hole, top-down) -> the store lands in the new,
 *                 unrelated mapping. No crash, just corrupted data.
 *           This is use-after-free at the page level. It happens with
 *           mmap'd camera/DMA buffers, shared memory, and file mappings
 *           kept in a cache after the owner unmapped them.
 *
 *             mmap  -> A [config v1 ]     cfg = A
 *             munmap(A)                   cfg still = A   (dangling)
 *             mmap  -> A [camera frame]   other = A  (same address!)
 *             cfg[0] = 'X'  -> other now "Xamera frame"
 *
 * Tool    : NOT AddressSanitizer's use-after-free check: ASan tracks
 *           malloc/free, not mmap/munmap, so case (a) shows up only as a
 *           generic "SEGV on unknown address" and case (b) not at all.
 *           What catches it:
 *             - the kernel: SIGSEGV with SEGV_MAPERR in case (a) (shown in
 *               a child process below);
 *             - the SELF-CHECK in case (b): the program compares the new
 *               mapping's contents and exits 1;
 *             - design: set the pointer to NULL right after munmap, wrap
 *               mappings in an RAII owner (../cpp/01_mapping_raii.cpp), or
 *               keep the range reserved with PROT_NONE instead of
 *               unmapping it (then every stale access faults).
 *           Run it:
 *             make run P=linux-internals/14-virtual-memory/c/07_use_after_munmap_bug.c; echo $?
 *           Not run by `make test` (it is a _bug file).
 * Read    : ../README.md  (section "7. Common mistakes and pitfalls")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    alarm(10);
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    /* ---- (a) stale pointer, nothing mapped there: SIGSEGV ---- */
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return 2;
    if (pid == 0) {
        alarm(5);
        char *buf = mmap(NULL, page, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED)
            _exit(2);
        strcpy(buf, "frame 1");
        munmap(buf, page);
        printf("(a) child: munmap'd %p, now writing through the stale "
               "pointer...\n", (void *)buf);
        fflush(stdout);
        ((volatile char *)buf)[0] = 'X'; /* BUG: use after munmap */
        printf("(a) child: no crash?!\n");
        _exit(0);
    }
    int st;
    if (waitpid(pid, &st, 0) != pid)
        return 2;
    if (WIFSIGNALED(st))
        printf("(a) child killed by signal %d (%s): the kernel caught it\n",
               WTERMSIG(st), strsignal(WTERMSIG(st)));
    else
        printf("(a) child exited with status %d (a sanitizer turned the "
               "SIGSEGV into a report and exit)\n", WEXITSTATUS(st));

    /* ---- (b) the address gets reused: silent corruption ---- */
    char *cfg = mmap(NULL, page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cfg == MAP_FAILED)
        return 2;
    strcpy(cfg, "config v1");
    munmap(cfg, page); /* BUG starts here: cfg is not cleared */

    char *other = mmap(NULL, page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (other == MAP_FAILED)
        return 2;
    strcpy(other, "camera frame");
    printf("(b) old mapping was %p, new mapping is %p%s\n", (void *)cfg,
           (void *)other, cfg == other ? "  <- SAME ADDRESS" : "");

    if (cfg != other) {
        printf("(b) address not reused this time; the stale write would "
               "fault like (a). Run again.\n");
        munmap(other, page);
        return 0;
    }
    ((volatile char *)cfg)[0] = 'X'; /* BUG: lands in `other` */
    printf("(b) wrote through the stale config pointer; the camera frame "
           "now reads \"%s\"\n", other);
    int corrupted = strcmp(other, "camera frame") != 0;
    munmap(other, page);
    if (corrupted) {
        printf("BUG CAUGHT by the self-check: use after munmap corrupted an "
               "unrelated mapping, with no crash and no ASan report\n");
        return 1;
    }
    return 0;
}
