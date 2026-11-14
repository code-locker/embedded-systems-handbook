/*
 * 02_doubly_linked_list.c — doubly linked list: O(1) insert/delete at both ends
 *                           and O(1) delete of a node you already hold
 *
 * Concept : Each node has prev AND next. That one extra pointer buys O(1)
 *           pop_back and O(1) "unlink this node" (no need to find its
 *           predecessor), at the cost of 8 more bytes per node and four
 *           pointer writes per insert instead of two.
 * Build   : make run P=dsa/06-linked-lists/c/02_doubly_linked_list.c
 * Read    : ../README.md  (section "Doubly linked list")
 */
#include <stdbool.h>
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

typedef struct DNode {
    int value;
    struct DNode *prev;
    struct DNode *next;
} DNode;

typedef struct {
    DNode *head;
    DNode *tail;
    size_t size;
} DList;

/* Ownership: the list owns every node; unlink_node frees it. */
static DNode *new_node(int value)
{
    DNode *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(EXIT_FAILURE); }
    n->value = value;
    n->prev = n->next = NULL;
    return n;
}

/* O(1).
 *            NULL <- [N] -> [A] <-> [B]
 *   n->next = head;  head->prev = n;  head = n                        */
static DNode *push_front(DList *l, int value)
{
    DNode *n = new_node(value);
    n->next = l->head;
    if (l->head) l->head->prev = n;
    else         l->tail = n;
    l->head = n;
    l->size++;
    return n;
}

/* O(1). Mirror image of push_front. */
static DNode *push_back(DList *l, int value)
{
    DNode *n = new_node(value);
    n->prev = l->tail;
    if (l->tail) l->tail->next = n;
    else         l->head = n;
    l->tail = n;
    l->size++;
    return n;
}

/* O(1): insert n right after node `at` — FOUR pointer writes.
 *
 *   before:   [at] <=====> [B]
 *
 *   1. n->prev = at              [at] <-- [N]
 *   2. n->next = at->next                 [N] --> [B]
 *   3. at->next->prev = n  (or tail = n if at was the tail)
 *   4. at->next = n
 *
 *   after:    [at] <=> [N] <=> [B]
 *
 * Do 1-2 first (they only touch the new node), then 3-4. If you do 4 before
 * 2, at->next already points at N and you lose B.                     */
static DNode *insert_after(DList *l, DNode *at, int value)
{
    DNode *n = new_node(value);
    n->prev = at;
    n->next = at->next;
    if (at->next) at->next->prev = n;
    else          l->tail = n;
    at->next = n;
    l->size++;
    return n;
}

/* O(1): the superpower of a doubly list. Given the node itself, its
 * neighbours are right there — no O(n) search for the predecessor.
 *
 *   [A] <=> [X] <=> [B]          X->prev->next = X->next
 *   [A] <=========> [B]          X->next->prev = X->prev ; free(X)
 *
 * The head/tail branches are the "special cases" that the kernel's
 * circular list with a sentinel removes (see 08_kernel_list_head.c). */
static int unlink_node(DList *l, DNode *x)
{
    int v = x->value;
    if (x->prev) x->prev->next = x->next;
    else         l->head = x->next;
    if (x->next) x->next->prev = x->prev;
    else         l->tail = x->prev;
    free(x);
    l->size--;
    return v;
}

static bool pop_back(DList *l, int *out)  /* O(1), unlike the singly list */
{
    if (!l->tail) return false;
    *out = unlink_node(l, l->tail);
    return true;
}

static bool pop_front(DList *l, int *out) /* O(1) */
{
    if (!l->head) return false;
    *out = unlink_node(l, l->head);
    return true;
}

/* O(min(pos, size-pos)): walk from whichever end is closer. */
static DNode *node_at(const DList *l, size_t pos)
{
    if (pos >= l->size) return NULL;
    DNode *n;
    if (pos < l->size / 2) {
        n = l->head;
        for (size_t i = 0; i < pos; i++) n = n->next;
    } else {
        n = l->tail;
        for (size_t i = l->size - 1; i > pos; i--) n = n->prev;
    }
    return n;
}

static DNode *find(const DList *l, int value) /* O(n) */
{
    for (DNode *n = l->head; n; n = n->next)
        if (n->value == value) return n;
    return NULL;
}

/* Self-check that walks BOTH directions: catches a forgotten prev update,
 * the most common doubly-list bug. */
static bool equals(const DList *l, const int *expect, size_t cnt)
{
    if (l->size != cnt) return false;
    size_t i = 0;
    const DNode *prev = NULL;
    for (const DNode *n = l->head; n; prev = n, n = n->next, i++) {
        if (i >= cnt || n->value != expect[i]) return false;
        if (n->prev != prev) return false; /* back link must match */
    }
    if (i != cnt || l->tail != prev) return false;
    i = cnt;
    for (const DNode *n = l->tail; n; n = n->prev)
        if (n->value != expect[--i]) return false;
    return i == 0;
}

static void print(const char *label, const DList *l)
{
    printf("%-18s NULL <- ", label);
    for (const DNode *n = l->head; n; n = n->next)
        printf("[%d]%s", n->value, n->next ? " <=> " : "");
    printf(" -> NULL\n");
}

static void list_free(DList *l) /* O(n) */
{
    int v;
    while (pop_front(l, &v)) {}
}

int main(void)
{
    DList l = {NULL, NULL, 0};

    push_back(&l, 2);
    push_back(&l, 3);
    push_front(&l, 1);
    print("push:", &l);
    CHECK(equals(&l, (int[]){1, 2, 3}, 3));

    DNode *two = find(&l, 2);
    CHECK(two != NULL);
    insert_after(&l, two, 25);
    insert_after(&l, l.tail, 4); /* after the tail -> becomes new tail */
    print("insert_after:", &l);
    CHECK(equals(&l, (int[]){1, 2, 25, 3, 4}, 5));

    CHECK(node_at(&l, 0)->value == 1);
    CHECK(node_at(&l, 4)->value == 4); /* walked from the tail */
    CHECK(node_at(&l, 3)->value == 3);
    CHECK(node_at(&l, 5) == NULL);

    /* Unlink a middle node we hold a pointer to: O(1). */
    CHECK(unlink_node(&l, two) == 2);
    print("unlink(2):", &l);
    CHECK(equals(&l, (int[]){1, 25, 3, 4}, 4));

    int v;
    CHECK(pop_back(&l, &v) && v == 4);
    CHECK(pop_front(&l, &v) && v == 1);
    print("pop both ends:", &l);
    CHECK(equals(&l, (int[]){25, 3}, 2));

    list_free(&l);
    CHECK(l.head == NULL && l.tail == NULL && l.size == 0);
    CHECK(!pop_back(&l, &v));

    printf("02_doubly_linked_list: all checks passed\n");
    return 0;
}
