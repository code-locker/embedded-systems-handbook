/*
 * 05_floyd_cycle.c — detect a cycle (Floyd's tortoise and hare), find where
 *                    the cycle starts, and measure its length
 *
 * Concept : If a list has a cycle, a walker never reaches NULL. Send a slow
 *           pointer (1 step) and a fast one (2 steps): inside the loop the
 *           fast one gains 1 node per step, so it MUST land on slow. Then the
 *           math trick: reset one pointer to head, move both 1 step at a
 *           time — they meet exactly at the cycle's first node.
 * Build   : make run P=dsa/06-linked-lists/c/05_floyd_cycle.c
 * Read    : ../README.md  (section "Floyd cycle detection")
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

/* We allocate nodes as one array so freeing is trivial even when the list
 * has a cycle (a normal "walk and free" loop would never terminate, or
 * double-free). Ownership: caller frees the returned array. */
static Node *make_nodes(size_t n)
{
    Node *nodes = calloc(n, sizeof *nodes);
    if (!nodes) { perror("calloc"); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < n; i++) {
        nodes[i].value = (int)i;
        nodes[i].next = (i + 1 < n) ? &nodes[i + 1] : NULL;
    }
    return nodes;
}

/* O(n) time, O(1) space. Returns the meeting node or NULL. */
static Node *meet_point(Node *head)
{
    Node *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) return slow; /* compare ADDRESSES, not values */
    }
    return NULL; /* fast hit NULL: no cycle */
}

static bool has_cycle(Node *head) { return meet_point(head) != NULL; }

/* Why does resetting to head find the start?
 *
 *   head ---- a ----> S (cycle start) ---- b ----> M (meeting point)
 *                     ^                            |
 *                     +--------- c ----------------+     loop length L = b + c
 *
 *   When they meet: slow walked  a + b
 *                   fast walked  a + b + kL   (some whole laps more)
 *   fast walked twice as far:   2(a + b) = a + b + kL   =>   a = kL - b
 *                                                         =   (k-1)L + c
 *   So walking `a` steps from head reaches S, and walking `a` steps from M
 *   also reaches S (c steps to S, then whole laps). Move both 1 at a time:
 *   they collide at S.                                                  */
static Node *cycle_start(Node *head)
{
    Node *m = meet_point(head);
    if (!m) return NULL;
    Node *p = head;
    while (p != m) {
        p = p->next;
        m = m->next;
    }
    return p;
}

/* From the meeting point, walk once round the loop: O(L). */
static size_t cycle_length(Node *head)
{
    Node *m = meet_point(head);
    if (!m) return 0;
    size_t len = 1;
    for (Node *p = m->next; p != m; p = p->next) len++;
    return len;
}

/* Fix the bug: find the node whose next is the start and cut it. */
static void break_cycle(Node *head)
{
    Node *start = cycle_start(head);
    if (!start) return;
    Node *p = start;
    while (p->next != start) p = p->next; /* last node of the loop */
    p->next = NULL;
}

int main(void)
{
    /* Straight list 0..9: no cycle. */
    Node *nodes = make_nodes(10);
    CHECK(!has_cycle(nodes));
    CHECK(cycle_start(nodes) == NULL && cycle_length(nodes) == 0);
    printf("0..9 straight:        no cycle\n");

    /* Make 9 -> 3:
     *   [0]->[1]->[2]->[3]->[4]->[5]->[6]->[7]->[8]->[9]
     *                   ^                             |
     *                   +-----------------------------+   loop length 7 */
    nodes[9].next = &nodes[3];
    CHECK(has_cycle(nodes));
    printf("9 -> 3:               cycle starts at [%d], length %zu\n",
           cycle_start(nodes)->value, cycle_length(nodes));
    CHECK(cycle_start(nodes) == &nodes[3]);
    CHECK(cycle_length(nodes) == 7);

    break_cycle(nodes);
    CHECK(!has_cycle(nodes) && nodes[9].next == NULL);
    printf("after break_cycle:    no cycle\n");

    /* Edge cases: self-loop, whole-list loop, empty list. */
    nodes[0].next = &nodes[0];
    CHECK(cycle_start(nodes) == &nodes[0] && cycle_length(nodes) == 1);
    nodes[0].next = &nodes[1];
    nodes[9].next = &nodes[0];
    CHECK(cycle_start(nodes) == &nodes[0] && cycle_length(nodes) == 10);
    CHECK(!has_cycle(NULL));
    free(nodes);

    /* Exhaustive: every list length 1..30, every loop target, plus none. */
    for (size_t n = 1; n <= 30; n++) {
        Node *ns = make_nodes(n);
        for (size_t t = 0; t <= n; t++) {
            ns[n - 1].next = (t < n) ? &ns[t] : NULL;
            if (t < n) {
                CHECK(cycle_start(ns) == &ns[t]);
                CHECK(cycle_length(ns) == n - t);
            } else {
                CHECK(!has_cycle(ns));
            }
        }
        free(ns);
    }
    printf("exhaustive 1..30:     ok\n");

    printf("05_floyd_cycle: all checks passed\n");
    return 0;
}
