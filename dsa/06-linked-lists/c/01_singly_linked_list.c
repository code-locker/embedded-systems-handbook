/*
 * 01_singly_linked_list.c — singly linked list: insert/delete at head, tail,
 *                           position; search; length; free
 *
 * Concept : A singly linked list is a chain of heap nodes, each holding a
 *           value and ONE pointer to the next node. The list itself is just a
 *           pointer to the first node (head). We also keep a tail pointer so
 *           push_back is O(1) instead of O(n).
 * Build   : make run P=dsa/06-linked-lists/c/01_singly_linked_list.c
 * Read    : ../README.md  (section "Singly linked list")
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* CHECK always evaluates its argument (unlike assert, which vanishes under
 * -DNDEBUG — and would take pop_front(...) calls inside it along with it). */
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

typedef struct Node {
    int value;
    struct Node *next; /* NULL marks the end of the chain */
} Node;

/* The "handle" for the whole list. Owns every node reachable from head. */
typedef struct {
    Node *head;
    Node *tail;  /* cached so push_back does not have to walk: O(1) not O(n) */
    size_t size; /* cached so length() is O(1) */
} List;

/* Ownership: new_node mallocs; whoever unlinks the node must free it. */
static Node *new_node(int value)
{
    Node *n = malloc(sizeof *n);
    if (!n) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    n->value = value;
    n->next = NULL;
    return n;
}

static void list_init(List *l) { l->head = l->tail = NULL; l->size = 0; }

/* O(1): only the head pointer changes.
 *
 *   before:  head -> [A] -> [B] -> NULL
 *   step 1:  n->next = head          [N] -> [A]
 *   step 2:  head = n        head -> [N] -> [A] -> [B] -> NULL
 *
 * Order matters: if you set head = n first, you lose the old chain. */
static void push_front(List *l, int value)
{
    Node *n = new_node(value);
    n->next = l->head;
    l->head = n;
    if (!l->tail) l->tail = n; /* list was empty: new node is also the tail */
    l->size++;
}

/* O(1) thanks to the tail pointer (O(n) without it).
 *
 *   before:  head -> [A] -> [B] -> NULL        tail -> [B]
 *   step 1:  tail->next = n     [B] -> [N] -> NULL
 *   step 2:  tail = n                          tail -> [N]          */
static void push_back(List *l, int value)
{
    Node *n = new_node(value);
    if (l->tail) l->tail->next = n;
    else         l->head = n; /* empty list: head must point at it too */
    l->tail = n;
    l->size++;
}

/* O(pos): walk to the node BEFORE the insertion point.
 * pos == 0 inserts at the front; pos == size inserts at the back.
 *
 *   insert N at pos 2:
 *   head -> [A] -> [B] -> [C]        prev = [B] (index 1)
 *                   |
 *            N->next = prev->next  ([N] -> [C])
 *            prev->next = N        ([B] -> [N])
 *   head -> [A] -> [B] -> [N] -> [C]                                  */
static bool insert_at(List *l, size_t pos, int value)
{
    if (pos > l->size) return false;
    if (pos == 0) { push_front(l, value); return true; }
    if (pos == l->size) { push_back(l, value); return true; }
    Node *prev = l->head;
    for (size_t i = 0; i + 1 < pos; i++) prev = prev->next;
    Node *n = new_node(value);
    n->next = prev->next;
    prev->next = n;
    l->size++;
    return true;
}

/* O(1). Returns false on empty list. Frees the removed node.
 *
 *   head -> [A] -> [B]      old = [A]; head = A->next; free(A)
 *   head -> [B]                                                      */
static bool pop_front(List *l, int *out)
{
    if (!l->head) return false;
    Node *old = l->head;
    if (out) *out = old->value;
    l->head = old->next;
    if (!l->head) l->tail = NULL; /* removed the last node */
    free(old);
    l->size--;
    return true;
}

/* O(n): even with a tail pointer! We must find the node BEFORE the tail to
 * set its next to NULL, and a singly list cannot walk backwards. This is the
 * #1 reason doubly linked lists exist. */
static bool pop_back(List *l, int *out)
{
    if (!l->head) return false;
    if (l->head == l->tail) return pop_front(l, out);
    Node *prev = l->head;
    while (prev->next != l->tail) prev = prev->next; /* O(n) walk */
    if (out) *out = l->tail->value;
    free(l->tail);
    prev->next = NULL;
    l->tail = prev;
    l->size--;
    return true;
}

/* O(pos). Removes node at index pos.
 *
 *   remove index 1:
 *   head -> [A] -> [B] -> [C]       prev = [A], victim = [B]
 *   prev->next = victim->next       [A] ------> [C]
 *   free(victim)                                                      */
static bool remove_at(List *l, size_t pos, int *out)
{
    if (pos >= l->size) return false;
    if (pos == 0) return pop_front(l, out);
    Node *prev = l->head;
    for (size_t i = 0; i + 1 < pos; i++) prev = prev->next;
    Node *victim = prev->next;
    if (out) *out = victim->value;
    prev->next = victim->next;
    if (victim == l->tail) l->tail = prev; /* keep tail valid! classic bug */
    free(victim);
    l->size--;
    return true;
}

/* O(n): linear scan, no random access. Returns index or -1. */
static long search(const List *l, int value)
{
    long i = 0;
    for (const Node *n = l->head; n; n = n->next, i++)
        if (n->value == value) return i;
    return -1;
}

/* O(n) walk — just to cross-check the cached size. */
static size_t count_nodes(const List *l)
{
    size_t c = 0;
    for (const Node *n = l->head; n; n = n->next) c++;
    return c;
}

/* O(n). Must save next BEFORE freeing: after free(n), n->next is garbage
 * (use-after-free, ASan would flag it). */
static void list_free(List *l)
{
    Node *n = l->head;
    while (n) {
        Node *next = n->next;
        free(n);
        n = next;
    }
    list_init(l);
}

static void print(const char *label, const List *l)
{
    printf("%-22s head -> ", label);
    for (const Node *n = l->head; n; n = n->next) printf("[%d] -> ", n->value);
    printf("NULL   (size %zu)\n", l->size);
}

/* Compare the list with an expected array: our self-check. */
static bool equals(const List *l, const int *expect, size_t n)
{
    if (l->size != n || count_nodes(l) != n) return false;
    const Node *node = l->head;
    for (size_t i = 0; i < n; i++, node = node->next)
        if (node->value != expect[i]) return false;
    return n == 0 ? l->tail == NULL : l->tail->value == expect[n - 1];
}

int main(void)
{
    List l;
    list_init(&l);

    push_back(&l, 20);
    push_back(&l, 30);
    push_front(&l, 10);
    print("push_back/front:", &l);
    CHECK(equals(&l, (int[]){10, 20, 30}, 3));

    insert_at(&l, 1, 15);
    insert_at(&l, 4, 40);   /* at the end -> push_back */
    insert_at(&l, 0, 5);    /* at the front -> push_front */
    print("insert_at:", &l);
    CHECK(equals(&l, (int[]){5, 10, 15, 20, 30, 40}, 6));
    CHECK(!insert_at(&l, 99, 0)); /* out of range is rejected */

    CHECK(search(&l, 20) == 3);
    CHECK(search(&l, 99) == -1);
    printf("search(20) = index %ld, search(99) = %ld\n", search(&l, 20), search(&l, 99));

    int v;
    CHECK(pop_front(&l, &v) && v == 5);
    CHECK(pop_back(&l, &v) && v == 40);
    CHECK(remove_at(&l, 1, &v) && v == 15);
    CHECK(remove_at(&l, 2, &v) && v == 30); /* removes the tail via remove_at */
    print("after removals:", &l);
    CHECK(equals(&l, (int[]){10, 20}, 2));

    push_back(&l, 99); /* would crash / corrupt if tail were left dangling */
    CHECK(equals(&l, (int[]){10, 20, 99}, 3));

    while (pop_back(&l, &v)) {}
    CHECK(equals(&l, NULL, 0));
    CHECK(!pop_front(&l, &v));
    print("emptied:", &l);

    for (int i = 0; i < 1000; i++) push_back(&l, i);
    CHECK(l.size == 1000 && search(&l, 999) == 999);
    list_free(&l); /* every node freed; ASan leak check proves it */
    CHECK(l.head == NULL && l.size == 0);

    printf("01_singly_linked_list: all checks passed\n");
    return 0;
}
