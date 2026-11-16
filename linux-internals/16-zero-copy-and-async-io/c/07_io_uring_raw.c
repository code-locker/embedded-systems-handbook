/*
 * 07_io_uring_raw.c — io_uring with raw syscalls (no liburing): set up the
 *                     rings, mmap them, submit NOP and READ requests in
 *                     batches, reap completions, register an eventfd
 *
 * Concept : io_uring is the kernel's native async I/O interface (5.1+).
 *           Two ring buffers live in memory SHARED between your process and
 *           the kernel: you put Submission Queue Entries (SQEs: "read fd 3,
 *           offset X, len Y, into buf Z") on the SQ, the kernel puts
 *           Completion Queue Entries (CQEs: "request #7 returned 131072")
 *           on the CQ. One io_uring_enter() syscall can submit many SQEs
 *           AND wait for completions — batching amortises the syscall cost,
 *           and the rings themselves need no syscall to read. Each ring is
 *           single-producer/single-consumer: the producer writes the entry,
 *           then publishes the new tail with a RELEASE store; the consumer
 *           reads the tail with ACQUIRE (the same SPSC ring as
 *           dsa/07 section 3.8, with the kernel on the other side).
 *           If io_uring is disabled (sysctl kernel.io_uring_disabled, a
 *           seccomp container, old kernel) the program explains and skips.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/07_io_uring_raw.c
 * Read    : ../README.md  (section 3.8 "io_uring")
 */
#define _GNU_SOURCE /* syscall, mkostemp */
#include <errno.h>
#include <fcntl.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define FSIZE   (4u << 20)
#define BLK     (128u << 10)
#define NBLK    (FSIZE / BLK) /* 32 reads of 128 KiB */
#define QD      8             /* queue depth: requests in flight at once */

/* glibc has no wrappers; these are the three io_uring syscalls. */
static int io_uring_setup(unsigned entries, struct io_uring_params *p)
{
    return (int)syscall(__NR_io_uring_setup, entries, p);
}
static int io_uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags)
{
    return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL, 0);
}
static int io_uring_register(int fd, unsigned op, void *arg, unsigned nr)
{
    return (int)syscall(__NR_io_uring_register, fd, op, arg, nr);
}

/*
 *  shared memory, mapped from the ring fd:
 *
 *   SQ ring (IORING_OFF_SQ_RING)          SQE array (IORING_OFF_SQES)
 *   +------+------+------+-------------+  +--------+--------+-----+
 *   | head | tail | mask | array[idx]..|  | sqe[0] | sqe[1] | ... |
 *   +------+------+------+-------------+  +--------+--------+-----+
 *     ^kernel  ^we      array[] holds indices into the SQE array
 *     advances advance
 *
 *   CQ ring (same mmap as SQ ring when IORING_FEAT_SINGLE_MMAP)
 *   +------+------+------+--------------------------------+
 *   | head | tail | mask | cqe[0] cqe[1] ... {user_data,res,flags}
 *   +------+------+------+--------------------------------+
 *     ^we      ^kernel
 */
struct ring {
    int fd;
    unsigned *sq_head, *sq_tail, *sq_mask, *sq_array;
    unsigned *cq_head, *cq_tail, *cq_mask;
    struct io_uring_sqe *sqes;
    struct io_uring_cqe *cqes;
    void *sq_ptr, *cq_ptr;
    size_t sq_sz, cq_sz, sqes_sz;
    unsigned entries;
    unsigned long enters; /* io_uring_enter calls, for the statistics */
};

static int ring_init(struct ring *r, unsigned entries)
{
    struct io_uring_params p;
    memset(&p, 0, sizeof p);
    memset(r, 0, sizeof *r);
    r->fd = io_uring_setup(entries, &p);
    if (r->fd < 0)
        return -errno;
    r->entries = p.sq_entries;
    r->sq_sz = p.sq_off.array + p.sq_entries * sizeof(unsigned);
    r->cq_sz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
    if (p.features & IORING_FEAT_SINGLE_MMAP) /* 5.4+: one mapping for both rings */
        r->sq_sz = r->cq_sz = r->sq_sz > r->cq_sz ? r->sq_sz : r->cq_sz;
    r->sq_ptr = mmap(NULL, r->sq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
                     IORING_OFF_SQ_RING);
    CHECK(r->sq_ptr != MAP_FAILED);
    if (p.features & IORING_FEAT_SINGLE_MMAP)
        r->cq_ptr = r->sq_ptr;
    else {
        r->cq_ptr = mmap(NULL, r->cq_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
                         IORING_OFF_CQ_RING);
        CHECK(r->cq_ptr != MAP_FAILED);
    }
    r->sqes_sz = p.sq_entries * sizeof(struct io_uring_sqe);
    r->sqes = mmap(NULL, r->sqes_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, r->fd,
                   IORING_OFF_SQES);
    CHECK(r->sqes != MAP_FAILED);

    char *sq = r->sq_ptr, *cq = r->cq_ptr;
    r->sq_head = (unsigned *)(sq + p.sq_off.head);
    r->sq_tail = (unsigned *)(sq + p.sq_off.tail);
    r->sq_mask = (unsigned *)(sq + p.sq_off.ring_mask);
    r->sq_array = (unsigned *)(sq + p.sq_off.array);
    r->cq_head = (unsigned *)(cq + p.cq_off.head);
    r->cq_tail = (unsigned *)(cq + p.cq_off.tail);
    r->cq_mask = (unsigned *)(cq + p.cq_off.ring_mask);
    r->cqes = (struct io_uring_cqe *)(cq + p.cq_off.cqes);
    printf("   ring fd %d: %u SQ entries, %u CQ entries, features 0x%x%s\n", r->fd, p.sq_entries,
           p.cq_entries, p.features,
           (p.features & IORING_FEAT_SINGLE_MMAP) ? " (single mmap)" : "");
    return 0;
}

static void ring_exit(struct ring *r)
{
    munmap(r->sqes, r->sqes_sz);
    if (r->cq_ptr != r->sq_ptr)
        munmap(r->cq_ptr, r->cq_sz);
    munmap(r->sq_ptr, r->sq_sz);
    close(r->fd);
}

/* Get the next free SQE, or NULL if the SQ is full. We are the only SQ
 * producer: our own tail needs no atomic load; the kernel's head does. */
static struct io_uring_sqe *get_sqe(struct ring *r)
{
    unsigned tail = *r->sq_tail;
    unsigned head = __atomic_load_n(r->sq_head, __ATOMIC_ACQUIRE);
    if (tail - head >= r->entries)
        return NULL;
    unsigned idx = tail & *r->sq_mask;
    struct io_uring_sqe *sqe = &r->sqes[idx];
    memset(sqe, 0, sizeof *sqe);
    r->sq_array[idx] = idx;
    return sqe;
}
/* Publish the SQE: RELEASE so the kernel sees the filled entry before the
 * new tail. */
static void commit_sqe(struct ring *r)
{
    __atomic_store_n(r->sq_tail, *r->sq_tail + 1, __ATOMIC_RELEASE);
}

static void prep_read(struct io_uring_sqe *s, int fd, void *buf, unsigned len, uint64_t off,
                      uint64_t user_data)
{
    s->opcode = IORING_OP_READ; /* 5.6+; READV works since 5.1 */
    s->fd = fd;
    s->addr = (uint64_t)(uintptr_t)buf;
    s->len = len;
    s->off = off;
    s->user_data = user_data; /* comes back in the CQE: "which request was this?" */
}

/* Pop one CQE if available. ACQUIRE on the kernel's tail, RELEASE on our
 * head (tells the kernel the slot is free again). */
static int pop_cqe(struct ring *r, struct io_uring_cqe *out)
{
    unsigned head = *r->cq_head;
    if (head == __atomic_load_n(r->cq_tail, __ATOMIC_ACQUIRE))
        return 0;
    *out = r->cqes[head & *r->cq_mask];
    __atomic_store_n(r->cq_head, head + 1, __ATOMIC_RELEASE);
    return 1;
}

static int enter(struct ring *r, unsigned submit, unsigned wait)
{
    int n;
    do
        n = io_uring_enter(r->fd, submit, wait, wait ? IORING_ENTER_GETEVENTS : 0);
    while (n < 0 && errno == EINTR);
    r->enters++;
    return n;
}

static unsigned char pattern(size_t i) { return (unsigned char)(i * 13u + (i >> 12)); }

int main(void)
{
    alarm(20);
    char path[64];
    snprintf(path, sizeof path, "/tmp/zc07-%d-XXXXXX", (int)getpid());
    int fd = mkostemp(path, O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(unlink(path) == 0);
    unsigned char *src = malloc(FSIZE);
    CHECK(src);
    for (size_t i = 0; i < FSIZE; i++)
        src[i] = pattern(i);
    CHECK(pwrite(fd, src, FSIZE, 0) == (ssize_t)FSIZE);

    printf("1) io_uring_setup(%d)\n", QD);
    struct ring r;
    int e = ring_init(&r, QD);
    if (e < 0) {
        char v[16] = "?";
        FILE *f = fopen("/proc/sys/kernel/io_uring_disabled", "r");
        if (f) {
            if (!fgets(v, sizeof v, f))
                v[0] = '\0';
            fclose(f);
        }
        printf("   io_uring_setup: %s. kernel.io_uring_disabled = %s"
               "   (0 = on, 1 = only for io_uring_group, 2 = off; ENOSYS = kernel < 5.1\n"
               "    or a seccomp filter, common in containers). Skipping.\n",
               strerror(-e), v);
        free(src);
        close(fd);
        printf("07_io_uring_raw: all checks passed\n");
        return 0;
    }

    /* ---- 2. a NOP: the smallest round trip through both rings ---- */
    struct io_uring_sqe *s = get_sqe(&r);
    CHECK(s);
    s->opcode = IORING_OP_NOP;
    s->user_data = 0xC0FFEE;
    commit_sqe(&r);
    CHECK(enter(&r, 1, 1) == 1); /* submit 1, wait for 1, in one syscall */
    struct io_uring_cqe cqe;
    CHECK(pop_cqe(&r, &cqe) == 1);
    CHECK(cqe.user_data == 0xC0FFEE && cqe.res == 0);
    printf("2) NOP: submitted and completed in one io_uring_enter, user_data 0x%llx\n",
           (unsigned long long)cqe.user_data);

    /* ---- 3. a batch of 4 reads at different offsets, ONE syscall ---- */
    unsigned char *dst = calloc(1, FSIZE);
    CHECK(dst);
    const uint64_t offs[4] = {3 * BLK, 0, 17 * BLK, 31 * BLK}; /* out of order on purpose */
    for (int i = 0; i < 4; i++) {
        s = get_sqe(&r);
        CHECK(s);
        prep_read(s, fd, dst + offs[i], BLK, offs[i], offs[i]); /* user_data = offset */
        commit_sqe(&r);
    }
    CHECK(enter(&r, 4, 4) == 4);
    int seen = 0;
    printf("3) batch of 4 READs, completion order:");
    while (pop_cqe(&r, &cqe)) {
        CHECK(cqe.res == (int)BLK); /* res = bytes read, or -errno */
        CHECK(memcmp(dst + cqe.user_data, src + cqe.user_data, BLK) == 0);
        printf(" @%llu", (unsigned long long)(cqe.user_data / BLK));
        seen++;
    }
    printf(" (blocks)\n");
    CHECK(seen == 4);

    /* ---- 4. the whole file: 32 reads, keep QD in flight ----
     *   submit up to QD -> enter(submit=n, wait=1) -> reap all ready -> refill */
    memset(dst, 0, FSIZE);
    unsigned long enters0 = r.enters;
    unsigned next = 0, inflight = 0, completed = 0;
    while (completed < NBLK) {
        unsigned n = 0;
        while (next < NBLK && inflight < QD && (s = get_sqe(&r)) != NULL) {
            prep_read(s, fd, dst + (size_t)next * BLK, BLK, (uint64_t)next * BLK, next);
            commit_sqe(&r);
            next++;
            inflight++;
            n++;
        }
        CHECK(enter(&r, n, 1) == (int)n);
        while (pop_cqe(&r, &cqe)) {
            CHECK(cqe.res == (int)BLK);
            inflight--;
            completed++;
        }
    }
    CHECK(memcmp(dst, src, FSIZE) == 0);
    printf("4) %u MiB as %u reads of %u KiB at queue depth %d: %lu io_uring_enter calls\n"
           "   (read() would need %u syscalls), data verified\n",
           FSIZE >> 20, NBLK, BLK >> 10, QD, r.enters - enters0, NBLK);

    /* ---- 5. eventfd registration: completions also signal an eventfd,
     * so an epoll loop (lesson 13) can wait for io_uring and sockets
     * together. ---- */
    int efd = eventfd(0, EFD_CLOEXEC);
    CHECK(efd >= 0);
    if (io_uring_register(r.fd, IORING_REGISTER_EVENTFD, &efd, 1) == 0) {
        s = get_sqe(&r);
        CHECK(s);
        s->opcode = IORING_OP_NOP;
        s->user_data = 5;
        commit_sqe(&r);
        CHECK(enter(&r, 1, 1) == 1);
        uint64_t cnt = 0;
        CHECK(read(efd, &cnt, sizeof cnt) == sizeof cnt); /* blocks until signalled */
        CHECK(pop_cqe(&r, &cqe) == 1 && cqe.user_data == 5);
        printf("5) IORING_REGISTER_EVENTFD: eventfd counter %llu after the completion\n",
               (unsigned long long)cnt);
        CHECK(cnt >= 1);
        CHECK(io_uring_register(r.fd, IORING_UNREGISTER_EVENTFD, NULL, 0) == 0);
    } else {
        printf("5) IORING_REGISTER_EVENTFD: %s (skipped)\n", strerror(errno));
    }
    close(efd);

    ring_exit(&r);
    free(dst);
    free(src);
    close(fd);
    printf("   liburing (io_uring_queue_init / io_uring_get_sqe / io_uring_submit_and_wait)\n"
           "   wraps exactly these steps; see cpp/02 for an RAII version.\n");
    printf("07_io_uring_raw: all checks passed\n");
    return 0;
}
