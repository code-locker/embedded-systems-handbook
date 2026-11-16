/*
 * 09_shared_counter_bug.c — INTENTIONAL BUG: two processes increment a
 *                           plain (non-atomic) counter in shared memory and
 *                           lose updates
 *
 * Concept : `counter++` is three steps: load, add, store. Two CPUs doing it
 *           at the same time both load the same old value and both store
 *           old+1: one increment vanishes. Between threads this is the race
 *           from lesson 02; between PROCESSES it is exactly the same,
 *           because the memory is the same. `volatile` does not help: it
 *           only stops the compiler from caching the value, the three steps
 *           are still separate.
 *
 *              CPU0 (proc A)          CPU1 (proc B)        counter
 *              load  41                                      41
 *                                     load  41               41
 *              store 42                                      42
 *                                     store 42               42  <- lost
 *
 * Tool    : the SELF-CHECK. ThreadSanitizer CANNOT see this race: it tracks
 *           memory accesses per process, and each process here has only one
 *           thread, so there is nobody for it to race with. (Helgrind/DRD
 *           are per-process too.) Cross-process races are found by
 *           invariants — here the final count — by stress tests, and by
 *           review. Run it:
 *             make build/linux-internals/11-ipc-shared-memory/c/09_shared_counter_bug
 *             ./build/linux-internals/11-ipc-shared-memory/c/09_shared_counter_bug; echo $?
 *           It prints how many updates were lost and exits 1. (Rarely, if
 *           the two children never overlap on the CPUs, nothing is lost —
 *           it then says so; run it again.) The second half shows the fix:
 *           atomic_fetch_add, always exact.
 *           Not run by `make test` (it is a _bug file).
 * Read    : ../README.md  (section "3.9 The race you cannot see with TSan")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define N_PROCS 2
#define N_INCS 2000000L

struct shared {
    _Atomic int go;         /* start line: makes the children overlap */
    volatile long plain;    /* BUG: non-atomic read-modify-write */
    _Atomic long atomic;    /* FIX */
};

static void run(struct shared *s, int use_atomic)
{
    pid_t pids[N_PROCS];
    atomic_store(&s->go, 0);
    fflush(stdout);
    for (int p = 0; p < N_PROCS; p++) {
        pids[p] = fork();
        if (pids[p] < 0) {
            perror("fork");
            exit(2);
        }
        if (pids[p] == 0) {
            alarm(10);
            while (!atomic_load(&s->go))
                sched_yield(); /* wait at the start line */
            for (long i = 0; i < N_INCS; i++) {
                if (use_atomic)
                    atomic_fetch_add_explicit(&s->atomic, 1,
                                              memory_order_relaxed);
                else
                    s->plain++; /* load; add; store — not atomic */
            }
            _exit(0);
        }
    }
    atomic_store(&s->go, 1); /* both start (nearly) together */
    for (int p = 0; p < N_PROCS; p++) {
        int st;
        if (waitpid(pids[p], &st, 0) != pids[p] || !WIFEXITED(st) ||
            WEXITSTATUS(st) != 0) {
            fprintf(stderr, "child failed\n");
            exit(2);
        }
    }
}

int main(void)
{
    struct shared *s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE,
                            MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    const long want = N_PROCS * N_INCS;

    run(s, 0);
    long lost = want - s->plain;
    printf("plain   counter: %ld (expected %ld) -> %ld updates LOST (%.1f%%)\n",
           s->plain, want, lost, 100.0 * (double)lost / (double)want);

    run(s, 1);
    long a = atomic_load(&s->atomic);
    printf("atomic  counter: %ld (expected %ld) -> exact\n", a, want);

    munmap(s, sizeof *s);
    if (a != want) {
        printf("atomic counter wrong?! (should never happen)\n");
        return 2;
    }
    if (lost > 0) {
        printf("BUG CAUGHT by the self-check: lost updates between "
               "processes (TSan sees nothing here)\n");
        return 1;
    }
    printf("no updates lost this time (the processes did not overlap); "
           "run again\n");
    return 0;
}
