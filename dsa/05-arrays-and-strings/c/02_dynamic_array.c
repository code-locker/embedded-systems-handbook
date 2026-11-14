/*
 * 02_dynamic_array.c — a growable array ("vector") from scratch in C
 *
 * Concept : Keep {data, len, cap}. Push writes into spare capacity in O(1);
 *           when full, allocate cap*2, copy, free the old block. Doubling
 *           makes the total copy work over n pushes < 2n, so push is
 *           AMORTISED O(1). Also: insert/erase in the middle (O(n) shifts),
 *           shrink, and why growth must be geometric, not "+10".
 * Build   : make run P=dsa/05-arrays-and-strings/c/02_dynamic_array.c
 * Read    : ../README.md  (section "Dynamic array")
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/*
 *   IntVec v;                       heap block (owned by v)
 *   +---------+                     +----+----+----+----+----+----+----+----+
 *   | data  ---------------------->  | 3  | 1  | 4  | 1  | 5  | ?  | ?  | ?  |
 *   | len = 5 |                     +----+----+----+----+----+----+----+----+
 *   | cap = 8 |                      <------ len ------>  <-- spare --->
 *   +---------+                      <------------------ cap ------------->
 */
typedef struct {
    int *data;
    size_t len;
    size_t cap;
    /* instrumentation for the lesson: */
    size_t reallocs;
    size_t elems_copied;
} IntVec;

static void vec_init(IntVec *v)
{
    memset(v, 0, sizeof *v); /* empty vector owns nothing: data == NULL */
}

static void vec_free(IntVec *v)
{
    free(v->data);           /* free(NULL) is fine */
    vec_init(v);
}

/* O(n) when it runs (copies len elements), but runs rarely.
 * Returns false on allocation failure and leaves v untouched — realloc
 * into a temporary so we never lose the old pointer. */
static bool vec_reserve(IntVec *v, size_t new_cap)
{
    if (new_cap <= v->cap)
        return true;
    if (new_cap > SIZE_MAX / sizeof(int))
        return false; /* new_cap * sizeof(int) would overflow */
    int *p = realloc(v->data, new_cap * sizeof(int));
    if (!p)
        return false; /* v->data still valid and still ours */
    v->data = p;
    v->cap = new_cap;
    v->reallocs++;
    v->elems_copied += v->len; /* worst case: realloc had to move them */
    return true;
}

/* Amortised O(1). Growth factor 2: capacities 0,1,2,4,8,16,...
 *
 *  push into full vec (len == cap == 4):
 *   old: [a b c d]            new: [a b c d e . . .]
 *         ^ freed after copy         len=5, cap=8
 */
static bool vec_push(IntVec *v, int x)
{
    if (v->len == v->cap && !vec_reserve(v, v->cap ? v->cap * 2 : 1))
        return false;
    v->data[v->len++] = x;
    return true;
}

/* O(1). Caller must check len > 0. */
static int vec_pop(IntVec *v)
{
    return v->data[--v->len];
}

/* O(n - i): shift the tail right by one, then write.
 *
 *   insert 9 at i=1:   [3 1 4 1 5 .]
 *                         \ \ \ \           memmove (overlapping!)
 *                      [3 . 1 4 1 5]  ->  [3 9 1 4 1 5]
 */
static bool vec_insert(IntVec *v, size_t i, int x)
{
    if (i > v->len)
        return false;
    if (v->len == v->cap && !vec_reserve(v, v->cap ? v->cap * 2 : 1))
        return false;
    /* memmove, not memcpy: source and destination overlap. */
    memmove(&v->data[i + 1], &v->data[i], (v->len - i) * sizeof(int));
    v->data[i] = x;
    v->len++;
    return true;
}

/* O(n - i): shift the tail left over the hole.
 *   erase i=1:  [3 9 1 4 1 5]  ->  [3 1 4 1 5]
 */
static bool vec_erase(IntVec *v, size_t i)
{
    if (i >= v->len)
        return false;
    memmove(&v->data[i], &v->data[i + 1], (v->len - i - 1) * sizeof(int));
    v->len--;
    return true;
}

/* O(1) "unordered erase": move the last element into the hole.
 * Used in game engines / kernel arrays when order does not matter. */
static void vec_swap_remove(IntVec *v, size_t i)
{
    v->data[i] = v->data[--v->len];
}

/* O(n). Give back unused capacity (like std::vector::shrink_to_fit). */
static void vec_shrink_to_fit(IntVec *v)
{
    if (v->len == 0) {
        free(v->data);
        v->data = NULL;
        v->cap = 0;
        return;
    }
    int *p = realloc(v->data, v->len * sizeof(int));
    if (p) {
        v->data = p;
        v->cap = v->len;
    }
}

/* Total element copies for n pushes with additive growth (+step). */
static size_t copies_additive(size_t n, size_t step)
{
    size_t cap = 0, copies = 0;
    for (size_t len = 0; len < n; len++) {
        if (len == cap) {
            copies += len;
            cap += step;
        }
    }
    return copies;
}

static void print_vec(const char *label, const IntVec *v)
{
    printf("  %-22s len=%zu cap=%zu [", label, v->len, v->cap);
    for (size_t i = 0; i < v->len; i++)
        printf("%s%d", i ? " " : "", v->data[i]);
    puts("]");
}

int main(void)
{
    IntVec v;
    vec_init(&v);

    puts("== 1. push and watch capacity double ==");
    size_t last_cap = 0;
    for (int i = 0; i < 20; i++) {
        CHECK(vec_push(&v, i * i));
        if (v.cap != last_cap) {
            printf("  len=%2zu -> grew to cap=%2zu\n", v.len, v.cap);
            last_cap = v.cap;
        }
    }
    CHECK(v.len == 20 && v.cap == 32);
    for (int i = 0; i < 20; i++)
        CHECK(v.data[i] == i * i);
    vec_free(&v);

    puts("\n== 2. insert / erase / swap_remove ==");
    int init[] = {3, 1, 4, 1, 5};
    for (size_t i = 0; i < 5; i++)
        CHECK(vec_push(&v, init[i]));
    print_vec("start", &v);
    CHECK(vec_insert(&v, 1, 9));
    print_vec("insert(1, 9)", &v);
    CHECK(v.data[1] == 9 && v.data[2] == 1 && v.len == 6);
    CHECK(vec_insert(&v, v.len, 7)); /* insert at end == push */
    CHECK(v.data[6] == 7);
    CHECK(!vec_insert(&v, 100, 0));  /* out of range rejected */
    CHECK(vec_erase(&v, 1));
    print_vec("erase(1)", &v);
    CHECK(v.data[1] == 1);
    vec_swap_remove(&v, 0);           /* 3 replaced by last (7) */
    print_vec("swap_remove(0)", &v);
    CHECK(v.data[0] == 7 && v.len == 5);
    CHECK(vec_pop(&v) == 5);
    vec_shrink_to_fit(&v);
    print_vec("pop + shrink_to_fit", &v);
    CHECK(v.cap == v.len);
    vec_free(&v);

    puts("\n== 3. Amortised cost: doubling vs +10 ==");
    const size_t N = 100000;
    for (size_t i = 0; i < N; i++)
        CHECK(vec_push(&v, (int)i));
    printf("  doubling: %zu pushes -> %zu reallocs, %zu element copies"
           " (%.2f per push)\n",
           N, v.reallocs, v.elems_copied, (double)v.elems_copied / (double)N);
    /* 1 + 2 + 4 + ... + 2^k < 2 * 2^k <= 2N: geometric series bound. */
    CHECK(v.elems_copied < 2 * N);
    vec_free(&v);

    size_t add = copies_additive(N, 10);
    printf("  +10 growth: %zu element copies (%.0f per push) -> O(n^2) total\n",
           add, (double)add / (double)N);
    CHECK(add > 100 * N); /* ~n^2/20: quadratic */

    puts("\nall checks passed");
    return 0;
}
