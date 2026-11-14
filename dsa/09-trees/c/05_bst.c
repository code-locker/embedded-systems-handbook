/*
 * 05_bst.c — binary search tree: insert, search, delete (3 cases), min/max,
 *            successor/predecessor, floor/ceil, validate, in-order = sorted
 *
 * Concept : BST rule: everything in the left subtree < node < everything in the
 *           right subtree. Every operation walks ONE root-to-leaf path, so it
 *           costs O(h): O(log n) when bushy, O(n) when keys arrive sorted.
 *           Delete has three cases (0, 1 or 2 children). 200000 random
 *           operations are checked against a plain presence array.
 * Build   : make run P=dsa/09-trees/c/05_bst.c
 * Read    : ../README.md  (section "Binary search tree")
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
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

/* ---- search: O(h), iterative, no stack needed ---------------------------- */
static Node *bst_search(Node *t, int key)
{
    while (t && t->key != key)
        t = key < t->key ? t->left : t->right;   /* discard half each step */
    return t;
}

/* ---- insert: O(h) ----------------------------------------------------------
 * The pointer-to-pointer trick: `link` points at the slot (root, or some
 * node's left/right field) that will hold the new node. No special case for
 * the empty tree, no "remember the parent".
 *
 *   insert 5 into:     8            link walks: &root -> &8.left -> &3.right
 *                     / \                      -> &6.left (NULL) -> put 5 here
 *                    3   10
 *                     \
 *                      6
 *                     /
 *                   (5)
 * Returns false if the key already exists (this BST stores a set).
 */
static bool bst_insert(Node **root, int key)
{
    Node **link = root;
    while (*link) {
        if (key == (*link)->key) return false;
        link = key < (*link)->key ? &(*link)->left : &(*link)->right;
    }
    *link = node_new(key);
    return true;
}

/* ---- min / max: O(h) - just keep going left (right) ---------------------- */
static Node *bst_min(Node *t) { while (t && t->left) t = t->left; return t; }
static Node *bst_max(Node *t) { while (t && t->right) t = t->right; return t; }

/* ---- delete: O(h) ----------------------------------------------------------
 *
 * Case 1: leaf (0 children) - just cut it off.
 *
 *        8                8
 *       / \     del 4    / \
 *      3   10   ---->   3   10
 *       \                \
 *        6                6
 *       /
 *      4                (6.left becomes NULL)
 *
 * Case 2: one child - the child moves up into the hole.
 *
 *        8                8
 *       / \     del 10   / \
 *      3   10   ---->   3   14
 *            \             /
 *             14          13
 *            /
 *           13
 *
 * Case 3: two children - cannot just remove it (two orphans, one slot).
 *         Replace its KEY with its in-order successor (smallest key in the
 *         right subtree), then delete that successor node. The successor
 *         has NO left child (else that would be smaller), so deleting it
 *         is case 1 or case 2.
 *
 *        8                       9                    9
 *       / \      del 8         / \                  / \
 *      3   12    ---->        3   12     ---->     3   12
 *         /  \   succ(8)=9       /  \  splice 9's     /  \
 *        9    14 copy 9 up      9    14 right child  10   14
 *         \                      \     into its slot
 *          10                     10
 *
 *  The order 3 < 9 < 10 < 12 < 14 is preserved: 9 was the next key after 8.
 */
static bool bst_delete(Node **root, int key)
{
    Node **link = root;
    while (*link && (*link)->key != key)
        link = key < (*link)->key ? &(*link)->left : &(*link)->right;
    Node *victim = *link;
    if (!victim) return false;

    if (victim->left && victim->right) {          /* case 3: two children */
        Node **slink = &victim->right;             /* find successor slot */
        while ((*slink)->left) slink = &(*slink)->left;
        Node *succ = *slink;
        victim->key = succ->key;                   /* copy key up */
        *slink = succ->right;                      /* splice out successor */
        free(succ);
    } else {                                       /* case 1 or 2 */
        *link = victim->left ? victim->left : victim->right;  /* NULL for leaf */
        free(victim);
    }
    return true;
}
/* Note: Copying the key is fine for ints. If nodes carry big payloads or
 * other code holds pointers to nodes (like the Linux rbtree, where the node
 * is embedded in the user's struct), you must instead RELINK the successor
 * node into the victim's position. The C++ version (cpp/02) does that. */

/* ---- successor / predecessor without parent pointers: O(h) ---------------
 * successor(x): if x has a right subtree -> min of that subtree.
 * Otherwise it is the nearest ancestor for which x lies in the LEFT subtree.
 * Walking down from the root, every time we go left we remember that node:
 * the last one remembered is the answer.
 *
 *          20           succ(15): go left at 20 (remember 20), right at 10,
 *         /             right at 15? no - found 15 with no right child.
 *       10              Answer = last "left turn" = 20.
 *         \
 *          15
 */
static Node *bst_successor(Node *root, int key)
{
    Node *best = NULL;
    while (root) {
        if (key < root->key) { best = root; root = root->left; }
        else root = root->right;     /* key >= root: answer is further right */
    }
    return best;                     /* smallest key strictly greater than key */
}

static Node *bst_predecessor(Node *root, int key)
{
    Node *best = NULL;
    while (root) {
        if (key > root->key) { best = root; root = root->right; }
        else root = root->left;
    }
    return best;                     /* largest key strictly less than key */
}

/* floor = largest key <= x, ceil = smallest key >= x. O(h). */
static Node *bst_floor(Node *t, int x)
{
    Node *best = NULL;
    while (t) {
        if (t->key == x) return t;
        if (t->key < x) { best = t; t = t->right; } else t = t->left;
    }
    return best;
}
static Node *bst_ceil(Node *t, int x)
{
    Node *best = NULL;
    while (t) {
        if (t->key == x) return t;
        if (t->key > x) { best = t; t = t->left; } else t = t->right;
    }
    return best;
}

/* ---- validate: O(n) --------------------------------------------------------
 * WRONG check: "left child < node < right child" at every node. This tree
 * passes it but is not a BST (6 is in 10's left subtree... fine, but 12 is
 * in 10's LEFT subtree and 12 > 10):
 *
 *        10
 *       /
 *      5
 *       \
 *        12      <- 5 < 12 locally OK, but 12 > 10: broken
 *
 * RIGHT check: carry the allowed open interval (lo, hi) down the tree.
 * long long bounds so INT_MIN / INT_MAX keys still work. */
static bool valid_rec(const Node *t, long long lo, long long hi)
{
    if (!t) return true;
    if (t->key <= lo || t->key >= hi) return false;
    return valid_rec(t->left, lo, t->key) && valid_rec(t->right, t->key, hi);
}
static bool bst_valid(const Node *t) { return valid_rec(t, LLONG_MIN, LLONG_MAX); }

static bool local_check_only(const Node *t)       /* the broken version */
{
    if (!t) return true;
    if (t->left && t->left->key >= t->key) return false;
    if (t->right && t->right->key <= t->key) return false;
    return local_check_only(t->left) && local_check_only(t->right);
}

static int inorder_to(const Node *t, int *out, int n)   /* returns new n */
{
    if (!t) return n;
    n = inorder_to(t->left, out, n);
    out[n++] = t->key;
    return inorder_to(t->right, out, n);
}

static int height(const Node *t)
{
    if (!t) return -1;
    int a = height(t->left), b = height(t->right);
    return 1 + (a > b ? a : b);
}

#define U 2000          /* key universe 0..U-1 for the random test */

int main(void)
{
    Node *root = NULL;
    int keys[] = { 8, 3, 10, 1, 6, 14, 4, 7, 13 };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        assert(bst_insert(&root, keys[i]));
    /*
     *            8
     *          /   \
     *         3     10
     *        / \      \
     *       1   6      14
     *          / \    /
     *         4   7  13
     */
    assert(!bst_insert(&root, 6));                /* duplicate rejected */
    assert(bst_search(root, 7) && !bst_search(root, 5));
    assert(bst_min(root)->key == 1 && bst_max(root)->key == 14);
    assert(bst_successor(root, 7)->key == 8);     /* climb case */
    assert(bst_successor(root, 8)->key == 10);    /* right-subtree case */
    assert(bst_successor(root, 14) == NULL);
    assert(bst_predecessor(root, 8)->key == 7);
    assert(bst_predecessor(root, 4)->key == 3);
    assert(bst_floor(root, 5)->key == 4 && bst_ceil(root, 5)->key == 6);
    assert(bst_floor(root, 0) == NULL && bst_ceil(root, 15) == NULL);
    assert(bst_valid(root));

    int out[U];
    int n = inorder_to(root, out, 0);
    printf("in-order :");
    for (int i = 0; i < n; i++) printf(" %d", out[i]);
    printf("   (always sorted for a BST)\n");

    assert(bst_delete(&root, 4));     /* case 1: leaf */
    assert(bst_delete(&root, 10));    /* case 2: one child (14) */
    assert(bst_delete(&root, 3));     /* case 3: two children, succ = 6 */
    assert(!bst_delete(&root, 99));
    assert(bst_valid(root));
    n = inorder_to(root, out, 0);
    printf("after deleting 4 (leaf), 10 (one child), 3 (two children):");
    for (int i = 0; i < n; i++) printf(" %d", out[i]);
    putchar('\n');
    assert(n == 6 && root->left->key == 6);      /* 6 took 3's place */
    tree_free(root);

    /* The broken validator is fooled; the interval validator is not. */
    Node *bad = node_new(10);
    bad->left = node_new(5);
    bad->left->right = node_new(12);
    assert(local_check_only(bad) && !bst_valid(bad));
    tree_free(bad);

    /* Sorted insertion degenerates into a list: height n-1. */
    root = NULL;
    for (int i = 0; i < 1000; i++) bst_insert(&root, i);
    printf("1000 sorted inserts -> height %d (a linked list!)\n", height(root));
    assert(height(root) == 999);
    tree_free(root);

    /* Randomised check against a presence array (the "simple reference"). */
    srand(42);
    static bool present[U];
    int count = 0;
    root = NULL;
    for (int op = 0; op < 200000; op++) {
        int k = rand() % U;
        switch (rand() % 3) {
        case 0: {                                  /* insert */
            bool ins = bst_insert(&root, k);
            assert(ins == !present[k]);
            if (ins) { present[k] = true; count++; }
            break;
        }
        case 1: {                                  /* delete */
            bool del = bst_delete(&root, k);
            assert(del == present[k]);
            if (del) { present[k] = false; count--; }
            break;
        }
        default: {                                 /* search + successor */
            assert((bst_search(root, k) != NULL) == present[k]);
            int ref = -1;
            for (int j = k + 1; j < U; j++) if (present[j]) { ref = j; break; }
            Node *s = bst_successor(root, k);
            assert(ref == -1 ? s == NULL : (s && s->key == ref));
        }
        }
        if (op % 5000 == 0) {                      /* full structural check */
            assert(bst_valid(root));
            n = inorder_to(root, out, 0);
            assert(n == count);
            for (int i = 1; i < n; i++) assert(out[i - 1] < out[i]);
        }
    }
    printf("random tree of %d keys has height %d (random inserts give ~3*log2(n);\n"
           "  log2(n) would be ~10: plain BSTs are never perfectly balanced)\n",
           count, height(root));
    tree_free(root);
    printf("all BST checks passed (200000 random ops)\n");
    return 0;
}
