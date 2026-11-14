/*
 * 01_stack_array.c — stack on a contiguous array: push, pop, peek, grow
 *
 * Concept : A stack is "last in, first out" (LIFO). Backed by an array it is
 *           just a buffer plus one index `top`. Push writes at top++, pop
 *           reads at --top. A fixed-capacity version (no malloc, like a
 *           firmware stack) and a growable version (realloc x2) are shown.
 * Build   : make run P=dsa/07-stacks-and-queues/c/01_stack_array.c
 * Read    : ../README.md  (section "Stack on an array")
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

/* ------------------------------------------------------------------------ */
/* Part 1: fixed-capacity stack — no heap at all (embedded-friendly)        */
/* ------------------------------------------------------------------------ */
/*
 *   data: [ 10 | 20 | 30 |    |    |    |    |    ]
 *            0    1    2    3 ...
 *                           ^
 *                           top = 3  (= number of elements = next free slot)
 *
 *   push(40): data[top] = 40; top++      -> top = 4
 *   pop()   : top--; return data[top]    -> returns 40, top = 3
 *
 *   Invariant: elements live in data[0 .. top-1]. top==0 means empty,
 *   top==CAP means full. One integer tells you everything.
 */
#define FIXED_CAP 8

typedef struct {
    int data[FIXED_CAP];
    size_t top; /* count of elements, also index of next free slot */
} FixedStack;

static void fs_init(FixedStack *s) { s->top = 0; }
static bool fs_empty(const FixedStack *s) { return s->top == 0; }
static bool fs_full(const FixedStack *s) { return s->top == FIXED_CAP; }

/* O(1). Returns false instead of overflowing: in C nobody stops you from
 * writing data[8], it silently corrupts whatever lives after the array. */
static bool fs_push(FixedStack *s, int v)
{
    if (fs_full(s))
        return false; /* "stack overflow" — refuse, never write past end */
    s->data[s->top++] = v;
    return true;
}

/* O(1). Returns false on underflow (pop of empty stack). */
static bool fs_pop(FixedStack *s, int *out)
{
    if (fs_empty(s))
        return false;
    *out = s->data[--s->top]; /* pre-decrement: top pointed at the free slot */
    return true;
}

/* O(1). Look without removing. */
static bool fs_peek(const FixedStack *s, int *out)
{
    if (fs_empty(s))
        return false;
    *out = s->data[s->top - 1];
    return true;
}

/* ------------------------------------------------------------------------ */
/* Part 2: growable stack — heap buffer doubled on demand                   */
/* ------------------------------------------------------------------------ */
/*
 * Ownership: dstack_init() does not allocate; the first push mallocs.
 * dstack_free() releases the buffer. The caller owns the DynStack struct.
 *
 * Growth by DOUBLING gives amortised O(1) push: copying n elements happens
 * only after n cheap pushes, so the total copy cost over n pushes is
 * 1 + 2 + 4 + ... + n < 2n  ->  O(1) per push on average.
 * Growing by +1 each time would cost 1+2+...+n = O(n^2) total.
 */
typedef struct {
    int *data;
    size_t top;
    size_t cap;
    size_t grow_count; /* only for the lesson: how many reallocs happened */
} DynStack;

static void dstack_init(DynStack *s)
{
    s->data = NULL;
    s->top = s->cap = s->grow_count = 0;
}

static void dstack_free(DynStack *s)
{
    free(s->data);
    dstack_init(s);
}

/* Amortised O(1); a single push is O(n) when it triggers a grow. */
static bool dstack_push(DynStack *s, int v)
{
    if (s->top == s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 4;
        /* realloc into a temporary: if it fails, the old buffer is still
         * valid and still owned by us — assigning straight to s->data
         * would leak it. */
        int *p = realloc(s->data, ncap * sizeof *p);
        if (!p)
            return false;
        s->data = p;
        s->cap = ncap;
        s->grow_count++;
    }
    s->data[s->top++] = v;
    return true;
}

/* O(1). We never shrink here; a real implementation might shrink to half
 * when the stack falls to a QUARTER full (not half — otherwise a push/pop
 * at the boundary would realloc every time: "thrashing"). */
static bool dstack_pop(DynStack *s, int *out)
{
    if (s->top == 0)
        return false;
    *out = s->data[--s->top];
    return true;
}

/* ------------------------------------------------------------------------ */
/* Part 3: an application — reverse a string with a stack                  */
/* ------------------------------------------------------------------------ */
/* O(n) time, O(n) space. LIFO naturally reverses order. */
static void reverse_with_stack(const char *in, char *out, size_t outsz)
{
    DynStack s;
    dstack_init(&s);
    for (const char *p = in; *p; p++)
        CHECK(dstack_push(&s, (unsigned char)*p));
    size_t i = 0;
    int c;
    while (i + 1 < outsz && dstack_pop(&s, &c))
        out[i++] = (char)c;
    out[i] = '\0';
    dstack_free(&s);
}

int main(void)
{
    /* --- fixed stack --- */
    FixedStack fs;
    fs_init(&fs);
    CHECK(fs_empty(&fs));
    for (int i = 1; i <= FIXED_CAP; i++)
        CHECK(fs_push(&fs, i * 10));
    CHECK(fs_full(&fs));
    CHECK(!fs_push(&fs, 999)); /* overflow refused */
    int v;
    CHECK(fs_peek(&fs, &v) && v == 80);
    printf("fixed stack popped:");
    for (int expect = 80; expect >= 10; expect -= 10) {
        CHECK(fs_pop(&fs, &v) && v == expect); /* LIFO order */
        printf(" %d", v);
    }
    printf("\n");
    CHECK(!fs_pop(&fs, &v)); /* underflow refused */

    /* --- dynamic stack --- */
    DynStack ds;
    dstack_init(&ds);
    const int N = 1000;
    for (int i = 0; i < N; i++)
        CHECK(dstack_push(&ds, i));
    /* 4 -> 8 -> ... -> 1024 : 9 reallocs for 1000 pushes, not 1000 */
    printf("dynamic stack: %d pushes, capacity %zu, reallocs %zu\n", N,
           ds.cap, ds.grow_count);
    CHECK(ds.cap == 1024 && ds.grow_count == 9);
    for (int i = N - 1; i >= 0; i--)
        CHECK(dstack_pop(&ds, &v) && v == i);
    CHECK(!dstack_pop(&ds, &v));
    dstack_free(&ds);

    /* --- application --- */
    char out[32];
    reverse_with_stack("stack", out, sizeof out);
    printf("reverse(\"stack\") = \"%s\"\n", out);
    CHECK(out[0] == 'k' && out[4] == 's' && out[5] == '\0');

    printf("01_stack_array: all checks passed\n");
    return 0;
}
