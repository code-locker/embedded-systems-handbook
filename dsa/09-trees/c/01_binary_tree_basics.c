/*
 * 01_binary_tree_basics.c — node layout in memory, terminology, shape tests
 *
 * Concept : A binary tree node is just a key plus two pointers. From that one
 *           struct we compute size, height, depth, leaves and degree, and we
 *           classify shapes: full, complete, perfect, balanced, degenerate.
 *           Every classifier is checked on hand-built trees with known answers.
 * Build   : make run P=dsa/09-trees/c/01_binary_tree_basics.c
 * Read    : ../README.md  (sections "Mental model" and "Terminology")
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * The node. On a 64-bit machine this is 24 bytes:
 *
 *   offset 0   key    (int, 4 bytes) + 4 bytes padding so the pointers align
 *   offset 8   left   (8 bytes)  -> another Node or NULL
 *   offset 16  right  (8 bytes)  -> another Node or NULL
 *
 * Each node is its own malloc() block, so nodes are scattered over the heap.
 * Following a child pointer is a "pointer chase": a likely cache miss. That is
 * the main reason trees are slower in practice than their O(log n) suggests,
 * and why B-trees (lesson 12) pack many keys into one node.
 */
typedef struct Node {
    int key;
    struct Node *left;
    struct Node *right;
} Node;

/* Ownership: node_new mallocs, tree_free frees the whole subtree. */
static Node *node_new(int key)
{
    Node *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(1); }
    n->key = key;
    n->left = n->right = NULL;
    return n;
}

/* O(n) time, O(h) stack: post-order, because children must die before parent
 * (free the parent first and you lose the pointers to its children). */
static void tree_free(Node *t)
{
    if (!t) return;
    tree_free(t->left);
    tree_free(t->right);
    free(t);
}

/* ---- basic measurements: each is one post-order walk, O(n) -------------- */

static int tree_size(const Node *t)
{
    return t ? 1 + tree_size(t->left) + tree_size(t->right) : 0;
}

/* Height counted in EDGES: empty tree = -1, single node = 0.
 * (Some books count nodes, giving 0 / 1. Pick one and state it.) */
static int tree_height(const Node *t)
{
    if (!t) return -1;
    int hl = tree_height(t->left), hr = tree_height(t->right);
    return 1 + (hl > hr ? hl : hr);
}

static int count_leaves(const Node *t)
{
    if (!t) return 0;
    if (!t->left && !t->right) return 1;     /* leaf: degree 0 */
    return count_leaves(t->left) + count_leaves(t->right);
}

static int degree(const Node *t) { return (t->left != NULL) + (t->right != NULL); }

/* Depth of key = number of edges from the root down to it. O(n) worst case. */
static int depth_of(const Node *t, int key, int d)
{
    if (!t) return -1;
    if (t->key == key) return d;
    int l = depth_of(t->left, key, d + 1);
    return l >= 0 ? l : depth_of(t->right, key, d + 1);
}

/* ---- shape classifiers ---------------------------------------------------- */

/* FULL (a.k.a. proper/strict): every node has 0 or 2 children. O(n). */
static bool is_full(const Node *t)
{
    if (!t) return true;
    if (degree(t) == 1) return false;
    return is_full(t->left) && is_full(t->right);
}

/* PERFECT: full AND all leaves on the same level  <=>  size == 2^(h+1) - 1. */
static bool is_perfect(const Node *t)
{
    int h = tree_height(t);
    return tree_size(t) == (1 << (h + 1)) - 1;
}

/* COMPLETE: every level full except maybe the last, which fills left->right.
 * Level-order walk; after the first "hole" (NULL) no real node may follow.
 * This is exactly the shape a binary heap stores in an array (lesson 10). O(n). */
static bool is_complete(const Node *t)
{
    if (!t) return true;
    int n = tree_size(t);
    /* queue big enough for every node plus every NULL child: 2n + 1 */
    const Node **q = malloc((size_t)(2 * n + 1) * sizeof *q);
    if (!q) { perror("malloc"); exit(1); }
    int head = 0, tail = 0;
    bool seen_hole = false, ok = true;
    q[tail++] = t;
    while (head < tail) {
        const Node *x = q[head++];
        if (!x) { seen_hole = true; continue; }
        if (seen_hole) { ok = false; break; }   /* a node after a hole */
        q[tail++] = x->left;
        q[tail++] = x->right;
    }
    free(q);
    return ok;
}

/* HEIGHT-BALANCED (AVL definition): at every node |h(L) - h(R)| <= 1.
 * Returns the height, or -2 as "already unbalanced" so we stay O(n) instead
 * of the naive O(n log n)..O(n^2) "call height() at every node". */
static int balanced_height(const Node *t)
{
    if (!t) return -1;
    int hl = balanced_height(t->left);
    if (hl == -2) return -2;
    int hr = balanced_height(t->right);
    if (hr == -2) return -2;
    if (hl - hr > 1 || hr - hl > 1) return -2;
    return 1 + (hl > hr ? hl : hr);
}
static bool is_balanced(const Node *t) { return balanced_height(t) != -2; }

/* DEGENERATE: every node has at most one child -> it is a linked list,
 * height n-1, and every O(h) operation becomes O(n). */
static bool is_degenerate(const Node *t)
{
    for (; t; t = t->left ? t->left : t->right)
        if (degree(t) == 2) return false;
    return true;
}

/* ---- printing: sideways tree (root on the left, right subtree on top) ----- */
static void print_sideways(const Node *t, int indent)
{
    if (!t) return;
    print_sideways(t->right, indent + 6);
    printf("%*s%d\n", indent, "", t->key);
    print_sideways(t->left, indent + 6);
}

/* ---- builders for test shapes -------------------------------------------- */

/* Perfect tree of height h with keys 1..2^(h+1)-1 in heap order (i, 2i, 2i+1). */
static Node *build_heap_shape(int i, int n)
{
    if (i > n) return NULL;
    Node *x = node_new(i);
    x->left = build_heap_shape(2 * i, n);
    x->right = build_heap_shape(2 * i + 1, n);
    return x;
}

/* Chain 1 -> 2 -> ... -> n (all right children): the degenerate tree you get
 * by inserting sorted keys into a plain BST. */
static Node *build_chain(int n)
{
    Node *root = NULL, **link = &root;
    for (int i = 1; i <= n; i++) { *link = node_new(i); link = &(*link)->right; }
    return root;
}

static void report(const char *name, const Node *t)
{
    printf("%-22s size=%2d height=%2d leaves=%2d  full=%d complete=%d "
           "perfect=%d balanced=%d degenerate=%d\n",
           name, tree_size(t), tree_height(t), count_leaves(t), is_full(t),
           is_complete(t), is_perfect(t), is_balanced(t), is_degenerate(t));
}

int main(void)
{
    printf("sizeof(Node)=%zu  offsetof key=%zu left=%zu right=%zu\n\n",
           sizeof(Node), offsetof(Node, key), offsetof(Node, left),
           offsetof(Node, right));

    /*
     * Hand-built example used throughout the README:
     *
     *              1            depth 0   <- root
     *            /   \
     *           2     3         depth 1
     *          / \     \
     *         4   5     6       depth 2
     *            /
     *           7               depth 3   <- deepest leaf, height = 3
     */
    Node *t = node_new(1);
    t->left = node_new(2);
    t->right = node_new(3);
    t->left->left = node_new(4);
    t->left->right = node_new(5);
    t->right->right = node_new(6);
    t->left->right->left = node_new(7);

    printf("sideways (turn your head left):\n");
    print_sideways(t, 0);
    putchar('\n');

    assert(tree_size(t) == 7);
    assert(tree_height(t) == 3);
    assert(count_leaves(t) == 3);              /* 4, 7, 6 */
    assert(depth_of(t, 1, 0) == 0);
    assert(depth_of(t, 5, 0) == 2);
    assert(depth_of(t, 7, 0) == 3);
    assert(depth_of(t, 99, 0) == -1);
    assert(degree(t) == 2 && degree(t->right) == 1 && degree(t->left->left) == 0);
    assert(!is_full(t));                       /* 3 and 5 have one child   */
    assert(!is_complete(t));                   /* hole at 3's left         */
    assert(!is_perfect(t));
    /* Balanced? Check every node's |h(L) - h(R)| (height of empty = -1):
     *   node 1: L{2,4,5,7} h=2, R{3,6} h=1   -> diff 1 ok
     *   node 2: L{4} h=0,       R{5,7} h=1   -> diff 1 ok
     *   node 3: L{} h=-1,       R{6} h=0     -> diff 1 ok
     *   node 5: L{7} h=0,       R{} h=-1     -> diff 1 ok
     * So it IS height-balanced even though it looks lopsided. Add one more
     * node under 7 and node 5 becomes L h=1, R h=-1 -> diff 2 -> unbalanced. */
    assert(is_balanced(t));
    report("example", t);
    t->left->right->left->left = node_new(8);
    assert(!is_balanced(t) && tree_height(t) == 4);
    report("example + 8 under 7", t);
    tree_free(t);

    /* Perfect tree, height 2: 7 nodes */
    Node *p = build_heap_shape(1, 7);
    report("perfect h=2", p);
    assert(is_full(p) && is_complete(p) && is_perfect(p) && is_balanced(p));
    assert(!is_degenerate(p) && count_leaves(p) == 4);
    tree_free(p);

    /* Complete but not perfect: 6 nodes, last level filled from the left */
    Node *c = build_heap_shape(1, 6);
    report("complete n=6", c);
    assert(is_complete(c) && !is_perfect(c) && !is_full(c) && is_balanced(c));
    tree_free(c);

    /* Full but not complete:      1
     *                            / \
     *                           2   3
     *                              / \
     *                             4   5          */
    Node *f = node_new(1);
    f->left = node_new(2);
    f->right = node_new(3);
    f->right->left = node_new(4);
    f->right->right = node_new(5);
    report("full, not complete", f);
    assert(is_full(f) && !is_complete(f) && !is_perfect(f) && is_balanced(f));
    tree_free(f);

    /* Degenerate chain: height n-1 */
    Node *d = build_chain(6);
    report("degenerate chain n=6", d);
    assert(is_degenerate(d) && tree_height(d) == 5 && !is_balanced(d));
    assert(!is_complete(d) && count_leaves(d) == 1);
    tree_free(d);

    /* Property sweep: every heap-shaped tree with n nodes is complete and
     * balanced, and has height floor(log2 n). */
    for (int n = 1; n <= 200; n++) {
        Node *h = build_heap_shape(1, n);
        int lg = 0;
        while ((2 << lg) <= n) lg++;          /* floor(log2 n) */
        assert(is_complete(h) && is_balanced(h));
        assert(tree_height(h) == lg && tree_size(h) == n);
        assert(is_perfect(h) == (((n + 1) & n) == 0));  /* n = 2^k - 1 */
        assert(count_leaves(h) == n - n / 2);           /* ceil(n/2) leaves */
        tree_free(h);
    }
    printf("\nall basics checks passed\n");
    return 0;
}
