/*
 * 04_reverse_and_middle.c — reverse a singly list (iterative and recursive)
 *                           and find the middle with slow/fast pointers
 *
 * Concept : Reversal is THE pointer-rewiring drill: three pointers
 *           (prev, cur, next) walk the list and flip one arrow per step.
 *           The recursive version flips arrows on the way back up the call
 *           stack. Slow/fast: fast moves 2, slow moves 1, so when fast falls
 *           off the end, slow is in the middle — one pass, O(1) space.
 * Build   : make run P=dsa/06-linked-lists/c/04_reverse_and_middle.c
 * Read    : ../README.md  (sections "Reverse" and "Slow and fast pointers")
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

/* Build a list from an array. Caller owns the result; free with free_list. */
static Node *from_array(const int *a, size_t n)
{
    Node *head = NULL, **link = &head; /* pointer-to-pointer append, see 07 */
    for (size_t i = 0; i < n; i++) {
        Node *node = malloc(sizeof *node);
        if (!node) { perror("malloc"); exit(EXIT_FAILURE); }
        node->value = a[i];
        node->next = NULL;
        *link = node;
        link = &node->next;
    }
    return head;
}

static void free_list(Node *head)
{
    while (head) { Node *next = head->next; free(head); head = next; }
}

static bool equals(const Node *h, const int *a, size_t n)
{
    for (size_t i = 0; i < n; i++, h = h->next)
        if (!h || h->value != a[i]) return false;
    return h == NULL;
}

static void print(const char *label, const Node *h)
{
    printf("%-22s", label);
    for (; h; h = h->next) printf("[%d] -> ", h->value);
    printf("NULL\n");
}

/* O(n) time, O(1) space. Flip one arrow per step.
 *
 *   start:  prev=NULL  cur=[1] -> [2] -> [3] -> NULL
 *
 *   step:   next = cur->next      remember the rest BEFORE breaking the link
 *           cur->next = prev      flip the arrow
 *           prev = cur            advance both
 *           cur = next
 *
 *   after 1: NULL <- [1]    [2] -> [3] -> NULL      prev=[1] cur=[2]
 *   after 2: NULL <- [1] <- [2]    [3] -> NULL      prev=[2] cur=[3]
 *   after 3: NULL <- [1] <- [2] <- [3]              prev=[3] cur=NULL
 *
 *   return prev: the new head is the old tail.                       */
static Node *reverse_iter(Node *head)
{
    Node *prev = NULL, *cur = head;
    while (cur) {
        Node *next = cur->next;
        cur->next = prev;
        prev = cur;
        cur = next;
    }
    return prev;
}

/* O(n) time, O(n) STACK space (one frame per node) — so a 1-million-node
 * list can overflow an 8 MB stack. Fine for learning; prefer iterative.
 *
 *   reverse_rec([1]->[2]->[3]):
 *     new_head = reverse_rec([2]->[3])   -> returns [3], list is now
 *                                           [1] -> [2] <- [3]
 *                                                  |
 *                                                 NULL
 *     head->next->next = head            [2]->next = [1]:  [1] <-> [2]
 *     head->next = NULL                  [1] -> NULL  (break the 2-cycle)
 *     return new_head (= [3], passed up unchanged from the bottom)     */
static Node *reverse_rec(Node *head)
{
    if (!head || !head->next) return head; /* 0 or 1 node: already reversed */
    Node *new_head = reverse_rec(head->next);
    head->next->next = head;
    head->next = NULL;
    return new_head;
}

/* O(n) one pass, O(1) space.
 *   fast moves 2, slow moves 1. When fast can't move 2 more, stop.
 *
 *   odd  (5):  [1] [2] [3] [4] [5]     slow ends on [3]
 *   even (4):  [1] [2] [3] [4]         slow ends on [3] (the SECOND middle)
 *
 * Condition `fast && fast->next` gives the second middle for even length.
 * Use `fast->next && fast->next->next` (starting with fast = head) to get the
 * FIRST middle — needed when splitting a list in half for merge sort.   */
static Node *middle(Node *head)
{
    Node *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    return slow;
}

static Node *first_middle(Node *head)
{
    if (!head) return NULL;
    Node *slow = head, *fast = head;
    while (fast->next && fast->next->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    return slow;
}

/* Application: palindrome check in O(n) time, O(1) extra space.
 * Reverse the second half, compare, then reverse it back (be polite: the
 * caller's list must look unchanged afterwards). */
static bool is_palindrome(Node *head)
{
    if (!head || !head->next) return true;
    Node *mid = first_middle(head);
    Node *second = reverse_iter(mid->next);
    bool ok = true;
    for (Node *a = head, *b = second; b; a = a->next, b = b->next)
        if (a->value != b->value) { ok = false; break; }
    mid->next = reverse_iter(second); /* restore */
    return ok;
}

int main(void)
{
    const int a[] = {1, 2, 3, 4, 5};
    Node *h = from_array(a, 5);
    print("original:", h);

    h = reverse_iter(h);
    print("reverse_iter:", h);
    CHECK(equals(h, (int[]){5, 4, 3, 2, 1}, 5));

    h = reverse_rec(h);
    print("reverse_rec:", h);
    CHECK(equals(h, a, 5));

    CHECK(reverse_iter(NULL) == NULL);
    CHECK(reverse_rec(NULL) == NULL);

    printf("middle of 5 nodes:    [%d]\n", middle(h)->value);
    CHECK(middle(h)->value == 3);
    CHECK(first_middle(h)->value == 3);
    free_list(h);

    h = from_array((int[]){1, 2, 3, 4}, 4);
    printf("middle of 4 nodes:    second=[%d] first=[%d]\n",
           middle(h)->value, first_middle(h)->value);
    CHECK(middle(h)->value == 3 && first_middle(h)->value == 2);
    free_list(h);

    h = from_array((int[]){7}, 1);
    CHECK(middle(h)->value == 7 && first_middle(h)->value == 7);
    free_list(h);

    h = from_array((int[]){1, 2, 3, 2, 1}, 5);
    CHECK(is_palindrome(h));
    CHECK(equals(h, (int[]){1, 2, 3, 2, 1}, 5)); /* list restored */
    free_list(h);
    h = from_array((int[]){1, 2, 2, 1}, 4);
    CHECK(is_palindrome(h));
    free_list(h);
    h = from_array((int[]){1, 2, 3}, 3);
    CHECK(!is_palindrome(h));
    CHECK(equals(h, (int[]){1, 2, 3}, 3));
    free_list(h);
    printf("palindrome checks:    ok\n");

    /* Bigger list: iterative handles it; recursive would use 100k frames. */
    int *big = malloc(100000 * sizeof *big);
    CHECK(big != NULL);
    for (int i = 0; i < 100000; i++) big[i] = i;
    h = reverse_iter(from_array(big, 100000));
    CHECK(h->value == 99999 && middle(h)->value == 49999);
    free_list(h);
    free(big);

    printf("04_reverse_and_middle: all checks passed\n");
    return 0;
}
