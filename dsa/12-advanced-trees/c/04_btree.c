/*
 * 04_btree.c — B-tree of minimum degree T: search, insert (split on the way down), delete
 *
 * Concept : A B-tree node holds many sorted keys (here up to 2T-1) and one
 *           more child pointer than keys. Fat nodes => very short trees, so a
 *           lookup touches only a handful of nodes (= disk blocks). Insert
 *           splits full nodes on the way down, so the tree only ever grows
 *           at the ROOT and every leaf stays at the same depth.
 * Build   : make run P=dsa/12-advanced-trees/c/04_btree.c
 * Read    : ../README.md  (section "B-tree")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimum degree T (CLRS notation):
 *   every node except the root has  T-1 .. 2T-1 keys
 *   an internal node with k keys has k+1 children
 * T = 3 -> 2..5 keys per node. Real databases use T in the hundreds so
 * one node fills one 4 KiB / 16 KiB page.
 */
#define T 3
#define MAXK (2 * T - 1)

typedef struct BNode {
    int n;                       /* number of keys in use               */
    bool leaf;
    int key[MAXK];               /* key[0] < key[1] < ... < key[n-1]    */
    struct BNode *child[MAXK + 1];
} BNode;

/*
 * Memory picture of one node (T=3), keys 10 20 30:
 *
 *   +----+----+----+----+----+
 *   | 10 | 20 | 30 |    |    |   key[]   n = 3
 *   +----+----+----+----+----+----+
 *   | c0 | c1 | c2 | c3 |    |    |   child[] (n+1 used)
 *   +----+----+----+----+----+----+
 *    <10  10..20 20..30 >30
 */

static BNode *node_new(bool leaf)
{
    BNode *x = calloc(1, sizeof *x); /* caller (the tree) owns it; freed in btree_free */
    assert(x);
    x->leaf = leaf;
    return x;
}

static void btree_free(BNode *x)
{
    if (!x)
        return;
    if (!x->leaf)
        for (int i = 0; i <= x->n; i++)
            btree_free(x->child[i]);
    free(x);
}

/*
 * Search: O(T log_T n) comparisons, but only O(log_T n) NODE visits —
 * and node visits are what cost a disk read.
 */
static bool btree_search(const BNode *x, int k)
{
    while (x) {
        int i = 0;
        while (i < x->n && k > x->key[i]) /* linear scan; binary search for big T */
            i++;
        if (i < x->n && k == x->key[i])
            return true;
        if (x->leaf)
            return false;
        x = x->child[i];
    }
    return false;
}

/*
 * split_child: parent x is NOT full, its child y = x->child[i] IS full (2T-1 keys).
 * Move y's median up into x, and y's upper half into a new node z.  O(T)
 *
 *   T=3, y full:             x: [ .. 40 .. ]          x: [ .. 15 40 .. ]
 *                                  /                        /    \
 *        y: [5 10 15 20 25]   y: [5 10 15 20 25]   y: [5 10]   z: [20 25]
 *                                   ^ median
 */
static void split_child(BNode *x, int i)
{
    BNode *y = x->child[i];
    BNode *z = node_new(y->leaf);
    z->n = T - 1;
    memcpy(z->key, &y->key[T], (T - 1) * sizeof(int));          /* upper T-1 keys    */
    if (!y->leaf)
        memcpy(z->child, &y->child[T], T * sizeof(BNode *));    /* upper T children  */
    y->n = T - 1;

    /* open a gap in x at position i for the median and at i+1 for z */
    memmove(&x->child[i + 2], &x->child[i + 1], (size_t)(x->n - i) * sizeof(BNode *));
    memmove(&x->key[i + 1], &x->key[i], (size_t)(x->n - i) * sizeof(int));
    x->child[i + 1] = z;
    x->key[i] = y->key[T - 1];                                  /* median goes up    */
    x->n++;
}

/* x is guaranteed not full, so a split below always has room for the median. */
static void insert_nonfull(BNode *x, int k)
{
    int i = x->n - 1;
    if (x->leaf) {
        while (i >= 0 && k < x->key[i]) { /* shift bigger keys right: insertion sort step */
            x->key[i + 1] = x->key[i];
            i--;
        }
        x->key[i + 1] = k;
        x->n++;
        return;
    }
    while (i >= 0 && k < x->key[i])
        i--;
    i++;                                  /* child[i] is where k belongs */
    if (x->child[i]->n == MAXK) {
        split_child(x, i);                /* pre-emptive split: never need to back up */
        if (k > x->key[i])
            i++;                          /* the median moved up; pick the correct half */
    }
    insert_nonfull(x->child[i], k);
}

/*
 * Insert: O(T log_T n). If the root is full we split it first: a new root
 * appears ABOVE the old one. That is the only way the height ever grows,
 * which is why all leaves stay at the same depth.
 */
static BNode *btree_insert(BNode *root, int k)
{
    if (!root) {
        root = node_new(true);
        root->key[0] = k;
        root->n = 1;
        return root;
    }
    if (root->n == MAXK) {
        BNode *s = node_new(false);
        s->child[0] = root;
        split_child(s, 0);
        root = s;
    }
    insert_nonfull(root, k);
    return root;
}

/* ===================== deletion (CLRS, top-down) ===================== */
/*
 * Rule: before descending into a child, make sure it has at least T keys
 * (one more than the minimum) so that deleting from it can never underflow.
 * Three fix-ups: borrow from left sibling, borrow from right sibling, merge.
 */

static int max_key(BNode *x) { while (!x->leaf) x = x->child[x->n]; return x->key[x->n - 1]; }
static int min_key(BNode *x) { while (!x->leaf) x = x->child[0]; return x->key[0]; }

/*
 * merge child[i], key[i], child[i+1] into one node (both children have T-1 keys
 * -> result has 2T-1 keys). O(T)
 *
 *    x: [ .. 30 .. ]                  x: [ .. .. ]
 *         /    \          ---->            |
 *     [10 20]  [40 50]               [10 20 30 40 50]
 */
static void merge(BNode *x, int i)
{
    BNode *a = x->child[i], *b = x->child[i + 1];
    a->key[a->n] = x->key[i];
    memcpy(&a->key[a->n + 1], b->key, (size_t)b->n * sizeof(int));
    if (!a->leaf)
        memcpy(&a->child[a->n + 1], b->child, (size_t)(b->n + 1) * sizeof(BNode *));
    a->n += b->n + 1;
    memmove(&x->key[i], &x->key[i + 1], (size_t)(x->n - i - 1) * sizeof(int));
    memmove(&x->child[i + 1], &x->child[i + 2], (size_t)(x->n - i - 1) * sizeof(BNode *));
    x->n--;
    free(b); /* b's keys and children now live in a */
}

/*
 * Ensure x->child[i] has >= T keys. Returns the index of the child to descend
 * into (it can change to i-1 after a merge with the left sibling).
 *
 * borrow from left (rotate right through the parent):
 *     x: [ .. 20 .. ]                 x: [ .. 15 .. ]
 *        /       \        ---->          /       \
 *  [5 10 15]   [30 40]             [5 10]    [20 30 40]
 */
static int fill(BNode *x, int i)
{
    BNode *c = x->child[i];
    if (i > 0 && x->child[i - 1]->n >= T) {             /* borrow from LEFT sibling */
        BNode *l = x->child[i - 1];
        memmove(&c->key[1], c->key, (size_t)c->n * sizeof(int));
        if (!c->leaf)
            memmove(&c->child[1], c->child, (size_t)(c->n + 1) * sizeof(BNode *));
        c->key[0] = x->key[i - 1];
        if (!c->leaf)
            c->child[0] = l->child[l->n];
        x->key[i - 1] = l->key[l->n - 1];
        l->n--;
        c->n++;
        return i;
    }
    if (i < x->n && x->child[i + 1]->n >= T) {          /* borrow from RIGHT sibling */
        BNode *r = x->child[i + 1];
        c->key[c->n] = x->key[i];
        if (!c->leaf)
            c->child[c->n + 1] = r->child[0];
        x->key[i] = r->key[0];
        memmove(r->key, &r->key[1], (size_t)(r->n - 1) * sizeof(int));
        if (!r->leaf)
            memmove(r->child, &r->child[1], (size_t)r->n * sizeof(BNode *));
        r->n--;
        c->n++;
        return i;
    }
    if (i < x->n) {                                     /* both siblings minimal: MERGE */
        merge(x, i);
        return i;
    }
    merge(x, i - 1);
    return i - 1;
}

/* O(T log_T n). Returns true if k was found and removed. */
static bool delete_from(BNode *x, int k)
{
    int i = 0;
    while (i < x->n && k > x->key[i])
        i++;
    if (i < x->n && x->key[i] == k) {
        if (x->leaf) {                                   /* case 1: key in a leaf */
            memmove(&x->key[i], &x->key[i + 1], (size_t)(x->n - i - 1) * sizeof(int));
            x->n--;
            return true;
        }
        /* case 2: key in an internal node -> replace with predecessor/successor */
        if (x->child[i]->n >= T) {                       /* 2a */
            int pred = max_key(x->child[i]);
            x->key[i] = pred;
            return delete_from(x->child[i], pred);
        }
        if (x->child[i + 1]->n >= T) {                   /* 2b */
            int succ = min_key(x->child[i + 1]);
            x->key[i] = succ;
            return delete_from(x->child[i + 1], succ);
        }
        merge(x, i);                                     /* 2c: k moves down into merged child */
        return delete_from(x->child[i], k);
    }
    if (x->leaf)
        return false;                                    /* not in tree */
    /* case 3: key is below child[i]; top it up first so it can lose a key */
    if (x->child[i]->n < T)
        i = fill(x, i);
    return delete_from(x->child[i], k);
}

static BNode *btree_delete(BNode *root, int k, bool *found)
{
    *found = root ? delete_from(root, k) : false;
    if (root && root->n == 0) {           /* root emptied: height SHRINKS at the top */
        BNode *old = root;
        root = root->leaf ? NULL : root->child[0];
        free(old);
    }
    return root;
}

/* ===================== checking / printing ===================== */

/* Verify every B-tree invariant; returns leaf depth, appends keys to out[]. */
static int check(const BNode *x, bool is_root, int depth, int *out, int *cnt, long lo, long hi)
{
    assert(x->n <= MAXK);
    assert(is_root ? x->n >= 1 : x->n >= T - 1);
    for (int i = 0; i < x->n; i++) {
        assert(x->key[i] > lo && x->key[i] < hi);
        if (i) assert(x->key[i - 1] < x->key[i]);
    }
    if (x->leaf) {
        for (int i = 0; i < x->n; i++) out[(*cnt)++] = x->key[i];
        return depth;
    }
    int d = -1;
    for (int i = 0; i <= x->n; i++) {
        long clo = i == 0 ? lo : x->key[i - 1];
        long chi = i == x->n ? hi : x->key[i];
        int cd = check(x->child[i], false, depth + 1, out, cnt, clo, chi);
        assert(d == -1 || d == cd); /* all leaves at the same depth */
        d = cd;
        if (i < x->n) out[(*cnt)++] = x->key[i];
    }
    return d;
}

static void print_tree(const BNode *x, int indent)
{
    printf("%*s[", indent, "");
    for (int i = 0; i < x->n; i++)
        printf(i ? " %d" : "%d", x->key[i]);
    printf("]\n");
    if (!x->leaf)
        for (int i = 0; i <= x->n; i++)
            print_tree(x->child[i], indent + 4);
}

static uint32_t rng_state = 7u;
static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    BNode *root = NULL;
    int demo[] = { 10, 20, 5, 6, 12, 30, 7, 17, 3, 4, 2, 1, 25, 26, 27 };
    for (size_t i = 0; i < sizeof demo / sizeof demo[0]; i++)
        root = btree_insert(root, demo[i]);
    printf("B-tree (T=%d, 2..5 keys/node) after inserting 15 keys:\n", T);
    print_tree(root, 2);
    assert(btree_search(root, 17) && !btree_search(root, 18));
    bool found;
    root = btree_delete(root, 6, &found);
    assert(found);
    root = btree_delete(root, 12, &found);
    printf("after deleting 6 and 12:\n");
    print_tree(root, 2);
    btree_free(root);

    /* randomised check against a presence table (keys 0..RANGE-1) */
    enum { RANGE = 3000, OPS = 40000 };
    static bool present[RANGE];
    static int out[RANGE];
    root = NULL;
    int size = 0;
    for (int op = 0; op < OPS; op++) {
        int k = (int)(rnd() % RANGE);
        if (rnd() % 3) { /* insert-biased so the tree gets tall */
            if (!present[k]) {
                root = btree_insert(root, k);
                present[k] = true;
                size++;
            }
        } else {
            root = btree_delete(root, k, &found);
            assert(found == present[k]);
            if (found) { present[k] = false; size--; }
        }
        assert(btree_search(root, k) == present[k]);
        if (op % 500 == 0 && root) {  /* full invariant check: O(n) */
            int cnt = 0;
            check(root, true, 0, out, &cnt, -1, RANGE);
            assert(cnt == size);
            int j = 0;
            for (int v = 0; v < RANGE; v++)
                if (present[v]) assert(out[j++] == v); /* in-order == sorted reference */
        }
    }
    int cnt = 0, h = root ? check(root, true, 0, out, &cnt, -1, RANGE) : 0;
    printf("random test: %d insert/delete/search ops, final size %d, height %d, invariants hold\n",
           OPS, size, h + 1);
    btree_free(root);
    printf("OK\n");
    return 0;
}
