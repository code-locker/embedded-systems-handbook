/*
 * 07_shm_spsc_ring.c — a lock-free single-producer/single-consumer ring
 *                      buffer in POSIX shared memory, between two PROCESSES,
 *                      with C11 atomics (acquire/release)
 *
 * Concept : The SPSC ring from dsa/07 (section 3.8) works unchanged between
 *           processes, because a lock-free atomic operates on the MEMORY
 *           location, not on anything process-local: two processes mapping
 *           the same page get the same cache line and the same coherency
 *           protocol. Rules for that to hold:
 *             - the atomics must be LOCK-FREE (a lock-based fallback uses a
 *               lock table private to each process -> no exclusion at all),
 *             - the ring contains no pointers, only indices,
 *             - producer and consumer each own one index, on its own cache
 *               line (no false sharing).
 *           This is the shape of every zero-copy frame/sample transport:
 *           GStreamer shmsink/shmsrc, PipeWire, io_uring's SQ/CQ rings,
 *           RPMsg vrings between the A72 and R5F cores of the TDA4VM.
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/07_shm_spsc_ring.c
 * Read    : ../README.md  (section "3.7 Lock-free SPSC ring across processes")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
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

#define RING_SIZE 256u /* power of two */
#define MASK (RING_SIZE - 1)
#define N_ITEMS 100000u
#define CACHE_LINE 64

/* Cross-process atomics must not depend on a per-process lock table. */
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "need always-lock-free 32-bit atomics");

/* One slot = one "sample". value and check are written together and must
 * always match: a torn/stale read shows up as a mismatch. */
struct sample {
    uint32_t seq;
    uint32_t pad;
    uint64_t value;
    uint64_t check; /* = ~value */
};

/*
 * Shared layout (offsets are the same in every process — the ADDRESSES
 * are not, which is why there is no pointer anywhere in here):
 *
 *   offset 0     : head  (written only by the consumer)  [own cache line]
 *   offset 64    : tail  (written only by the producer)  [own cache line]
 *   offset 128   : slots[256] of struct sample (24 B each)
 */
struct shm_ring {
    _Alignas(CACHE_LINE) _Atomic uint32_t head;
    _Alignas(CACHE_LINE) _Atomic uint32_t tail;
    _Alignas(CACHE_LINE) struct sample slots[RING_SIZE];
};

_Static_assert(offsetof(struct shm_ring, tail) == CACHE_LINE, "layout");
_Static_assert(offsetof(struct shm_ring, slots) == 2 * CACHE_LINE, "layout");

static bool ring_push(struct shm_ring *r, const struct sample *s)
{
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t - h == RING_SIZE)
        return false;                    /* full */
    r->slots[t & MASK] = *s;             /* (1) write the data      */
    atomic_store_explicit(&r->tail, t + 1, memory_order_release); /* (2) */
    return true;
}

static bool ring_pop(struct shm_ring *r, struct sample *out)
{
    uint32_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&r->tail, memory_order_acquire); /* (3) */
    if (t == h)
        return false;                    /* empty */
    *out = r->slots[h & MASK];           /* (4) read the data */
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return true;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Back off politely while the ring is full/empty, and give up after 3 s
 * so a dead peer fails the test instead of spinning forever. */
static void backoff(double start, unsigned long *spins)
{
    (*spins)++;
    sched_yield();
    if ((*spins & 1023) == 0)
        CHECK(now_s() - start < 3.0);
}

static struct shm_ring *map_ring(const char *name, int oflag)
{
    int fd = shm_open(name, oflag, 0600);
    CHECK(fd >= 0);
    if (oflag & O_CREAT)
        CHECK(ftruncate(fd, sizeof(struct shm_ring)) == 0);
    struct shm_ring *r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, 0);
    CHECK(r != MAP_FAILED);
    close(fd);
    /* mmap returns page-aligned memory, so the 64-byte alignment of the
     * struct members holds in every process. */
    CHECK(((uintptr_t)r % CACHE_LINE) == 0);
    return r;
}

int main(void)
{
    alarm(15);
    char name[64];
    snprintf(name, sizeof name, "/spsc_ring_%d", (int)getpid());

    struct shm_ring *ring = map_ring(name, O_CREAT | O_EXCL | O_RDWR);
    /* Initialise BEFORE the other side can look at it. (ftruncate already
     * zero-filled; atomic_init documents intent.) */
    atomic_init(&ring->head, 0);
    atomic_init(&ring->tail, 0);
    printf("ring \"%s\": %u slots x %zu B, struct size %zu B, head@+%zu "
           "tail@+%zu slots@+%zu\n", name, RING_SIZE, sizeof(struct sample),
           sizeof(struct shm_ring), offsetof(struct shm_ring, head),
           offsetof(struct shm_ring, tail), offsetof(struct shm_ring, slots));

    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* ---- producer process: maps the ring by NAME (its own mapping,
         * possibly at another address) ---- */
        alarm(10);
        struct shm_ring *r = map_ring(name, O_RDWR);
        unsigned long spins = 0;
        double start = now_s();
        for (uint32_t i = 0; i < N_ITEMS; i++) {
            uint64_t v = (uint64_t)i * 2654435761u;
            struct sample s = {.seq = i, .value = v, .check = ~v};
            while (!ring_push(r, &s))
                backoff(start, &spins);
        }
        printf("producer: pushed %u samples, found the ring full %lu times "
               "(timing-dependent)\n", N_ITEMS, spins);
        fflush(stdout);
        munmap(r, sizeof *r);
        _exit(0);
    }

    /* ---- consumer: the parent ---- */
    unsigned long spins = 0;
    double start = now_s();
    uint32_t expect = 0;
    struct sample s;
    while (expect < N_ITEMS) {
        if (!ring_pop(ring, &s)) {
            backoff(start, &spins);
            continue;
        }
        CHECK(s.seq == expect);                   /* order, no loss/dup */
        CHECK(s.value == (uint64_t)expect * 2654435761u);
        CHECK(s.check == ~s.value);               /* no torn slot */
        expect++;
    }
    double secs = now_s() - start;
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(atomic_load(&ring->head) == N_ITEMS);
    CHECK(atomic_load(&ring->tail) == N_ITEMS);
    printf("consumer: %u samples in order, every slot consistent; empty "
           "%lu times; %.0f k samples/s (timing-dependent)\n",
           expect, spins, expect / secs / 1000);

    CHECK(munmap(ring, sizeof *ring) == 0);
    CHECK(shm_unlink(name) == 0);
    printf("07_shm_spsc_ring: all checks passed\n");
    return 0;
}
