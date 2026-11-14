/*
 * 07_generic_sort_qsort.c — type-generic sorting in C: void*, element size and
 *                           a comparator function pointer (exactly like qsort)
 *
 * Concept : C has no templates, so a generic sort works on raw BYTES:
 *              base  : void* to the first element
 *              n     : number of elements
 *              size  : bytes per element   (element i lives at base + i*size)
 *              cmp   : int (*)(const void *, const void *) -> <0, 0, >0
 *           We write our own generic stable merge sort with that signature,
 *           test it against libc qsort() on ints, structs (multi-key) and
 *           strings, and use bsearch() on the result.
 *           Cost of genericity: an indirect call per comparison (no inlining)
 *           and memcpy-based moves — why C++ std::sort is often 2-3x faster.
 * Build   : make run P=dsa/14-sorting-and-searching/c/07_generic_sort_qsort.c
 * Read    : ../README.md  (section "3.10 Generic sorting: qsort vs templates")
 */
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*cmp_fn)(const void *, const void *);

static uint32_t rng_state = 8086u;
static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return rng_state = x;
}

/* ---------------------------------------------------------------------------
 * Generic merge of base[lo..mid) and base[mid..hi) via tmp.
 * Pointer arithmetic must be done on char* (void* arithmetic is a GNU
 * extension that -Wpedantic rejects): element i is at  (char*)base + i*size.
 *
 *   base -> | e0 | e1 | e2 | e3 |      each box = 'size' bytes
 *            ^ +0  ^ +size  ^ +2*size
 * --------------------------------------------------------------------------- */
static void gmerge(char *base, char *tmp, size_t lo, size_t mid, size_t hi,
                   size_t size, cmp_fn cmp)
{
    size_t i = lo, j = mid, k = lo;
    while (i < mid && j < hi) {
        /* cmp(left, right) <= 0 -> take left: ties go left -> STABLE */
        if (cmp(base + i * size, base + j * size) <= 0)
            memcpy(tmp + k++ * size, base + i++ * size, size);
        else
            memcpy(tmp + k++ * size, base + j++ * size, size);
    }
    if (i < mid) memcpy(tmp + k * size, base + i * size, (mid - i) * size);
    else if (j < hi) memcpy(tmp + k * size, base + j * size, (hi - j) * size);
    memcpy(base + lo * size, tmp + lo * size, (hi - lo) * size);
}

/* Small runs: generic insertion sort. Needs one element of scratch ('key'). */
static void ginsertion(char *base, size_t lo, size_t hi, size_t size, cmp_fn cmp, char *key)
{
    for (size_t i = lo + 1; i < hi; i++) {
        memcpy(key, base + i * size, size);
        size_t j = i;
        while (j > lo && cmp(base + (j - 1) * size, key) > 0) {
            memcpy(base + j * size, base + (j - 1) * size, size);
            j--;
        }
        memcpy(base + j * size, key, size);
    }
}

static void gmsort_rec(char *base, char *tmp, size_t lo, size_t hi, size_t size, cmp_fn cmp)
{
    if (hi - lo <= 16) {                    /* insertion sort beats merging tiny runs */
        ginsertion(base, lo, hi, size, cmp, tmp + lo * size);  /* tmp slot as 'key' */
        return;
    }
    size_t mid = lo + (hi - lo) / 2;
    gmsort_rec(base, tmp, lo, mid, size, cmp);
    gmsort_rec(base, tmp, mid, hi, size, cmp);
    if (cmp(base + (mid - 1) * size, base + mid * size) <= 0) return;  /* already ordered */
    gmerge(base, tmp, lo, mid, hi, size, cmp);
}

/* Public API — same shape as qsort, but STABLE and returns -1 on OOM.
 * Ownership: allocates an n*size scratch buffer, frees it before returning. */
static int stable_sort(void *base, size_t n, size_t size, cmp_fn cmp)
{
    if (n < 2) return 0;
    char *tmp = malloc(n * size);
    if (!tmp) return -1;
    gmsort_rec(base, tmp, 0, n, size, cmp);
    free(tmp);
    return 0;
}

/* ---------------------------------------------------------------------------
 * COMPARATORS. The contract: negative / zero / positive, and CONSISTENT
 * (a<b and b<c => a<c). Breaking the contract is undefined behaviour for
 * qsort and has crashed real programs (out-of-bounds reads in some libcs).
 * --------------------------------------------------------------------------- */
static int cmp_int_asc(const void *p, const void *q)
{
    int a = *(const int *)p, b = *(const int *)q;
    return (a > b) - (a < b);      /* -1/0/1, never overflows */
}

/* THE classic bug: 'return a - b;' overflows for a=INT_MAX, b=-1 -> negative,
 * so INT_MAX would sort BEFORE -1. Signed overflow is UB (UBSan would flag
 * it), so we do the subtraction in unsigned arithmetic, which wraps the same
 * way the hardware does, just to SHOW the value the buggy comparator sees. */
static int cmp_int_buggy_result(int a, int b)
{
    return (int)(uint32_t)((uint32_t)a - (uint32_t)b);   /* what int a-b wraps to */
}

static int cmp_int_desc(const void *p, const void *q) { return cmp_int_asc(q, p); }

typedef struct {
    char   name[16];
    int    prio;        /* sort by prio DESC, then name ASC */
    int    seq;         /* original position, to verify stability */
} task_t;

static int cmp_task_prio_name(const void *p, const void *q)
{
    const task_t *a = p, *b = q;
    if (a->prio != b->prio) return (a->prio < b->prio) - (a->prio > b->prio);  /* desc */
    return strcmp(a->name, b->name);
}
static int cmp_task_prio(const void *p, const void *q)
{
    const task_t *a = p, *b = q;
    return (a->prio < b->prio) - (a->prio > b->prio);
}

/* Array of task_t* : same unwrapping idea as for strings below. */
static int cmp_taskptr(const void *p, const void *q)
{
    const task_t *const *a = p, *const *b = q;
    return cmp_task_prio_name(*a, *b);
}

/* Array of char* : qsort hands us POINTERS TO the elements, i.e. char**.
 * Passing strcmp directly would compare the pointer bytes as text — a
 * famous bug. Unwrap one level first. */
static int cmp_cstr(const void *p, const void *q)
{
    const char *const *a = p, *const *b = q;
    return strcmp(*a, *b);
}

int main(void)
{
    printf("=== Generic sort with comparator function pointers ===\n\n");

    /* ---- ints, both directions, against qsort ----------------------------- */
    enum { MAXN = 3000 };
    static int x[MAXN], ref[MAXN];
    for (int t = 0; t < 200; t++) {
        size_t n = xorshift32() % MAXN;
        for (size_t i = 0; i < n; i++) x[i] = ref[i] = (int)xorshift32();   /* full range, negatives */
        cmp_fn c = (t & 1) ? cmp_int_desc : cmp_int_asc;
        assert(stable_sort(x, n, sizeof *x, c) == 0);
        qsort(ref, n, sizeof *ref, c);
        assert(memcmp(x, ref, n * sizeof *x) == 0);
    }
    printf("ints (asc and desc, full 32-bit range): stable_sort == qsort on 200 arrays\n");

    printf("\nWhy never 'return a - b':  a=INT_MAX, b=-1 -> a-b wraps to %d (says a < b!)\n",
           cmp_int_buggy_result(INT_MAX, -1));
    assert(cmp_int_buggy_result(INT_MAX, -1) < 0);
    int big = INT_MAX, neg = -1;
    assert(cmp_int_asc(&big, &neg) > 0);

    /* ---- structs, multi-key ----------------------------------------------- */
    task_t tasks[] = {
        {"uart", 2, 0}, {"dma", 5, 1}, {"gpio", 2, 2}, {"i2c", 5, 3},
        {"spi", 1, 4},  {"can", 2, 5}, {"eth", 5, 6},
    };
    size_t nt = sizeof tasks / sizeof tasks[0];
    task_t t2[sizeof tasks / sizeof tasks[0]];
    memcpy(t2, tasks, sizeof tasks);

    assert(stable_sort(tasks, nt, sizeof *tasks, cmp_task_prio_name) == 0);
    printf("\nTasks by priority desc, then name asc:\n  ");
    for (size_t i = 0; i < nt; i++) printf("%s(%d) ", tasks[i].name, tasks[i].prio);
    printf("\n");
    assert(strcmp(tasks[0].name, "dma") == 0 && strcmp(tasks[6].name, "spi") == 0);

    /* Stability lets you sort by key 2 then by key 1 and get "key1, then key2"
     * — the trick behind clicking column headers in a table UI. */
    assert(stable_sort(t2, nt, sizeof *t2, cmp_task_prio) == 0);
    printf("Tasks by priority only (stable -> equal prios keep insertion order):\n  ");
    for (size_t i = 0; i < nt; i++) printf("%s(%d,seq%d) ", t2[i].name, t2[i].prio, t2[i].seq);
    printf("\n");
    for (size_t i = 1; i < nt; i++)
        if (t2[i - 1].prio == t2[i].prio) assert(t2[i - 1].seq < t2[i].seq);

    /* ---- strings -------------------------------------------------------- */
    const char *words[] = {"pipeline", "buffer", "caps", "pad", "element", "bin", "bus"};
    size_t nw = sizeof words / sizeof words[0];
    assert(stable_sort(words, nw, sizeof *words, cmp_cstr) == 0);
    printf("\nStrings (array of char*, comparator unwraps char**):\n  ");
    for (size_t i = 0; i < nw; i++) printf("%s ", words[i]);
    printf("\n");
    for (size_t i = 1; i < nw; i++) assert(strcmp(words[i - 1], words[i]) <= 0);

    /* ---- bsearch: same comparator, sorted array ---------------------------- */
    const char *key = "caps";
    const char **hit = bsearch(&key, words, nw, sizeof *words, cmp_cstr);
    assert(hit && strcmp(*hit, "caps") == 0);
    const char *missing = "queue";
    assert(bsearch(&missing, words, nw, sizeof *words, cmp_cstr) == NULL);
    printf("bsearch(\"caps\") -> index %td, bsearch(\"queue\") -> NULL\n", hit - words);

    /* ---- big records: sort pointers/indices, not the records -------------- */
    printf("\nTip: for large records, sort an array of POINTERS (8 bytes moved per\n"
           "swap instead of the whole struct), then walk the records in that order.\n");
    task_t *ptrs[sizeof tasks / sizeof tasks[0]];
    for (size_t i = 0; i < nt; i++) ptrs[i] = &t2[i];
    /* Elements are task_t*, so the comparator receives task_t** — passing
     * cmp_task_prio_name here would read pointer bytes as a task_t
     * (ASan: stack-buffer-overflow). cmp_taskptr unwraps one level. */
    qsort(ptrs, nt, sizeof *ptrs, cmp_taskptr);
    for (size_t i = 1; i < nt; i++) assert(cmp_task_prio_name(ptrs[i - 1], ptrs[i]) <= 0);
    printf("  sorted %zu pointers; first = %s, last = %s\n", nt, ptrs[0]->name, ptrs[nt - 1]->name);

    printf("\nAll generic sort checks passed.\n");
    return 0;
}
