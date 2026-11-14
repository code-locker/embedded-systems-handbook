/*
 * 03_height_diameter_lca.c — height, diameter, lowest common ancestor
 *
 * Concept : "Ask your children, then combine" (post-order) solves most tree
 *           problems in ONE O(n) pass. Height: 1 + max(child heights).
 *           Diameter: the longest path bends at some node, so it is
 *           max over nodes of (hL + hR + 2). LCA: the first node whose two
 *           subtrees each report one of the targets. Each fast version is
 *           checked against a slow, obviously-correct reference.
 * Build   : make run P=dsa/09-trees/c/03_height_diameter_lca.c
 * Read    : ../README.md  (section "Height, diameter, LCA")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct Node {
    int key;
    struct Node *left, *right;
} Node;

static Node *node_new(int key)
{
    Node *n = malloc(sizeof *n);
    if (!n) { perror("malloc"); exit(1); }
    n->key = key;
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

static int max2(int a, int b) { return a > b ? a : b; }

/* Height in edges (empty = -1). O(n). */
static int height(const Node *t)
{
    return t ? 1 + max2(height(t->left), height(t->right)) : -1;
}

/* ---- diameter --------------------------------------------------------------
 * Diameter = number of EDGES on the longest path between any two nodes.
 * The path goes up from one end, bends at a single highest node X, and goes
 * down to the other end:
 *
 *              X            path length through X
 *            /   \            = (hL + 1) + (hR + 1)
 *          ...   ...          = hL + hR + 2
 *          /       \
 *        end1      end2     (with h(empty) = -1 this also works when one
 *                            side is empty: hL + (-1) + 2 = hL + 1)
 *
 * Note the path need NOT pass through the root.
 */

/* Slow reference: O(n^2) - recompute heights at every node. */
static int diameter_slow(const Node *t)
{
    if (!t) return 0;
    int through = height(t->left) + height(t->right) + 2;
    return max2(through, max2(diameter_slow(t->left), diameter_slow(t->right)));
}

/* Fast: O(n) - one post-order pass returns height and updates best. */
static int diam_rec(const Node *t, int *best)
{
    if (!t) return -1;
    int hl = diam_rec(t->left, best);
    int hr = diam_rec(t->right, best);
    *best = max2(*best, hl + hr + 2);
    return 1 + max2(hl, hr);
}
static int diameter_fast(const Node *t)
{
    int best = 0;
    diam_rec(t, &best);
    return best;
}

/* ---- LCA in a plain binary tree (not a BST) --------------------------------
 * lca(t, a, b) returns:
 *   - the LCA if both a and b are below t,
 *   - a or b itself if only one is below t,
 *   - NULL if neither is.
 * At the first node where the left call AND the right call both come back
 * non-NULL, a is on one side and b on the other: that node is the LCA.
 *
 *              3          lca(5, 1) = 3   (split at 3)
 *            /   \        lca(6, 4) = 5   (both under 5)
 *           5     1       lca(5, 4) = 5   (a node is its own ancestor)
 *          / \   / \
 *         6   2 0   8
 *            / \
 *           7   4
 *
 * O(n) time, O(h) stack. Assumes both keys exist (checked by the caller). */
static const Node *lca(const Node *t, int a, int b)
{
    if (!t || t->key == a || t->key == b) return t;
    const Node *l = lca(t->left, a, b);
    const Node *r = lca(t->right, a, b);
    if (l && r) return t;          /* split here */
    return l ? l : r;              /* pass up whichever side found something */
}

/* Reference LCA: record root->a path and root->b path, last common node.
 * O(n) per query but needs O(h) arrays - very easy to believe. */
static int path_to(const Node *t, int key, const Node **path, int depth)
{
    if (!t) return -1;
    path[depth] = t;
    if (t->key == key) return depth;
    int d = path_to(t->left, key, path, depth + 1);
    if (d >= 0) return d;
    return path_to(t->right, key, path, depth + 1);
}

#define MAXN 400

static const Node *lca_ref(const Node *t, int a, int b)
{
    const Node *pa[MAXN], *pb[MAXN];
    int da = path_to(t, a, pa, 0), db = path_to(t, b, pb, 0);
    assert(da >= 0 && db >= 0);
    const Node *ans = NULL;
    for (int i = 0; i <= da && i <= db && pa[i] == pb[i]; i++) ans = pa[i];
    return ans;
}

/* LCA in a BST is easier: walk down; split point is where a and b go to
 * different sides. O(h), no recursion, no "does it exist" ambiguity. */
static const Node *bst_lca(const Node *t, int a, int b)
{
    while (t) {
        if (a < t->key && b < t->key) t = t->left;
        else if (a > t->key && b > t->key) t = t->right;
        else return t;             /* split (or hit one of them) */
    }
    return NULL;
}

static Node *bst_insert(Node *t, int key)
{
    if (!t) return node_new(key);
    if (key < t->key) t->left = bst_insert(t->left, key);
    else if (key > t->key) t->right = bst_insert(t->right, key);
    return t;
}

/* Random shape with keys 0..n-1 (see 02_traversals.c). */
static Node *random_tree(int n)
{
    Node *root = NULL;
    for (int k = 0; k < n; k++) {
        Node **link = &root;
        while (*link) link = (rand() & 1) ? &(*link)->left : &(*link)->right;
        *link = node_new(k);
    }
    return root;
}

int main(void)
{
    /* Build the LCA picture above. */
    Node *t = node_new(3);
    t->left = node_new(5);
    t->right = node_new(1);
    t->left->left = node_new(6);
    t->left->right = node_new(2);
    t->left->right->left = node_new(7);
    t->left->right->right = node_new(4);
    t->right->left = node_new(0);
    t->right->right = node_new(8);

    printf("height = %d, diameter = %d\n", height(t), diameter_fast(t));
    assert(height(t) == 3);
    assert(diameter_fast(t) == 5);          /* 7-2-5-3-1-8 : 5 edges */
    assert(lca(t, 5, 1)->key == 3);
    assert(lca(t, 6, 4)->key == 5);
    assert(lca(t, 5, 4)->key == 5);
    assert(lca(t, 7, 8)->key == 3);
    printf("lca(6,4)=%d  lca(5,1)=%d  lca(7,8)=%d\n",
           lca(t, 6, 4)->key, lca(t, 5, 1)->key, lca(t, 7, 8)->key);
    tree_free(t);

    /* Diameter not through the root:
     *        1
     *       /
     *      2
     *     / \
     *    3   4
     *   /     \
     *  5       6         longest path 5-3-2-4-6 = 4 edges, root not on it */
    t = node_new(1);
    t->left = node_new(2);
    t->left->left = node_new(3);
    t->left->right = node_new(4);
    t->left->left->left = node_new(5);
    t->left->right->right = node_new(6);
    assert(diameter_fast(t) == 4 && diameter_slow(t) == 4);
    printf("diameter avoiding the root = %d\n", diameter_fast(t));
    tree_free(t);

    /* Randomised: fast == slow, lca == reference. */
    srand(2024);
    for (int iter = 0; iter < 500; iter++) {
        int n = 1 + rand() % (MAXN - 1);
        Node *r = random_tree(n);
        assert(diameter_fast(r) == diameter_slow(r));
        for (int q = 0; q < 50; q++) {
            int a = rand() % n, b = rand() % n;
            assert(lca(r, a, b) == lca_ref(r, a, b));
        }
        tree_free(r);
    }

    /* BST LCA vs generic LCA on random BSTs. */
    for (int iter = 0; iter < 300; iter++) {
        Node *r = NULL;
        int keys[200], n = 0;
        for (int i = 0; i < 200; i++) {
            int k = rand() % 1000;
            r = bst_insert(r, k);
            keys[n++] = k;
        }
        for (int q = 0; q < 50; q++) {
            int a = keys[rand() % n], b = keys[rand() % n];
            assert(bst_lca(r, a, b) == lca(r, a, b));
        }
        tree_free(r);
    }
    printf("all height/diameter/LCA checks passed\n");
    return 0;
}
