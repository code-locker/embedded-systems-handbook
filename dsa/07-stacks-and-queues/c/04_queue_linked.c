/*
 * 04_queue_linked.c — FIFO queue on a singly linked list with head + tail
 *
 * Concept : A queue removes at the FRONT and inserts at the BACK. With a
 *           singly linked list, removing at the head is O(1); inserting at
 *           the tail is O(1) only if we keep a tail pointer. Dequeue from
 *           the tail would be O(n) (no prev pointer) — so the direction of
 *           the arrows is chosen deliberately: head = front = oldest.
 * Build   : make run P=dsa/07-stacks-and-queues/c/04_queue_linked.c
 * Read    : ../README.md  (section "Queue on a linked list")
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

typedef struct QNode {
    int value;
    struct QNode *next; /* points toward the BACK (newer element) */
} QNode;

typedef struct {
    QNode *head; /* front: dequeue here */
    QNode *tail; /* back : enqueue here */
    size_t size;
} Queue;

static void q_init(Queue *q)
{
    q->head = q->tail = NULL;
    q->size = 0;
}

/*
 * enqueue(30), O(1):
 *
 *   head -> [10] -> [20] -> NULL
 *                    ^
 *                   tail
 *
 *   tail->next = n;   head -> [10] -> [20] -> [30] -> NULL
 *   tail = n;                                  ^ tail
 *
 * Special case: empty queue. There is no tail->next to write, and the new
 * node is BOTH head and tail.
 */
static bool q_enqueue(Queue *q, int v)
{
    QNode *n = malloc(sizeof *n);
    if (!n)
        return false;
    n->value = v;
    n->next = NULL;
    if (q->tail)
        q->tail->next = n;
    else
        q->head = n; /* was empty */
    q->tail = n;
    q->size++;
    return true;
}

/*
 * dequeue(), O(1):
 *
 *   head -> [10] -> [20] -> [30] -> NULL
 *   old = head; head = old->next; free(old)
 *   head -> [20] -> [30] -> NULL
 *
 * Classic bug: when the LAST node leaves, head becomes NULL but tail still
 * points at the freed node. The next enqueue would write tail->next into
 * freed memory. So: if head became NULL, tail must become NULL too.
 */
static bool q_dequeue(Queue *q, int *out)
{
    if (!q->head)
        return false;
    QNode *old = q->head;
    *out = old->value;
    q->head = old->next;
    if (!q->head)
        q->tail = NULL; /* queue is empty again: no dangling tail */
    free(old);
    q->size--;
    return true;
}

static void q_clear(Queue *q) /* O(n) */
{
    int dummy;
    while (q_dequeue(q, &dummy)) {
    }
}

/*
 * Application: breadth-first order. A queue processes things in the order
 * they were discovered: here, "which numbers can we reach from 1 using
 * x*2 and x+3", level by level — the same skeleton as BFS on a graph or a
 * work queue in a kernel workqueue / thread pool.
 */
static void bfs_demo(void)
{
    Queue q;
    q_init(&q);
    bool seen[64] = {false};
    int order[64];
    int n = 0;
    CHECK(q_enqueue(&q, 1));
    seen[1] = true;
    int x;
    while (q_dequeue(&q, &x)) {
        order[n++] = x;
        int next[2] = {x * 2, x + 3};
        for (int i = 0; i < 2; i++)
            if (next[i] < 20 && !seen[next[i]]) {
                seen[next[i]] = true;
                CHECK(q_enqueue(&q, next[i]));
            }
    }
    printf("bfs order:");
    for (int i = 0; i < n; i++)
        printf(" %d", order[i]);
    printf("\n");
    /* level 0: 1 | level 1: 2 4 | level 2: 5 8 7 | ... */
    CHECK(order[0] == 1 && order[1] == 2 && order[2] == 4 && order[3] == 5);
    CHECK(q.head == NULL && q.tail == NULL);
}

int main(void)
{
    Queue q;
    q_init(&q);
    int v;
    CHECK(!q_dequeue(&q, &v));

    for (int i = 1; i <= 4; i++)
        CHECK(q_enqueue(&q, i * 10));
    CHECK(q_dequeue(&q, &v) && v == 10); /* FIFO: first in, first out */
    CHECK(q_dequeue(&q, &v) && v == 20);
    CHECK(q_enqueue(&q, 50));
    CHECK(q_dequeue(&q, &v) && v == 30);
    CHECK(q_dequeue(&q, &v) && v == 40);
    CHECK(q_dequeue(&q, &v) && v == 50);
    CHECK(q.tail == NULL); /* the "dangling tail" bug is not present */

    /* Reuse after becoming empty — would crash/ASan-fail with the bug */
    CHECK(q_enqueue(&q, 7));
    CHECK(q.head == q.tail && q.head->value == 7);
    CHECK(q_enqueue(&q, 8));
    q_clear(&q);
    CHECK(q.size == 0);

    bfs_demo();
    printf("04_queue_linked: all checks passed\n");
    return 0;
}
