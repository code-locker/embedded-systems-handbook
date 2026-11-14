/*
 * 08_kernel_list_head.c — a mini version of the Linux kernel's intrusive
 *                         `struct list_head` and `container_of`
 *
 * Concept : Instead of a list node that CONTAINS your data, your data
 *           contains the list node ("intrusive" list). The list code only
 *           ever sees `struct list_head { next, prev }`; container_of() does
 *           pointer arithmetic to get back from the embedded member to the
 *           enclosing struct. The list is circular with a sentinel head, so
 *           add and delete have ZERO special cases, and one object can sit
 *           on several lists at once (one list_head member per list).
 *           Same API names as include/linux/list.h, simplified.
 * Build   : make run P=dsa/06-linked-lists/c/08_kernel_list_head.c
 * Read    : ../README.md  (section "Linux kernel list_head and container_of")
 */
#include <stdbool.h>
#include <stddef.h> /* offsetof */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/* ---------------------------------------------------------------------- *
 *  The mini list.h
 * ---------------------------------------------------------------------- */
struct list_head {
    struct list_head *next, *prev;
};

/* container_of(ptr, type, member):
 *   "ptr points at `member` inside some `type`; give me the `type *`."
 *
 *   struct task (at address 0x1000)
 *   +------------------+ 0x1000   <- container_of(p, struct task, node)
 *   | int pid          |              = (char *)p - offsetof(task, node)
 *   | char name[16]    |              = 0x1018 - 0x18 = 0x1000
 *   +------------------+ 0x1018   <- p (a struct list_head *)
 *   | list_head node   |
 *   +------------------+
 *
 * The kernel version adds a type check with typeof (a GNU extension); we
 * stay in ISO C11, so we cast through char * only.                      */
#define container_of(ptr, type, member) \
    ((type *)(void *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) container_of(ptr, type, member)

/* An empty list is a head that points at itself both ways:
 *
 *        +------+
 *   +--> | head | --+       head.next == head.prev == &head
 *   +--- |      | <-+
 *        +------+                                                    */
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static void INIT_LIST_HEAD(struct list_head *h) { h->next = h->prev = h; }

static bool list_empty(const struct list_head *h) { return h->next == h; }

/* O(1). Insert n between two KNOWN consecutive entries. Because the list is
 * circular with a sentinel, prev and next always exist: no NULL checks.
 *
 *   [prev] <=> [next]    ==>    [prev] <=> [n] <=> [next]           */
static void __list_add(struct list_head *n, struct list_head *prev,
                       struct list_head *next)
{
    next->prev = n;
    n->next = next;
    n->prev = prev;
    prev->next = n;
}

/* Add right after head: stack behaviour (LIFO when iterated forwards). */
static void list_add(struct list_head *n, struct list_head *head)
{
    __list_add(n, head, head->next);
}

/* Add right before head, i.e. at the tail: queue behaviour (FIFO). */
static void list_add_tail(struct list_head *n, struct list_head *head)
{
    __list_add(n, head->prev, head);
}

/* O(1). Unlink entry. No head pointer needed, no special case.
 *
 *   [A] <=> [entry] <=> [B]    ==>    [A] <=> [B]
 *
 * The kernel then poisons entry->next/prev (LIST_POISON1/2) so a use-after-
 * delete faults loudly; we point them at NULL for the same reason.      */
static void list_del(struct list_head *entry)
{
    entry->next->prev = entry->prev;
    entry->prev->next = entry->next;
    entry->next = entry->prev = NULL;
}

/* O(1): unlink and add to another list — e.g. move a task from the run
 * queue to a wait queue. */
static void list_move_tail(struct list_head *entry, struct list_head *head)
{
    list_del(entry);
    list_add_tail(entry, head);
}

/* Iteration. `pos` is the struct pointer; `member` names the list_head
 * inside it. The loop stops when we come back round to the sentinel.   */
#define list_for_each(pos, head) \
    for (pos = (head)->next; pos != (head); pos = pos->next)

/* Walk entries of a given struct type. The kernel uses typeof to avoid the
 * `type` argument; ISO C cannot, so we pass it explicitly.             */
#define list_for_each_entry(pos, head, type, member)          \
    for (pos = list_entry((head)->next, type, member);        \
         &pos->member != (head);                              \
         pos = list_entry(pos->member.next, type, member))

/* _safe: keep a lookahead `n` so we may list_del + free(pos) in the body.
 * Using the plain version while freeing reads pos->member.next AFTER free:
 * a use-after-free that ASan would report.                             */
#define list_for_each_entry_safe(pos, n, head, type, member)     \
    for (pos = list_entry((head)->next, type, member),           \
         n = list_entry(pos->member.next, type, member);         \
         &pos->member != (head);                                 \
         pos = n, n = list_entry(n->member.next, type, member))

/* ---------------------------------------------------------------------- *
 *  A user of the list: a "task" that is on TWO lists at once
 * ---------------------------------------------------------------------- */
struct task {
    int pid;
    char name[16];
    struct list_head all;   /* membership in the list of ALL tasks       */
    struct list_head queue; /* membership in the run queue OR wait queue */
};

static struct task *task_new(int pid, const char *name)
{
    struct task *t = malloc(sizeof *t); /* owner: freed in main's cleanup */
    if (!t) { perror("malloc"); exit(EXIT_FAILURE); }
    t->pid = pid;
    snprintf(t->name, sizeof t->name, "%s", name);
    INIT_LIST_HEAD(&t->all);
    INIT_LIST_HEAD(&t->queue);
    return t;
}

static size_t count(const struct list_head *head)
{
    size_t n = 0;
    const struct list_head *p;
    list_for_each(p, head) n++;
    return n;
}

static void print_queue(const char *label, struct list_head *head)
{
    struct task *t;
    printf("%-12s", label);
    list_for_each_entry(t, head, struct task, queue) printf("%s(%d) ", t->name, t->pid);
    printf("\n");
}

/* Collect pids of a queue into out[]; returns how many. For CHECKs. */
static size_t queue_pids(struct list_head *head, int *out, size_t max)
{
    size_t n = 0;
    struct task *t;
    list_for_each_entry(t, head, struct task, queue) if (n < max) out[n++] = t->pid;
    return n;
}

int main(void)
{
    LIST_HEAD(all_tasks);
    LIST_HEAD(run_queue);
    struct list_head wait_queue;
    INIT_LIST_HEAD(&wait_queue);
    CHECK(list_empty(&all_tasks) && list_empty(&run_queue));

    /* container_of sanity: the arithmetic really returns the outer struct. */
    struct task probe;
    CHECK(container_of(&probe.queue, struct task, queue) == &probe);
    CHECK(container_of(&probe.all, struct task, all) == &probe);
    printf("offsetof(task, all)=%zu  offsetof(task, queue)=%zu\n",
           offsetof(struct task, all), offsetof(struct task, queue));

    const char *names[] = {"init", "sshd", "gst-launch", "udevd"};
    for (int i = 0; i < 4; i++) {
        struct task *t = task_new(i + 1, names[i]);
        list_add_tail(&t->all, &all_tasks);   /* FIFO order on both lists */
        list_add_tail(&t->queue, &run_queue);
    }
    print_queue("run_queue:", &run_queue);
    CHECK(count(&all_tasks) == 4 && count(&run_queue) == 4);

    /* gst-launch (pid 3) blocks on I/O: move it to the wait queue. It stays
     * on all_tasks — that is the point of having two list_head members. */
    struct task *t, *tmp;
    list_for_each_entry(t, &run_queue, struct task, queue) {
        if (t->pid == 3) { list_move_tail(&t->queue, &wait_queue); break; }
        /* break is required: after the move, t->queue.next points into the
         * wait queue and the loop would never see &run_queue again. */
    }
    print_queue("run_queue:", &run_queue);
    print_queue("wait_queue:", &wait_queue);
    int pids[8];
    CHECK(queue_pids(&run_queue, pids, 8) == 3 && pids[0] == 1 && pids[1] == 2 && pids[2] == 4);
    CHECK(queue_pids(&wait_queue, pids, 8) == 1 && pids[0] == 3);
    CHECK(count(&all_tasks) == 4);

    /* list_add (at head) gives LIFO: push 10 then 11 -> iterate 11, 10. */
    struct task *a = task_new(10, "kworker"), *b = task_new(11, "ksoftirqd");
    list_add(&a->queue, &wait_queue);
    list_add(&b->queue, &wait_queue);
    list_add_tail(&a->all, &all_tasks);
    list_add_tail(&b->all, &all_tasks);
    print_queue("wait_queue:", &wait_queue);
    CHECK(queue_pids(&wait_queue, pids, 8) == 3 && pids[0] == 11 && pids[1] == 10 && pids[2] == 3);

    /* Round-robin on the run queue: take the first entry, rotate to tail. */
    struct task *first = list_entry(run_queue.next, struct task, queue);
    CHECK(first->pid == 1);
    list_move_tail(&first->queue, &run_queue);
    CHECK(queue_pids(&run_queue, pids, 8) == 3 && pids[0] == 2 && pids[2] == 1);
    print_queue("rotated:", &run_queue);

    /* Teardown: walk all_tasks with the _safe iterator, unlink from both
     * lists, free. Ownership: all_tasks is the owning list. */
    size_t freed = 0;
    list_for_each_entry_safe(t, tmp, &all_tasks, struct task, all) {
        list_del(&t->queue); /* it is on exactly one queue: O(1) unlink */
        list_del(&t->all);
        free(t);
        freed++;
    }
    CHECK(freed == 6);
    CHECK(list_empty(&all_tasks) && list_empty(&run_queue) && list_empty(&wait_queue));

    printf("08_kernel_list_head: all checks passed\n");
    return 0;
}
