/*
 * 02_stack_linked.c — stack on a singly linked list (push/pop at the head)
 *
 * Concept : The head of a singly linked list is the only place where both
 *           insert and delete are O(1) without extra pointers — exactly what
 *           a stack needs. No capacity limit, no realloc copies, but one
 *           malloc per push and poor cache locality.
 * Build   : make run P=dsa/07-stacks-and-queues/c/02_stack_linked.c
 * Read    : ../README.md  (section "Stack on a linked list")
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

typedef struct Node {
    int value;
    struct Node *next; /* the node BELOW this one in the stack */
} Node;

typedef struct {
    Node *top; /* NULL when empty */
    size_t size;
} LStack;

static void ls_init(LStack *s)
{
    s->top = NULL;
    s->size = 0;
}

/*
 * push(30), O(1):
 *
 *   before:  top -> [20] -> [10] -> NULL
 *
 *   1. n = malloc            n: [30] -> ?
 *   2. n->next = top         n: [30] ----+
 *                                        v
 *                            top -> [20] -> [10] -> NULL
 *   3. top = n               top -> [30] -> [20] -> [10] -> NULL
 *
 * Order matters: if you did step 3 before step 2 you would lose [20].
 * Ownership: the stack owns every node it mallocs.
 */
static bool ls_push(LStack *s, int v)
{
    Node *n = malloc(sizeof *n);
    if (!n)
        return false;
    n->value = v;
    n->next = s->top;
    s->top = n;
    s->size++;
    return true;
}

/*
 * pop(), O(1):
 *
 *   before:  top -> [30] -> [20] -> [10] -> NULL
 *   1. old = top             (remember it — we must free it)
 *   2. top = old->next       top -> [20] -> [10] -> NULL
 *   3. read old->value, free(old)
 *
 * Read the value BEFORE free: reading after free is use-after-free
 * (ASan reports it as heap-use-after-free).
 */
static bool ls_pop(LStack *s, int *out)
{
    if (!s->top)
        return false;
    Node *old = s->top;
    s->top = old->next;
    *out = old->value;
    free(old);
    s->size--;
    return true;
}

static bool ls_peek(const LStack *s, int *out)
{
    if (!s->top)
        return false;
    *out = s->top->value;
    return true;
}

/* O(n): free every node. Grab next before freeing the current node. */
static void ls_clear(LStack *s)
{
    Node *n = s->top;
    while (n) {
        Node *next = n->next;
        free(n);
        n = next;
    }
    ls_init(s);
}

/*
 * Application: an "undo" history, the classic real-world stack.
 * Every edit is pushed; undo pops the most recent one.
 */
int main(void)
{
    LStack s;
    ls_init(&s);
    int v;
    CHECK(!ls_pop(&s, &v)); /* underflow is reported, not a crash */

    for (int i = 1; i <= 5; i++)
        CHECK(ls_push(&s, i));
    CHECK(s.size == 5);
    CHECK(ls_peek(&s, &v) && v == 5);

    printf("popping:");
    for (int expect = 5; expect >= 3; expect--) {
        CHECK(ls_pop(&s, &v) && v == expect);
        printf(" %d", v);
    }
    printf("\n");
    CHECK(s.size == 2);

    /* undo history demo */
    int doc = 0; /* our "document" is a single number */
    LStack undo;
    ls_init(&undo);
    const int edits[] = {+5, +10, -3, +100};
    for (size_t i = 0; i < sizeof edits / sizeof edits[0]; i++) {
        doc += edits[i];
        CHECK(ls_push(&undo, edits[i])); /* remember how to reverse it */
    }
    CHECK(doc == 112);
    int e;
    CHECK(ls_pop(&undo, &e)); /* undo +100 */
    doc -= e;
    CHECK(ls_pop(&undo, &e)); /* undo -3 */
    doc -= e;
    printf("after 2 undos doc = %d\n", doc);
    CHECK(doc == 15);

    /* Leave nodes in both stacks on purpose: ls_clear must free them, or
     * LeakSanitizer would fail this program at exit. */
    ls_clear(&undo);
    ls_clear(&s);
    CHECK(s.top == NULL && s.size == 0);

    printf("02_stack_linked: all checks passed\n");
    return 0;
}
