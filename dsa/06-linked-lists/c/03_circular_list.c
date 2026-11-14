/*
 * 03_circular_list.c — circular singly linked list kept by its TAIL pointer,
 *                      plus a round-robin scheduler and the Josephus problem
 *
 * Concept : In a circular list the last node points back to the first, so
 *           there is no NULL. If you keep a pointer to the TAIL, the head is
 *           tail->next, which makes both push_front and push_back O(1) with
 *           a single pointer. Loops must stop when they come back to the
 *           start, not when they hit NULL.
 * Build   : make run P=dsa/06-linked-lists/c/03_circular_list.c
 * Read    : ../README.md  (section "Circular linked list")
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

typedef struct CNode {
    int value;
    struct CNode *next;
} CNode;

/* The whole list is ONE pointer: tail. Empty list: tail == NULL.
 *
 *                     tail
 *                      |
 *                      v
 *   [A] -> [B] -> [C] --+        tail is [C]; the head is not stored,
 *    ^                  |        it is always tail->next == [A]
 *    +------------------+                                              */
typedef struct {
    CNode *tail;
    size_t size;
} CList;

static CNode *new_node(int value)
{
    CNode *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(EXIT_FAILURE); }
    n->value = value;
    n->next = n; /* a single node is a circle of one: it points at itself */
    return n;
}

/* O(1). Splice n between tail and head.
 *
 *   tail -> [C] -> [A] ...       n->next = tail->next   ([N] -> [A])
 *                                tail->next = n          ([C] -> [N])
 *   tail -> [C] -> [N] -> [A] ...   N is the new head; tail unchanged   */
static void push_front(CList *l, int value)
{
    CNode *n = new_node(value);
    if (l->tail) {
        n->next = l->tail->next;
        l->tail->next = n;
    } else {
        l->tail = n;
    }
    l->size++;
}

/* O(1): exactly push_front, then advance tail onto the new node.
 * This trick is why the tail (not the head) is the pointer to keep. */
static void push_back(CList *l, int value)
{
    push_front(l, value);
    l->tail = l->tail->next;
}

/* O(1). Remove head = tail->next. */
static bool pop_front(CList *l, int *out)
{
    if (!l->tail) return false;
    CNode *head = l->tail->next;
    *out = head->value;
    if (head == l->tail) l->tail = NULL;     /* it was the only node */
    else                 l->tail->next = head->next;
    free(head);
    l->size--;
    return true;
}

/* O(1): the round-robin step. The old head becomes the tail. A scheduler
 * does exactly this: run the task at the head, then rotate. */
static void rotate(CList *l)
{
    if (l->tail) l->tail = l->tail->next;
}

/* O(n). Note the do/while: a while(n != head) loop would stop before
 * printing anything because we START at head. */
static void print(const char *label, const CList *l)
{
    printf("%-20s", label);
    if (!l->tail) { printf("(empty)\n"); return; }
    const CNode *head = l->tail->next, *n = head;
    do {
        printf("[%d] -> ", n->value);
        n = n->next;
    } while (n != head);
    printf("(back to [%d])\n", head->value);
}

static bool equals(const CList *l, const int *expect, size_t cnt)
{
    if (l->size != cnt) return false;
    if (cnt == 0) return l->tail == NULL;
    const CNode *n = l->tail->next;
    for (size_t i = 0; i < cnt; i++, n = n->next)
        if (n->value != expect[i]) return false;
    return n == l->tail->next; /* after cnt steps we are back at head */
}

static void list_free(CList *l)
{
    int v;
    while (pop_front(l, &v)) {}
}

/* Josephus: n people in a circle, every k-th is eliminated. Who survives?
 * O(n*k) by simulation — a circular list is the natural model. */
static int josephus(int people, int k)
{
    CList l = {NULL, 0};
    for (int i = 1; i <= people; i++) push_back(&l, i);
    CNode *prev = l.tail; /* prev->next is the person we are counting at */
    while (l.size > 1) {
        for (int i = 1; i < k; i++) prev = prev->next; /* skip k-1 people */
        CNode *victim = prev->next;
        prev->next = victim->next;         /* unlink: O(1) since we hold prev */
        if (victim == l.tail) l.tail = prev;
        free(victim);
        l.size--;
    }
    int survivor = l.tail->value;
    list_free(&l);
    return survivor;
}

/* Closed form for comparison: J(1)=0, J(n) = (J(n-1)+k) mod n (0-based). */
static int josephus_formula(int people, int k)
{
    int j = 0;
    for (int n = 2; n <= people; n++) j = (j + k) % n;
    return j + 1;
}

int main(void)
{
    CList l = {NULL, 0};
    push_back(&l, 2);
    push_back(&l, 3);
    push_front(&l, 1);
    print("push:", &l);
    CHECK(equals(&l, (int[]){1, 2, 3}, 3));
    CHECK(l.tail->next->value == 1); /* head is tail->next */

    /* Round-robin: run head, rotate. Three "tasks" get turns 1,2,3,1,2,3. */
    int order[6];
    for (int t = 0; t < 6; t++) {
        order[t] = l.tail->next->value;
        rotate(&l);
    }
    printf("round-robin turns:   %d %d %d %d %d %d\n",
           order[0], order[1], order[2], order[3], order[4], order[5]);
    CHECK(order[0] == 1 && order[3] == 1 && order[5] == 3);

    int v;
    CHECK(pop_front(&l, &v) && v == 1);
    print("pop_front:", &l);
    CHECK(equals(&l, (int[]){2, 3}, 2));
    list_free(&l);
    print("freed:", &l);
    CHECK(equals(&l, NULL, 0));

    /* Josephus with 7 people, k = 3: eliminated 3,6,2,7,5,1 -> survivor 4. */
    printf("josephus(7,3) = %d\n", josephus(7, 3));
    CHECK(josephus(7, 3) == 4);
    for (int n = 1; n <= 40; n++)
        for (int k = 1; k <= 5; k++)
            CHECK(josephus(n, k) == josephus_formula(n, k));

    printf("03_circular_list: all checks passed\n");
    return 0;
}
