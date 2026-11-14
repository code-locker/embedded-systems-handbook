/*
 * 05_deque.c — double-ended queue on a growable circular buffer
 *
 * Concept : A deque ("deck") allows push/pop at BOTH ends in O(1). On a ring
 *           buffer that is easy: pushing at the front just moves `head`
 *           backwards (with wrap). When full, we grow by allocating a new
 *           buffer twice as big and copying the elements in logical order
 *           ("unrolling" the ring). A deque can act as a stack AND a queue,
 *           and it is the engine behind the sliding-window maximum.
 * Build   : make run P=dsa/07-stacks-and-queues/c/05_deque.c
 * Read    : ../README.md  (section "Deque")
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

/* Ownership: dq_free() releases `buf`. cap is always a power of two so
 * that wrap-around is a mask, and (head - 1) wraps with the same mask. */
typedef struct {
    int *buf;
    size_t cap;  /* power of two (or 0 before first push) */
    size_t head; /* physical index of the logical element 0 */
    size_t size;
} Deque;

static void dq_init(Deque *d)
{
    d->buf = NULL;
    d->cap = d->head = d->size = 0;
}

static void dq_free(Deque *d)
{
    free(d->buf);
    dq_init(d);
}

/* logical index i -> physical slot. O(1). */
static size_t dq_slot(const Deque *d, size_t i)
{
    return (d->head + i) & (d->cap - 1);
}

/*
 * Grow, O(n). The old ring may be split; we copy in LOGICAL order so the
 * new buffer starts at index 0 and is not split:
 *
 *   old (cap 4, head=2):  [ c | d | a | b ]   logical: a b c d
 *                                   ^head
 *   new (cap 8, head=0):  [ a | b | c | d |   |   |   |   ]
 *
 * A plain realloc would NOT work: it keeps the physical layout, so c,d
 * would stay at 0,1 while the new empty slots appear after b — the
 * logical order would break.
 */
static bool dq_grow(Deque *d)
{
    size_t ncap = d->cap ? d->cap * 2 : 4;
    int *nb = malloc(ncap * sizeof *nb);
    if (!nb)
        return false;
    for (size_t i = 0; i < d->size; i++)
        nb[i] = d->buf[dq_slot(d, i)];
    free(d->buf);
    d->buf = nb;
    d->cap = ncap;
    d->head = 0;
    return true;
}

/* Amortised O(1). */
static bool dq_push_back(Deque *d, int v)
{
    if (d->size == d->cap && !dq_grow(d))
        return false;
    d->buf[dq_slot(d, d->size)] = v;
    d->size++;
    return true;
}

/*
 * Amortised O(1). Move head one step LEFT, wrapping 0 -> cap-1:
 *
 *   [ _ | _ | a | b ]  head=2   push_front(z)
 *   [ _ | z | a | b ]  head=1
 *
 *   [ a | b | _ | _ ]  head=0   push_front(z)
 *   [ a | b | _ | z ]  head=3   (0 - 1) & 3 == 3 for size_t: unsigned
 *                               wrap is defined behaviour in C.
 */
static bool dq_push_front(Deque *d, int v)
{
    if (d->size == d->cap && !dq_grow(d))
        return false;
    d->head = (d->head - 1) & (d->cap - 1);
    d->buf[d->head] = v;
    d->size++;
    return true;
}

static bool dq_pop_front(Deque *d, int *out) /* O(1) */
{
    if (d->size == 0)
        return false;
    *out = d->buf[d->head];
    d->head = (d->head + 1) & (d->cap - 1);
    d->size--;
    return true;
}

static bool dq_pop_back(Deque *d, int *out) /* O(1) */
{
    if (d->size == 0)
        return false;
    d->size--;
    *out = d->buf[dq_slot(d, d->size)];
    return true;
}

static int dq_at(const Deque *d, size_t i) { return d->buf[dq_slot(d, i)]; }

static void dq_print(const Deque *d, const char *label)
{
    printf("%-22s front [", label);
    for (size_t i = 0; i < d->size; i++)
        printf(i ? " %d" : "%d", dq_at(d, i));
    printf("] back   (cap %zu, head %zu)\n", d->cap, d->head);
}

/*
 * Application: palindrome check — compare and pop from both ends.
 * O(n). A deque is the natural shape: "peel the outer layer" each step.
 */
static bool is_palindrome(const char *s)
{
    Deque d;
    dq_init(&d);
    for (; *s; s++)
        CHECK(dq_push_back(&d, (unsigned char)*s));
    bool ok = true;
    int a, b;
    while (d.size > 1) {
        CHECK(dq_pop_front(&d, &a));
        CHECK(dq_pop_back(&d, &b));
        if (a != b) {
            ok = false;
            break;
        }
    }
    dq_free(&d);
    return ok;
}

int main(void)
{
    Deque d;
    dq_init(&d);
    CHECK(dq_push_back(&d, 1));
    CHECK(dq_push_back(&d, 2));
    CHECK(dq_push_front(&d, 0)); /* head wraps to the end of the buffer */
    CHECK(dq_push_front(&d, -1));
    dq_print(&d, "after 4 pushes:");
    CHECK(d.size == 4 && d.cap == 4);
    CHECK(dq_at(&d, 0) == -1 && dq_at(&d, 3) == 2);

    CHECK(dq_push_back(&d, 3)); /* full -> grow + unroll */
    dq_print(&d, "after grow:");
    CHECK(d.cap == 8 && d.head == 0);
    for (int i = 0; i < 5; i++)
        CHECK(dq_at(&d, (size_t)i) == i - 1);

    int v;
    CHECK(dq_pop_back(&d, &v) && v == 3);  /* used like a stack */
    CHECK(dq_pop_front(&d, &v) && v == -1); /* used like a queue */
    dq_print(&d, "after 2 pops:");
    CHECK(d.size == 3);

    /* Stress: random-ish mix, compared against a simple reference array */
    int ref[256];
    int lo = 128, hi = 128; /* reference occupies ref[lo..hi) */
    while (dq_pop_front(&d, &v)) {
    }
    unsigned seed = 12345;
    for (int step = 0; step < 2000; step++) {
        seed = seed * 1103515245u + 12345u;
        int op = (int)((seed >> 16) % 4);
        if (op == 0 && lo > 0) {
            CHECK(dq_push_front(&d, step));
            ref[--lo] = step;
        } else if (op == 1 && hi < 256) {
            CHECK(dq_push_back(&d, step));
            ref[hi++] = step;
        } else if (op == 2 && lo < hi) {
            CHECK(dq_pop_front(&d, &v) && v == ref[lo++]);
        } else if (op == 3 && lo < hi) {
            CHECK(dq_pop_back(&d, &v) && v == ref[--hi]);
        }
        CHECK(d.size == (size_t)(hi - lo));
        if (lo == hi) { /* recentre the reference window */
            lo = hi = 128;
        }
    }
    printf("stress: 2000 mixed ops matched the reference\n");

    CHECK(is_palindrome("racecar"));
    CHECK(is_palindrome("abba"));
    CHECK(!is_palindrome("abca"));
    printf("palindrome via deque: racecar yes, abca no\n");

    dq_free(&d);
    printf("05_deque: all checks passed\n");
    return 0;
}
