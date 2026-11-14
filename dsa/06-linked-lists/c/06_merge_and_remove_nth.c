/*
 * 06_merge_and_remove_nth.c — merge two sorted lists (dummy-node trick),
 *                             remove the n-th node from the end (gap pointers),
 *                             and merge sort a linked list
 *
 * Concept : A dummy (sentinel) node on the stack gives the result list a
 *           "node before the head", so the first append is not a special
 *           case. Remove-nth-from-end uses two pointers n apart: when the
 *           leader hits the end, the follower is just before the victim.
 *           Merge sort is THE sort for linked lists: O(n log n), no random
 *           access needed, O(1) extra memory for the merge.
 * Build   : make run P=dsa/06-linked-lists/c/06_merge_and_remove_nth.c
 * Read    : ../README.md  (sections "Merge two sorted lists", "Remove n-th
 *           from end")
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

static Node *from_array(const int *a, size_t n)
{
    Node dummy = {0, NULL}, *tail = &dummy;
    for (size_t i = 0; i < n; i++) {
        Node *node = malloc(sizeof *node);
        if (!node) { perror("malloc"); exit(EXIT_FAILURE); }
        node->value = a[i];
        node->next = NULL;
        tail->next = node;
        tail = node;
    }
    return dummy.next;
}

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
    printf("%-22s", label);
    for (; h; h = h->next) printf("[%d] -> ", h->value);
    printf("NULL\n");
}

/* O(n + m) time, O(1) extra space: we RELINK the existing nodes, no copies.
 * Ownership: both input lists are consumed; the result owns all nodes.
 *
 *   dummy -> ?          tail = &dummy
 *   a: [1] -> [4]       b: [2] -> [3]
 *
 *   take 1 from a:  dummy -> [1]                tail=[1]
 *   take 2 from b:  dummy -> [1] -> [2]         tail=[2]
 *   take 3 from b:  dummy -> [1] -> [2] -> [3]  tail=[3]
 *   b empty: tail->next = a (the rest [4], already sorted, one link!)
 *   return dummy.next
 *
 * `<=` (not `<`) takes from a on ties: the merge is STABLE.            */
static Node *merge_sorted(Node *a, Node *b)
{
    Node dummy = {0, NULL};
    Node *tail = &dummy;
    while (a && b) {
        if (a->value <= b->value) { tail->next = a; a = a->next; }
        else                      { tail->next = b; b = b->next; }
        tail = tail->next;
    }
    tail->next = a ? a : b;
    return dummy.next;
}

/* O(n) one pass, O(1) space. n is 1-based: n=1 removes the last node.
 *
 *   remove 2nd from end of [1]->[2]->[3]->[4]->[5]:
 *
 *   dummy->[1]->[2]->[3]->[4]->[5]->NULL
 *   lead moves n+1 = 3 steps from dummy:  lead = [3]
 *   then both move until lead == NULL:
 *       lag=dummy lead=[3] -> lag=[1] lead=[4] -> lag=[2] lead=[5]
 *       -> lag=[3] lead=NULL
 *   lag = [3] is the node BEFORE the victim [4]:
 *       lag->next = lag->next->next      [3] ------> [5]
 *
 * The dummy makes "remove the head" (n == length) work with no special case. */
static Node *remove_nth_from_end(Node *head, size_t n, bool *removed)
{
    Node dummy = {0, head};
    Node *lead = &dummy, *lag = &dummy;
    *removed = false;
    for (size_t i = 0; i <= n; i++) {
        if (!lead) return head; /* n > length: nothing to remove */
        lead = lead->next;
    }
    while (lead) { lead = lead->next; lag = lag->next; }
    Node *victim = lag->next;
    lag->next = victim->next;
    free(victim);
    *removed = true;
    return dummy.next;
}

/* Merge sort: O(n log n) time, O(log n) recursion stack.
 * Split with slow/fast (first middle), sort halves, merge.
 *
 *   [4,1,3,2] -> [4,1] | [3,2] -> [4]|[1]  [3]|[2] -> [1,4] [2,3] -> [1,2,3,4] */
static Node *merge_sort(Node *head)
{
    if (!head || !head->next) return head;
    Node *slow = head, *fast = head;
    while (fast->next && fast->next->next) {
        slow = slow->next;
        fast = fast->next->next;
    }
    Node *right = slow->next;
    slow->next = NULL; /* cut: left half now ends at slow */
    return merge_sorted(merge_sort(head), merge_sort(right));
}

int main(void)
{
    Node *a = from_array((int[]){1, 4, 6, 9}, 4);
    Node *b = from_array((int[]){2, 3, 6, 10, 11}, 5);
    print("a:", a);
    print("b:", b);
    Node *m = merge_sorted(a, b);
    print("merge(a, b):", m);
    CHECK(equals(m, (int[]){1, 2, 3, 4, 6, 6, 9, 10, 11}, 9));
    free_list(m);

    CHECK(merge_sorted(NULL, NULL) == NULL);
    m = merge_sorted(NULL, from_array((int[]){5}, 1));
    CHECK(equals(m, (int[]){5}, 1));
    free_list(m);

    bool removed;
    Node *h = from_array((int[]){1, 2, 3, 4, 5}, 5);
    h = remove_nth_from_end(h, 2, &removed);
    print("remove 2nd from end:", h);
    CHECK(removed && equals(h, (int[]){1, 2, 3, 5}, 4));
    h = remove_nth_from_end(h, 4, &removed); /* the head */
    CHECK(removed && equals(h, (int[]){2, 3, 5}, 3));
    h = remove_nth_from_end(h, 1, &removed); /* the tail */
    CHECK(removed && equals(h, (int[]){2, 3}, 2));
    h = remove_nth_from_end(h, 9, &removed); /* out of range */
    CHECK(!removed && equals(h, (int[]){2, 3}, 2));
    h = remove_nth_from_end(h, 1, &removed);
    h = remove_nth_from_end(h, 1, &removed);
    CHECK(removed && h == NULL);
    print("after more removals:", h);

    h = from_array((int[]){5, 1, 4, 2, 8, 3, 7, 6, 2}, 9);
    h = merge_sort(h);
    print("merge_sort:", h);
    CHECK(equals(h, (int[]){1, 2, 2, 3, 4, 5, 6, 7, 8}, 9));
    free_list(h);

    /* Pseudo-random bigger test: result must be sorted and keep its size. */
    int vals[2000];
    unsigned x = 12345;
    for (int i = 0; i < 2000; i++) { x = x * 1103515245u + 12345u; vals[i] = (int)(x >> 16) % 1000; }
    h = merge_sort(from_array(vals, 2000));
    size_t count = 0;
    for (Node *p = h; p; p = p->next, count++)
        if (p->next) CHECK(p->value <= p->next->value);
    CHECK(count == 2000);
    free_list(h);

    printf("06_merge_and_remove_nth: all checks passed\n");
    return 0;
}
