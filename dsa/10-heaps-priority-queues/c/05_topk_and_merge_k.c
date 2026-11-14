/*
 * 05_topk_and_merge_k.c — two classic heap applications
 *
 * Concept : (1) Top-k largest of a stream: keep a MIN-heap of size k. Its root
 *           is the smallest of the current top-k, i.e. the "bar to beat".
 *           O(n log k) time, O(k) memory, works on an endless stream.
 *           (2) Merge k sorted arrays: a min-heap holds the current head of
 *           each array; pop the smallest, push its successor. O(N log k).
 * Build   : make run P=dsa/10-heaps-priority-queues/c/05_topk_and_merge_k.c
 * Read    : ../README.md  (section "Applications")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- top-k with a fixed-size min-heap ---------------------------- */

static void sift_down_min(int *a, size_t n, size_t i)   /* O(log n) */
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && a[l] < a[m]) m = l;
        if (r < n && a[r] < a[m]) m = r;
        if (m == i) return;
        int t = a[i]; a[i] = a[m]; a[m] = t;
        i = m;
    }
}

static void sift_up_min(int *a, size_t i)                /* O(log n) */
{
    while (i > 0 && a[(i - 1) / 2] > a[i]) {
        int t = a[i]; a[i] = a[(i - 1) / 2]; a[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

/*
 * top_k — O(n log k). heap[] (caller-owned, size k) receives the k largest.
 *
 *   k=3, stream 5 1 9 3 7 2 8
 *   heap after 5,1,9:  {1,5,9}  root 1 = bar to beat
 *   3 > 1 -> replace:  {3,5,9}
 *   7 > 3 -> replace:  {5,7,9}
 *   2 < 5 -> ignore    (O(1) rejection: the common case for big streams)
 *   8 > 5 -> replace:  {7,8,9}  = answer
 */
static size_t top_k(const int *in, size_t n, int *heap, size_t k)
{
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        if (len < k) {
            heap[len] = in[i];
            sift_up_min(heap, len++);
        } else if (k > 0 && in[i] > heap[0]) {
            heap[0] = in[i];              /* evict the smallest of the top-k */
            sift_down_min(heap, k, 0);
        }
    }
    return len;
}

/* ---------- merge k sorted arrays ---------------------------------------- */

typedef struct { int val; size_t list; size_t pos; } Head;

static int head_less(const Head *x, const Head *y)
{
    if (x->val != y->val) return x->val < y->val;
    return x->list < y->list;            /* tie-break: makes the merge stable */
}

static void hswap(Head *x, Head *y) { Head t = *x; *x = *y; *y = t; }

static void head_sift_down(Head *h, size_t n, size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && head_less(&h[l], &h[m])) m = l;
        if (r < n && head_less(&h[r], &h[m])) m = r;
        if (m == i) return;
        hswap(&h[i], &h[m]);
        i = m;
    }
}

/*
 * merge_k — O(N log k), N = total elements. out must hold N ints.
 *
 *   lists: A=[1 4 7]  B=[2 5]  C=[0 9]
 *   heap of heads {A:1, B:2, C:0} -> pop C:0, push C:9
 *   heap {A:1, B:2, C:9} -> pop A:1, push A:4 ... and so on.
 *   When a list runs out we shrink the heap instead of pushing.
 */
static size_t merge_k(int *const *lists, const size_t *lens, size_t k, int *out)
{
    Head *h = malloc((k ? k : 1) * sizeof *h);     /* owned here, freed below */
    assert(h);
    size_t hn = 0, outn = 0;
    for (size_t i = 0; i < k; i++)
        if (lens[i]) h[hn++] = (Head){lists[i][0], i, 0};
    for (size_t i = hn / 2; i-- > 0;) head_sift_down(h, hn, i);   /* O(k) */

    while (hn) {
        out[outn++] = h[0].val;
        size_t L = h[0].list, P = h[0].pos + 1;
        if (P < lens[L]) {
            h[0] = (Head){lists[L][P], L, P};    /* replace root = pop+push in one sift */
        } else {
            h[0] = h[--hn];                      /* list exhausted: shrink */
        }
        head_sift_down(h, hn, 0);
    }
    free(h);
    return outn;
}

static int cmp_int(const void *x, const void *y)
{
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

int main(void)
{
    int stream[] = {5, 1, 9, 3, 7, 2, 8};
    int heap[3];
    size_t got = top_k(stream, 7, heap, 3);
    qsort(heap, got, sizeof *heap, cmp_int);
    printf("top-3 of {5,1,9,3,7,2,8} = %d %d %d\n", heap[0], heap[1], heap[2]);
    assert(got == 3 && heap[0] == 7 && heap[1] == 8 && heap[2] == 9);

    srand(99);
    enum { MAXN = 2000 };
    int *in = malloc(MAXN * sizeof *in), *ref = malloc(MAXN * sizeof *ref),
        *hk = malloc(MAXN * sizeof *hk);
    assert(in && ref && hk);
    for (int round = 0; round < 200; round++) {
        size_t n = (size_t)(rand() % MAXN), k = (size_t)(rand() % 50);
        for (size_t i = 0; i < n; i++) ref[i] = in[i] = rand() % 1000;
        size_t m = top_k(in, n, hk, k);
        assert(m == (k < n ? k : n));
        qsort(ref, n, sizeof *ref, cmp_int);
        qsort(hk, m, sizeof *hk, cmp_int);
        assert(memcmp(hk, ref + (n - m), m * sizeof *hk) == 0);   /* the m largest */
    }
    printf("200 random top-k runs match sort-and-take-last-k\n");

    /* merge demo */
    int A[] = {1, 4, 7}, B[] = {2, 5}, C[] = {0, 9};
    int *lists[] = {A, B, C};
    size_t lens[] = {3, 2, 2};
    int out[7];
    size_t on = merge_k(lists, lens, 3, out);
    printf("merge [1 4 7] [2 5] [0 9] ->");
    for (size_t i = 0; i < on; i++) printf(" %d", out[i]);
    printf("\n");
    int expect[] = {0, 1, 2, 4, 5, 7, 9};
    assert(on == 7 && memcmp(out, expect, sizeof expect) == 0);

    /* random merge check */
    for (int round = 0; round < 100; round++) {
        size_t k = (size_t)(rand() % 20) + 1, total = 0;
        int **ls = malloc(k * sizeof *ls);
        size_t *ln = malloc(k * sizeof *ln);
        assert(ls && ln);
        for (size_t i = 0; i < k; i++) {
            ln[i] = (size_t)(rand() % 60);           /* empty lists allowed */
            ls[i] = malloc((ln[i] ? ln[i] : 1) * sizeof **ls);
            assert(ls[i]);
            for (size_t j = 0; j < ln[i]; j++) ls[i][j] = rand() % 500;
            qsort(ls[i], ln[i], sizeof **ls, cmp_int);
            memcpy(ref + total, ls[i], ln[i] * sizeof *ref);
            total += ln[i];
        }
        size_t mo = merge_k(ls, ln, k, hk);
        qsort(ref, total, sizeof *ref, cmp_int);
        assert(mo == total && memcmp(hk, ref, total * sizeof *ref) == 0);
        for (size_t i = 0; i < k; i++) free(ls[i]);
        free(ls);
        free(ln);
    }
    printf("100 random k-way merges match qsort of the concatenation\n");

    free(in); free(ref); free(hk);
    printf("all checks passed\n");
    return 0;
}
