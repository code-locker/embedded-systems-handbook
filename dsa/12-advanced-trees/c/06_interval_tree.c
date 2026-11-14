/*
 * 06_interval_tree.c — interval tree: an augmented balanced BST for overlap queries
 *
 * Concept : Store closed intervals [lo, hi] in a BST ordered by lo. Augment
 *           every node with maxhi = the largest hi anywhere in its subtree.
 *           Then "does anything overlap [a, b]?" can skip a whole left subtree
 *           when its maxhi < a. Balance comes from a treap (random priorities
 *           + rotations); the key lesson is that EVERY rotation must recompute
 *           maxhi — exactly what the Linux kernel's augmented rbtree callbacks
 *           (include/linux/interval_tree_generic.h) do.
 * Build   : make run P=dsa/12-advanced-trees/c/06_interval_tree.c
 * Read    : ../README.md  (section "Interval tree")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct INode {
    int lo, hi;          /* the interval [lo, hi], lo <= hi              */
    int maxhi;           /* AUGMENTED: max hi in this whole subtree       */
    uint32_t prio;       /* treap heap priority: parent prio >= child prio */
    struct INode *left, *right;
} INode;

/*
 * Example (ordered by lo; number after '|' is maxhi):
 *
 *                 [15,20] | 30
 *                /              \
 *        [5,8] | 10            [17,19] | 30
 *        /                         \
 *   [1,10] | 10                  [25,30] | 30
 *
 * Query [12,14]: root [15,20] does not overlap. left.maxhi = 10 < 12, so NO
 * interval on the left can reach 12 -> skip it entirely. Go right... none.
 */

static uint32_t rng_state = 2024u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int imax(int a, int b) { return a > b ? a : b; }

/* O(1): recompute the augmentation from the node and its two children. */
static void pull(INode *x)
{
    x->maxhi = x->hi;
    if (x->left) x->maxhi = imax(x->maxhi, x->left->maxhi);
    if (x->right) x->maxhi = imax(x->maxhi, x->right->maxhi);
}

/*
 * Right rotation, O(1). Only x and y change subtrees, so only they need pull()
 * — and the LOWER one (x after rotation) must be pulled first.
 *
 *        x                y
 *       / \              / \
 *      y   C    --->    A   x
 *     / \                  / \
 *    A   B                B   C
 */
static INode *rot_right(INode *x)
{
    INode *y = x->left;
    x->left = y->right;
    y->right = x;
    pull(x);
    pull(y);
    return y;
}

static INode *rot_left(INode *x)
{
    INode *y = x->right;
    x->right = y->left;
    y->left = x;
    pull(x);
    pull(y);
    return y;
}

/* Ordering key: (lo, hi) so equal lows are still totally ordered. */
static bool less(int alo, int ahi, int blo, int bhi) { return alo < blo || (alo == blo && ahi < bhi); }

/* Expected O(log n): BST insert, then rotate up while the heap property is violated. */
static INode *it_insert(INode *t, int lo, int hi)
{
    if (!t) {
        INode *n = malloc(sizeof *n); /* owned by the tree, freed in it_free/it_erase */
        assert(n);
        *n = (INode){ lo, hi, hi, rnd(), NULL, NULL };
        return n;
    }
    if (less(lo, hi, t->lo, t->hi)) {
        t->left = it_insert(t->left, lo, hi);
        if (t->left->prio > t->prio) t = rot_right(t);
    } else {
        t->right = it_insert(t->right, lo, hi);
        if (t->right->prio > t->prio) t = rot_left(t);
    }
    pull(t); /* maxhi on the path may have grown */
    return t;
}

/* Expected O(log n): rotate the victim down until it is a leaf-ish node, then unlink. */
static INode *it_erase(INode *t, int lo, int hi, bool *found)
{
    if (!t)
        return NULL;
    if (t->lo == lo && t->hi == hi) {
        *found = true;
        if (!t->left || !t->right) {
            INode *child = t->left ? t->left : t->right;
            free(t);
            return child;
        }
        /* rotate the higher-priority child up, victim goes down one level */
        if (t->left->prio > t->right->prio) {
            t = rot_right(t);
            t->right = it_erase(t->right, lo, hi, found);
        } else {
            t = rot_left(t);
            t->left = it_erase(t->left, lo, hi, found);
        }
    } else if (less(lo, hi, t->lo, t->hi)) {
        t->left = it_erase(t->left, lo, hi, found);
    } else {
        t->right = it_erase(t->right, lo, hi, found);
    }
    pull(t);
    return t;
}

static bool overlaps(const INode *x, int a, int b) { return x->lo <= b && a <= x->hi; }

/*
 * Find ANY interval overlapping [a, b]: O(log n) — one root-to-leaf path.
 * Why going left is safe when left->maxhi >= a: some interval L on the left
 * has L.hi >= a. If L.lo <= b we found an overlap there. If L.lo > b then
 * every interval on the RIGHT has lo >= L.lo > b, so nothing there overlaps
 * either. Either way we never need to come back.
 */
static const INode *it_any(const INode *x, int a, int b)
{
    while (x && !overlaps(x, a, b))
        x = (x->left && x->left->maxhi >= a) ? x->left : x->right;
    return x;
}

/* Report ALL overlaps: O(k log n) for k results (prunes via maxhi and lo). */
static int it_all(const INode *x, int a, int b)
{
    if (!x || x->maxhi < a)          /* nothing in this subtree reaches a */
        return 0;
    int c = it_all(x->left, a, b);
    if (overlaps(x, a, b))
        c++;
    if (x->lo <= b)                  /* right subtree has lo >= x->lo; if x->lo > b, all too far */
        c += it_all(x->right, a, b);
    return c;
}

static void it_free(INode *x)
{
    if (!x) return;
    it_free(x->left);
    it_free(x->right);
    free(x);
}

/* Invariant check: BST order, heap order on prio, maxhi correct. Returns size. */
static int it_check(const INode *x)
{
    if (!x) return 0;
    int m = x->hi;
    if (x->left) {
        assert(!less(x->lo, x->hi, x->left->lo, x->left->hi) && x->left->prio <= x->prio);
        m = imax(m, x->left->maxhi);
    }
    if (x->right) {
        assert(!less(x->right->lo, x->right->hi, x->lo, x->hi) && x->right->prio <= x->prio);
        m = imax(m, x->right->maxhi);
    }
    assert(x->maxhi == m);
    return 1 + it_check(x->left) + it_check(x->right);
}

int main(void)
{
    INode *t = NULL;
    int demo[][2] = { { 15, 20 }, { 5, 8 }, { 17, 19 }, { 1, 10 }, { 25, 30 } };
    for (int i = 0; i < 5; i++)
        t = it_insert(t, demo[i][0], demo[i][1]);
    it_check(t);
    printf("root [%d,%d] maxhi=%d\n", t->lo, t->hi, t->maxhi);
    const INode *hit = it_any(t, 9, 12);
    printf("any overlap with [9,12]  -> [%d,%d]\n", hit->lo, hit->hi);
    assert(hit->lo == 1 && hit->hi == 10);
    printf("any overlap with [12,14] -> %s\n", it_any(t, 12, 14) ? "found" : "none");
    assert(!it_any(t, 12, 14));
    printf("count overlapping [16,26] = %d (expect 3)\n", it_all(t, 16, 26));
    assert(it_all(t, 16, 26) == 3);
    it_free(t);

    /* randomised test vs a brute-force list of intervals */
    enum { CAP = 2000, OPS = 20000, SPACE = 10000 };
    static int L[CAP][2];
    int n = 0;
    t = NULL;
    for (int op = 0; op < OPS; op++) {
        unsigned r = rnd() % 5; /* 2/5 insert, 1/5 erase, 2/5 query */
        if (r <= 1 && n < CAP) {
            int lo = (int)(rnd() % SPACE), len = (int)(rnd() % 200);
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (L[i][0] == lo && L[i][1] == lo + len) dup = true;
            if (!dup) {
                L[n][0] = lo;
                L[n][1] = lo + len;
                n++;
                t = it_insert(t, lo, lo + len);
            }
        } else if (r == 2 && n > 0) {
            int i = (int)(rnd() % (uint32_t)n);
            bool found = false;
            t = it_erase(t, L[i][0], L[i][1], &found);
            assert(found);
            L[i][0] = L[n - 1][0];
            L[i][1] = L[n - 1][1];
            n--;
        } else {
            int a = (int)(rnd() % SPACE), b = a + (int)(rnd() % 100);
            int want = 0;
            for (int i = 0; i < n; i++)          /* O(n) reference */
                if (L[i][0] <= b && a <= L[i][1]) want++;
            assert(it_all(t, a, b) == want);
            const INode *h = it_any(t, a, b);
            assert((h != NULL) == (want > 0));
            if (h) assert(overlaps(h, a, b));
        }
        if (op % 1000 == 0)
            assert(it_check(t) == n);
    }
    printf("random test: %d ops, final %d intervals, treap + maxhi invariants hold\n", OPS, it_check(t));
    it_free(t);
    printf("OK\n");
    return 0;
}
