/*
 * 08_offsets_not_pointers.c — data structures inside shared memory: store
 *                             OFFSETS, not pointers; plus a header, a bump
 *                             allocator, sizing and alignment
 *
 * Concept : The same shared object is mapped at DIFFERENT virtual addresses
 *           in different processes (even twice in one process). A pointer
 *           stored in shared memory is an address in the WRITER's address
 *           space: in any other mapping it points to the wrong place —
 *           someone else's data, or unmapped memory (SIGSEGV). Store the
 *           distance from the start of the region instead and convert with
 *           base + offset in each process. (Boost.Interprocess calls this
 *           offset_ptr; the kernel/user io_uring rings use offsets too.)
 * Build   : make run P=linux-internals/11-ipc-shared-memory/c/08_offsets_not_pointers.c
 * Read    : ../README.md  (section "3.8 Pointers in shared memory")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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

#define REGION_SIZE (64 * 1024)
#define N_NODES 10
#define MAGIC 0x4F464653u /* "OFFS" */
#define VERSION 1u

typedef uint32_t shm_off; /* 0 = "null" (offset 0 is the header anyway) */

/*
 * Region layout:
 *
 *   base+0       header  { magic, version, size, used, head_ptr, head_off }
 *   base+64      node    { value, next_ptr, next_off }  <- bump-allocated
 *   base+96      node    ...
 *   ...
 *   base+used    free space ...                       base+size
 *
 * A header with magic/version/size lets a process that maps the region
 * check that it really is what it expects (and the same layout version!)
 * before trusting a single byte of it.
 */
struct header {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    uint32_t used;          /* bump allocator: next free offset */
    struct node *head_ptr;  /* WRONG: an address, valid only in the writer */
    shm_off head_off;       /* RIGHT: position relative to the base */
};

struct node {
    int64_t value;
    struct node *next_ptr;  /* WRONG */
    shm_off next_off;       /* RIGHT */
};

/* Convert between offsets and pointers for THIS process's mapping. */
static void *off2ptr(void *base, shm_off off)
{
    return off ? (char *)base + off : NULL;
}
static shm_off ptr2off(void *base, const void *p)
{
    return p ? (shm_off)((const char *)p - (const char *)base) : 0;
}

/* Bump allocator inside the region: returns an OFFSET, aligned to `align`
 * (a power of two). Unaligned objects are slow on ARM, and unaligned
 * atomics/LDREX/STREX fault (alignment fault -> SIGBUS). */
static shm_off shm_alloc(void *base, size_t size, size_t align)
{
    struct header *h = base;
    size_t off = ((size_t)h->used + align - 1) & ~(align - 1);
    if (off + size > h->size)
        return 0;
    h->used = (uint32_t)(off + size);
    return (shm_off)off;
}

static void *map_obj(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);
    CHECK(fd >= 0);
    void *p = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    close(fd);
    return p;
}

static int64_t sum_by_offsets(void *base)
{
    struct header *h = base;
    CHECK(h->magic == MAGIC && h->version == VERSION);
    int64_t sum = 0;
    for (struct node *n = off2ptr(base, h->head_off); n;
         n = off2ptr(base, n->next_off))
        sum += n->value;
    return sum;
}

int main(void)
{
    alarm(10);
    char name[64];
    snprintf(name, sizeof name, "/offsets_demo_%d", (int)getpid());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    /* Size: the kernel maps whole pages. REGION_SIZE is a multiple of the
     * page size; touching beyond the object's size (even inside the last
     * mapped page's range past EOF) -> SIGBUS, so size it generously. */
    long page = sysconf(_SC_PAGESIZE);
    CHECK(REGION_SIZE % page == 0);
    CHECK(ftruncate(fd, REGION_SIZE) == 0);
    close(fd);

    /* Map it TWICE in this process: two different addresses, same pages —
     * exactly the situation of two processes. */
    void *a = map_obj(name);
    void *b = map_obj(name);
    printf("mapping A at %p, mapping B at %p (same object)\n", a, b);
    CHECK(a != b);

    /* Build a list through mapping A, storing both kinds of links. */
    struct header *h = a;
    h->magic = MAGIC;
    h->version = VERSION;
    h->size = REGION_SIZE;
    h->used = sizeof(struct header);
    h->head_ptr = NULL;
    h->head_off = 0;
    int64_t want = 0;
    for (int i = 1; i <= N_NODES; i++) {
        shm_off off = shm_alloc(a, sizeof(struct node), alignof(struct node));
        CHECK(off != 0 && off % alignof(struct node) == 0);
        struct node *n = off2ptr(a, off);
        n->value = i * 10;
        n->next_ptr = h->head_ptr; /* push front */
        n->next_off = h->head_off;
        h->head_ptr = n;
        h->head_off = ptr2off(a, n); /* == off */
        want += i * 10;
    }
    printf("built %d nodes in A; header->used = %u bytes; first node at "
           "offset %u\n", N_NODES, h->used, (unsigned)sizeof(struct header));

    /* ---- reading through mapping B ---- */
    struct header *hb = b;
    char *b_lo = b, *b_hi = (char *)b + REGION_SIZE;
    char *p = (char *)hb->head_ptr;
    int ptr_inside_b = p >= b_lo && p < b_hi;
    printf("via B: head_ptr = %p -> %s mapping B (it points into A!)\n",
           (void *)p, ptr_inside_b ? "inside" : "OUTSIDE");
    CHECK(!ptr_inside_b);
    /* We do NOT dereference it: here it happens to hit mapping A (same
     * process), so it would even "work" — the most dangerous kind of bug.
     * In another process that address is garbage or unmapped. */

    int64_t got_b = sum_by_offsets(b);
    printf("via B: walking the OFFSETS gives sum %lld (expected %lld)\n",
           (long long)got_b, (long long)want);
    CHECK(got_b == want);

    /* ---- reading in another process ---- */
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        CHECK(munmap(a, REGION_SIZE) == 0); /* make A's addresses invalid */
        void *c = map_obj(name);
        int64_t got_c = sum_by_offsets(c);
        struct header *hc = c;
        char *q = (char *)hc->head_ptr;
        int inside_c = q >= (char *)c && q < (char *)c + REGION_SIZE;
        printf("child: mapped at %p; offsets -> sum %lld\n", c,
               (long long)got_c);
        if (inside_c)
            printf("child: head_ptr %p lands inside the new mapping ONLY "
                   "because the kernel reused A's freed address range - "
                   "luck, not correctness\n", (void *)q);
        else
            printf("child: head_ptr %p is outside its mapping: "
                   "dereferencing it would read garbage or SIGSEGV\n",
                   (void *)q);
        fflush(stdout);
        CHECK(got_c == want);
        CHECK(!inside_c || c == a); /* only "works" by address coincidence */
        _exit(0);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* ---- alignment and cache lines ---- */
    shm_off line = shm_alloc(a, 64, 64);
    CHECK(line % 64 == 0);
    printf("cache-line aligned allocation at offset %u (mmap base is "
           "page-aligned, so offset %% 64 == 0 means address %% 64 == 0 in "
           "every process)\n", line);

    CHECK(munmap(a, REGION_SIZE) == 0);
    CHECK(munmap(b, REGION_SIZE) == 0);
    CHECK(shm_unlink(name) == 0);
    printf("08_offsets_not_pointers: all checks passed\n");
    return 0;
}
