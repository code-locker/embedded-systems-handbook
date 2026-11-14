/*
 * 05_kernel_hlist.c — a mini Linux-kernel hlist + container_of hash table
 *
 * Concept : The kernel's hash tables (PIDs, inodes, dentries, network
 *           sockets, net_device by name) use `struct hlist_head` buckets:
 *           ONE pointer per bucket (half the size of list_head), and the node
 *           is EMBEDDED inside the object ("intrusive"). container_of() turns
 *           a node pointer back into the object. `pprev` (pointer to the
 *           previous node's `next`) makes O(1) delete possible without
 *           knowing the bucket.
 * Build   : make run P=dsa/08-hashing/c/05_kernel_hlist.c
 * Read    : ../README.md  (section "Where it is used" -> "Linux kernel hlist")
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                    __LINE__);                                                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/* ---- the mini kernel API (same names/semantics as include/linux/list.h) - */

struct hlist_node {
    struct hlist_node *next;
    struct hlist_node **pprev; /* address of the pointer that points to me */
};
struct hlist_head {
    struct hlist_node *first;  /* ONE pointer: big tables stay small */
};

/*
 * container_of(ptr, type, member): ptr points at `member` inside a `type`;
 * subtract the member's offset to get the start of the object.
 *
 *   struct task (object)            address
 *   +---------------------+  <----  obj  = (char*)ptr - offsetof(task, node)
 *   | int pid             |
 *   | char comm[16]       |
 *   | struct hlist_node   |  <----  ptr
 *   +---------------------+
 *
 * (The kernel version also type-checks ptr with typeof; plain C11 cannot.)
 */
#define container_of(ptr, type, member)                                        \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define hlist_entry(ptr, type, member) container_of(ptr, type, member)

static void INIT_HLIST_HEAD(struct hlist_head *h) { h->first = NULL; }

/*
 * hlist_add_head: O(1).
 *
 *   before:  head.first ──► A          A.pprev = &head.first
 *   after :  head.first ──► N ──► A
 *            N.pprev = &head.first
 *            A.pprev = &N.next
 */
static void hlist_add_head(struct hlist_node *n, struct hlist_head *h)
{
    struct hlist_node *first = h->first;
    n->next = first;
    if (first) first->pprev = &n->next;
    h->first = n;
    n->pprev = &h->first;
}

/*
 * hlist_del: O(1), and we do NOT need the head or the previous node.
 * pprev points at whatever pointer points to us (head.first OR prev->next)
 * so the two cases are one line: *pprev = next.
 *
 *   head.first ──► A ──► N ──► B          delete N
 *                   A.next is *N.pprev
 *   *N.pprev = B:   head.first ──► A ──► B
 *   B.pprev  = N.pprev (= &A.next)
 */
static void hlist_del(struct hlist_node *n)
{
    struct hlist_node *next = n->next;
    *n->pprev = next;
    if (next) next->pprev = n->pprev;
    n->next = NULL;  /* the kernel poisons these (LIST_POISON1/2) so a */
    n->pprev = NULL; /* use-after-delete crashes loudly                */
}

/* hlist_for_each_entry(pos, head, member): pos is the OBJECT pointer.
 * Uses C11-compatible form: needs the object type passed explicitly
 * (the kernel gets it via typeof(*pos)). */
#define hlist_for_each_entry(pos, type, head, member)                          \
    for (pos = (head)->first ? hlist_entry((head)->first, type, member) : NULL; \
         pos;                                                                  \
         pos = pos->member.next ? hlist_entry(pos->member.next, type, member)  \
                                : NULL)

/* Kernel's hash_32(): multiplicative (Fibonacci) hashing, take TOP bits. */
#define GOLDEN_RATIO_32 0x61C88647u
static uint32_t hash_32(uint32_t val, unsigned bits)
{
    return (val * GOLDEN_RATIO_32) >> (32 - bits);
}

/* ---- a user of the API: a PID -> task lookup table ----------------------
 * Like the kernel's pid_hash: tasks live wherever they were allocated; the
 * table only links them. One object can sit in SEVERAL tables at once by
 * embedding several nodes (here: by pid and by name). */

#define PID_BITS 6 /* 64 buckets */
struct task {
    int pid;
    char comm[16];
    struct hlist_node pid_node;  /* link in pid_table   */
    struct hlist_node name_node; /* link in name_table  */
};

static struct hlist_head pid_table[1u << PID_BITS];
static struct hlist_head name_table[1u << PID_BITS];

static uint32_t name_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; }
    return h >> (32 - PID_BITS); /* FNV top bits */
}

static struct task *find_pid(int pid) /* average O(1) */
{
    struct task *t;
    hlist_for_each_entry(t, struct task, &pid_table[hash_32((uint32_t)pid, PID_BITS)], pid_node)
        if (t->pid == pid) return t;
    return NULL;
}

static struct task *find_name(const char *name)
{
    struct task *t;
    hlist_for_each_entry(t, struct task, &name_table[name_hash(name)], name_node)
        if (strcmp(t->comm, name) == 0) return t;
    return NULL;
}

int main(void)
{
    for (size_t i = 0; i < (1u << PID_BITS); i++) {
        INIT_HLIST_HEAD(&pid_table[i]);
        INIT_HLIST_HEAD(&name_table[i]);
    }
    CHECK(sizeof(struct hlist_head) == sizeof(void *)); /* the whole point */

    /* Tasks are allocated as one array (ownership: main). The hash tables
     * never allocate anything — the intrusive design. */
    enum { N = 500 };
    struct task *tasks = calloc(N, sizeof *tasks);
    CHECK(tasks != NULL);
    for (int i = 0; i < N; i++) {
        tasks[i].pid = 1000 + i * 7;
        snprintf(tasks[i].comm, sizeof tasks[i].comm, "kworker/%d", i);
        hlist_add_head(&tasks[i].pid_node, &pid_table[hash_32((uint32_t)tasks[i].pid, PID_BITS)]);
        hlist_add_head(&tasks[i].name_node, &name_table[name_hash(tasks[i].comm)]);
    }

    struct task *t = find_pid(1000 + 42 * 7);
    CHECK(t == &tasks[42]);
    CHECK(find_name("kworker/42") == &tasks[42]); /* same object, other table */
    CHECK(find_pid(3) == NULL);

    /* container_of by hand */
    struct hlist_node *node = &tasks[7].pid_node;
    CHECK(container_of(node, struct task, pid_node) == &tasks[7]);
    CHECK(container_of(&tasks[7].name_node, struct task, name_node) == &tasks[7]);

    /* Delete every even task from BOTH tables in O(1) each — no bucket
     * lookup needed thanks to pprev. */
    for (int i = 0; i < N; i += 2) {
        hlist_del(&tasks[i].pid_node);
        hlist_del(&tasks[i].name_node);
    }
    for (int i = 0; i < N; i++) {
        struct task *p = find_pid(tasks[i].pid);
        CHECK(i % 2 ? p == &tasks[i] : p == NULL);
    }

    /* bucket occupancy */
    size_t maxlen = 0;
    for (size_t b = 0; b < (1u << PID_BITS); b++) {
        size_t len = 0;
        for (struct hlist_node *n = pid_table[b].first; n; n = n->next) len++;
        if (len > maxlen) maxlen = len;
    }
    printf("250 tasks in 64 buckets via hash_32: longest chain %zu (avg %.1f)\n",
           maxlen, 250.0 / 64);
    CHECK(maxlen < 12);

    free(tasks); /* the tables held no memory of their own */
    printf("all hlist checks passed\n");
    return 0;
}
