/*
 * 06_avl.c — AVL tree: insert + delete with LL, RR, LR, RL rotations
 *
 * Concept : An AVL tree is a BST that repairs itself: every node stores its
 *           height, and after each insert/delete, on the way back up, any node
 *           whose children's heights differ by 2 is fixed by one or two
 *           rotations. That keeps height <= 1.44 log2(n), so every operation is
 *           O(log n) even for sorted input. The invariants (BST order, stored
 *           heights, |balance| <= 1) are checked after EVERY operation.
 * Build   : make run P=dsa/09-trees/c/06_avl.c
 * Read    : ../README.md  (section "AVL tree")
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int key;
    int height;              /* height in nodes: leaf = 1, empty = 0 */
    struct Node *left, *right;
} Node;

static int rot_ll, rot_rr, rot_lr, rot_rl;     /* statistics for the demo */

static int h(const Node *t) { return t ? t->height : 0; }
static int max2(int a, int b) { return a > b ? a : b; }
static void update(Node *t) { t->height = 1 + max2(h(t->left), h(t->right)); }
static int balance(const Node *t) { return h(t->left) - h(t->right); }  /* +: left-heavy */

static Node *node_new(int key)
{
    Node *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(1); }
    n->key = key;
    n->height = 1;
    n->left = n->right = NULL;
    return n;
}

static void tree_free(Node *t)
{
    if (!t) return;
    tree_free(t->left);
    tree_free(t->right);
    free(t);
}

/* ---- the two primitive rotations: O(1), 3 pointer moves -----------------
 *
 * rotate_right(y): y's left child x becomes the subtree root.
 *
 *           y                    x
 *          / \                  / \
 *         x   C     ---->      A   y
 *        / \                      / \
 *       A   B                    B   C
 *
 *   In-order before: A x B y C.  After: A x B y C.  Same order -> still a BST.
 *   Only B changes parent (from x to y). Heights of y then x must be updated
 *   (y first: it is now below x).
 */
static Node *rotate_right(Node *y)
{
    Node *x = y->left;
    Node *B = x->right;
    x->right = y;
    y->left = B;
    update(y);
    update(x);
    return x;
}

/* rotate_left(x): the mirror image.
 *
 *         x                        y
 *        / \                      / \
 *       A   y       ---->        x   C
 *          / \                  / \
 *         B   C                A   B
 */
static Node *rotate_left(Node *x)
{
    Node *y = x->right;
    Node *B = y->left;
    y->left = x;
    x->right = B;
    update(x);
    update(y);
    return y;
}

/* ---- rebalance: the four cases --------------------------------------------
 * Called on every node along the path back up after insert/delete. O(1).
 *
 * LL (left child is left-heavy or even) -> one right rotation
 *
 *            z (+2)                y
 *           /                    /   \
 *          y (+1)     ---->     x     z
 *         /
 *        x
 *
 * RR (mirror) -> one left rotation
 *
 *        z (-2)                    y
 *         \                      /   \
 *          y (-1)     ---->     z     x
 *           \
 *            x
 *
 * LR (left child is RIGHT-heavy: a zig-zag) -> left-rotate the child,
 *    which turns it into LL, then right-rotate z.
 *
 *          z (+2)            z               x
 *         /                 /              /   \
 *        y (-1)   ---->    x      ---->   y     z
 *         \               /
 *          x             y
 *     rotate_left(y)          rotate_right(z)
 *
 * RL (mirror) -> right-rotate the child, then left-rotate z.
 *
 *        z (-2)          z                    x
 *         \               \                 /   \
 *          y (+1) ---->    x      ---->    z     y
 *         /                 \
 *        x                   y
 *     rotate_right(y)          rotate_left(z)
 *
 * Why a single rotation fails for LR: rotating z right would move x's
 * subtree under z and leave the tree just as unbalanced the other way.
 * The zig-zag must be straightened into a line first.
 */
static Node *rebalance(Node *z)
{
    update(z);
    int b = balance(z);
    if (b > 1) {                               /* left side too tall */
        if (balance(z->left) < 0) {            /* LR: zig-zag */
            z->left = rotate_left(z->left);
            rot_lr++;
        } else {
            rot_ll++;                          /* LL. (balance == 0 only
                                                  happens after delete) */
        }
        return rotate_right(z);
    }
    if (b < -1) {                              /* right side too tall */
        if (balance(z->right) > 0) {           /* RL */
            z->right = rotate_right(z->right);
            rot_rl++;
        } else {
            rot_rr++;
        }
        return rotate_left(z);
    }
    return z;                                  /* already fine */
}

/* ---- insert: O(log n). Recursive so the "way back up" is free. ---------- */
static Node *avl_insert(Node *t, int key, bool *inserted)
{
    if (!t) { *inserted = true; return node_new(key); }
    if (key < t->key)      t->left = avl_insert(t->left, key, inserted);
    else if (key > t->key) t->right = avl_insert(t->right, key, inserted);
    else { *inserted = false; return t; }      /* duplicate: no change */
    return rebalance(t);                       /* at most ONE rebalance actually
                                                  rotates on insert */
}

/* Remove the minimum of subtree t; return new subtree root. Used by delete. */
static Node *remove_min(Node *t, Node **out_min)
{
    if (!t->left) { *out_min = t; return t->right; }
    t->left = remove_min(t->left, out_min);
    return rebalance(t);
}

/* ---- delete: O(log n). Same three BST cases, then rebalance upward.
 * Unlike insert, delete may need a rotation at EVERY level on the way up
 * (O(log n) rotations worst case) because a rotation can shorten a subtree. */
static Node *avl_delete(Node *t, int key, bool *deleted)
{
    if (!t) { *deleted = false; return NULL; }
    if (key < t->key)      t->left = avl_delete(t->left, key, deleted);
    else if (key > t->key) t->right = avl_delete(t->right, key, deleted);
    else {
        *deleted = true;
        Node *l = t->left, *r = t->right;
        free(t);
        if (!l) return r;                      /* case 1 / 2 */
        if (!r) return l;
        /* case 3: relink the successor node into this position */
        Node *succ;
        r = remove_min(r, &succ);
        succ->left = l;
        succ->right = r;
        return rebalance(succ);
    }
    return rebalance(t);
}

static bool avl_contains(const Node *t, int key)       /* O(log n) */
{
    while (t && t->key != key) t = key < t->key ? t->left : t->right;
    return t != NULL;
}

/* ---- invariant checker: returns real height, asserts everything ------- */
static int check(const Node *t, long long lo, long long hi, int *count)
{
    if (!t) return 0;
    assert(t->key > lo && t->key < hi);                    /* BST order */
    int hl = check(t->left, lo, t->key, count);
    int hr = check(t->right, t->key, hi, count);
    assert(t->height == 1 + max2(hl, hr));                 /* stored height */
    assert(hl - hr <= 1 && hr - hl <= 1);                  /* AVL balance */
    (*count)++;
    return t->height;
}

static void print_sideways(const Node *t, int indent)
{
    if (!t) return;
    print_sideways(t->right, indent + 5);
    printf("%*s%d\n", indent, "", t->key);
    print_sideways(t->left, indent + 5);
}

#define U 3000

int main(void)
{
    bool flag;
    Node *t = NULL;

    /* Demonstrate each rotation in isolation. */
    struct { const char *name; int keys[3]; int *counter; } demo[] = {
        { "LL (3,2,1)", { 3, 2, 1 }, &rot_ll },
        { "RR (1,2,3)", { 1, 2, 3 }, &rot_rr },
        { "LR (3,1,2)", { 3, 1, 2 }, &rot_lr },
        { "RL (1,3,2)", { 1, 3, 2 }, &rot_rl },
    };
    for (size_t d = 0; d < 4; d++) {
        int before = *demo[d].counter;
        t = NULL;
        for (int i = 0; i < 3; i++) t = avl_insert(t, demo[d].keys[i], &flag);
        printf("%s -> root %d, children %d and %d\n", demo[d].name, t->key,
               t->left->key, t->right->key);
        assert(t->key == 2 && t->left->key == 1 && t->right->key == 3);
        assert(*demo[d].counter == before + 1);
        tree_free(t);
    }

    /* Sorted input: a plain BST would have height 1023; AVL stays ~log2. */
    t = NULL;
    for (int i = 1; i <= 1023; i++) t = avl_insert(t, i, &flag);
    int cnt = 0;
    int ht = check(t, LLONG_MIN, LLONG_MAX, &cnt);
    printf("1023 sorted inserts -> height %d nodes (perfectly balanced = 10)\n", ht);
    assert(cnt == 1023 && ht == 10);
    tree_free(t);

    t = NULL;
    for (int i = 1; i <= 15; i++) t = avl_insert(t, i, &flag);
    printf("\nAVL after inserting 1..15 (sideways):\n");
    print_sideways(t, 0);
    tree_free(t);

    /* Randomised: every operation compared with a presence array and every
     * invariant checked after every single operation. */
    static bool present[U];
    int size = 0;
    rot_ll = rot_rr = rot_lr = rot_rl = 0;
    t = NULL;
    srand(1);
    for (int op = 0; op < 60000; op++) {
        int k = rand() % U;
        if (rand() % 2) {
            t = avl_insert(t, k, &flag);
            assert(flag == !present[k]);
            if (flag) { present[k] = true; size++; }
        } else {
            t = avl_delete(t, k, &flag);
            assert(flag == present[k]);
            if (flag) { present[k] = false; size--; }
        }
        int q = rand() % U;
        assert(avl_contains(t, q) == present[q]);
        cnt = 0;
        int height = check(t, LLONG_MIN, LLONG_MAX, &cnt);
        assert(cnt == size);
        /* AVL bound: n >= Fib(h+2) - 1  =>  h <= 1.4405 log2(n+2) */
        if (size > 0) {
            int lg = 0;
            while ((1 << lg) < size + 2) lg++;
            assert(height <= (int)(1.4405 * lg) + 1);
        }
    }
    printf("\nrandom ops: size=%d height=%d  rotations LL=%d RR=%d LR=%d RL=%d\n",
           size, h(t), rot_ll, rot_rr, rot_lr, rot_rl);
    tree_free(t);
    printf("all AVL checks passed (invariants verified after 60000 ops)\n");
    return 0;
}
