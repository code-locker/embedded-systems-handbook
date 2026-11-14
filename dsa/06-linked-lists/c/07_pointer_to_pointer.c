/*
 * 07_pointer_to_pointer.c — the "indirect pointer" trick: delete, insert
 *                           sorted, and remove-all-matching with NO special
 *                           case for the head
 *
 * Concept : The naive delete needs `if (node == head) head = ... else
 *           prev->next = ...`. Notice both branches do the same thing: they
 *           overwrite "the pointer that points at the node". Keep a pointer
 *           TO that pointer (Node **link) — it starts as &head and becomes
 *           &prev->next — and there is only one case: *link = victim->next.
 *           Linus Torvalds calls this "good taste" (TED 2016).
 * Build   : make run P=dsa/06-linked-lists/c/07_pointer_to_pointer.c
 * Read    : ../README.md  (section "The pointer-to-pointer trick")
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

typedef struct Node {
    int value;
    struct Node *next;
} Node;

static void free_list(Node *h)
{
    while (h) { Node *n = h->next; free(h); h = n; }
}

static bool equals(const Node *h, const int *a, size_t n)
{
    for (size_t i = 0; i < n; i++, h = h->next)
        if (!h || h->value != a[i]) return false;
    return h == NULL;
}

static void print(const char *label, const Node *h)
{
    printf("%-26s", label);
    for (; h; h = h->next) printf("[%d] -> ", h->value);
    printf("NULL\n");
}

/* ---- The "before": two cases ------------------------------------------ */
static bool remove_naive(Node **headp, int value)
{
    Node *prev = NULL, *cur = *headp;
    while (cur && cur->value != value) { prev = cur; cur = cur->next; }
    if (!cur) return false;
    if (!prev) *headp = cur->next;   /* special case: removing the head */
    else       prev->next = cur->next;
    free(cur);
    return true;
}

/* ---- The "after": one case -------------------------------------------- *
 * O(n). `link` is the ADDRESS of whichever pointer currently points at the
 * node we are looking at.
 *
 *   head --> [3] --> [7] --> [9] --> NULL
 *   ^          ^ next
 *   |          |
 *   link       link (after one step: link = &(*link)->next)
 *   = &head    = &node3->next
 *
 *   To remove [7] when link == &node3->next:
 *        *link = (*link)->next      node3->next now points at [9]
 *
 *   head --> [3] ------------> [9] --> NULL        free([7])
 *
 * When the victim is the head, link == &head and the same line updates head.
 * No prev, no if.                                                       */
static bool remove_value(Node **link, int value)
{
    while (*link && (*link)->value != value)
        link = &(*link)->next;
    if (!*link) return false;
    Node *victim = *link;
    *link = victim->next;
    free(victim);
    return true;
}

/* O(n): remove EVERY node matching a predicate in one pass. Note we do not
 * advance link after a removal: *link already names the next candidate. */
static size_t remove_if(Node **link, bool (*pred)(int))
{
    size_t removed = 0;
    while (*link) {
        if (pred((*link)->value)) {
            Node *victim = *link;
            *link = victim->next;
            free(victim);
            removed++;
        } else {
            link = &(*link)->next;
        }
    }
    return removed;
}

/* O(n): sorted insert. Walk link until *link is the first node >= value,
 * then splice in front of it. Works for empty list, new head, middle, tail.
 *
 *   link -> [ptr to 9]      n->next = *link   ([5] -> [9])
 *                           *link = n         ([3] -> [5])               */
static void insert_sorted(Node **link, int value)
{
    while (*link && (*link)->value < value)
        link = &(*link)->next;
    Node *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(EXIT_FAILURE); }
    n->value = value;
    n->next = *link;
    *link = n;
}

static bool is_even(int v) { return v % 2 == 0; }

int main(void)
{
    Node *head = NULL;
    const int in[] = {7, 3, 9, 1, 8, 4, 6, 2, 5};
    for (size_t i = 0; i < sizeof in / sizeof in[0]; i++)
        insert_sorted(&head, in[i]); /* new head, middle and tail all covered */
    print("insert_sorted:", head);
    CHECK(equals(head, (int[]){1, 2, 3, 4, 5, 6, 7, 8, 9}, 9));

    CHECK(remove_value(&head, 1));     /* the head  */
    CHECK(remove_value(&head, 5));     /* the middle */
    CHECK(remove_value(&head, 9));     /* the tail  */
    CHECK(!remove_value(&head, 42));   /* absent    */
    print("remove 1, 5, 9:", head);
    CHECK(equals(head, (int[]){2, 3, 4, 6, 7, 8}, 6));

    CHECK(remove_naive(&head, 2));     /* same results, more code */
    CHECK(remove_naive(&head, 8));
    print("remove_naive 2, 8:", head);
    CHECK(equals(head, (int[]){3, 4, 6, 7}, 4));

    CHECK(remove_if(&head, is_even) == 2);
    print("remove_if(even):", head);
    CHECK(equals(head, (int[]){3, 7}, 2));

    /* Removing consecutive matches at the head is the classic trap for the
     * prev/cur version. The link version doesn't care. */
    Node *h2 = NULL;
    for (int v = 1; v <= 6; v++) insert_sorted(&h2, v * 2); /* 2,4,...,12 */
    insert_sorted(&h2, 13);
    CHECK(remove_if(&h2, is_even) == 6);
    CHECK(equals(h2, (int[]){13}, 1));

    free_list(head);
    free_list(h2);
    printf("07_pointer_to_pointer: all checks passed\n");
    return 0;
}
