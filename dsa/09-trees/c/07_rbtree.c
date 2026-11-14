/*
 * 07_rbtree.c — red-black tree, Linux-kernel style (intrusive nodes)
 *
 * Concept : A red-black tree colours each node red or black and keeps 5 rules
 *           that force height <= 2*log2(n+1). It is "less perfectly" balanced
 *           than AVL but repairs itself with at most 2 rotations per insert
 *           and 3 per delete, which is why the Linux kernel (lib/rbtree.c),
 *           std::map and the CFS scheduler use it. The node is EMBEDDED in
 *           the user's struct (like struct rb_node in the kernel) and we get
 *           back to the user struct with container_of(). Rules are verified
 *           after every one of 60000 random operations.
 * Build   : make run P=dsa/09-trees/c/07_rbtree.c
 * Read    : ../README.md  (section "Red-black tree")
 */
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* ========================= the generic part (like lib/rbtree.c) ============
 * It knows nothing about keys. Users do the BST walk themselves and only ask
 * the library to (a) link a new node and recolour/rotate, (b) erase a node.
 */
enum { RB_RED = 0, RB_BLACK = 1 };

struct rb_node {
    struct rb_node *parent, *left, *right;
    int color;
};
struct rb_root { struct rb_node *node; };

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

static bool is_red(const struct rb_node *n) { return n && n->color == RB_RED; }

/* Make `newc` take `old`'s place under old's parent (or as the root). O(1). */
static void replace_child(struct rb_root *root, struct rb_node *old,
                          struct rb_node *newc, struct rb_node *parent)
{
    if (!parent)                 root->node = newc;
    else if (parent->left == old) parent->left = newc;
    else                          parent->right = newc;
}

/*
 * rotate_left(x): O(1), keeps in-order order.     rotate_right is the mirror.
 *
 *        P                   P
 *        |                   |
 *        x                   y
 *       / \       ---->     / \
 *      a   y               x   c
 *         / \             / \
 *        b   c           a   b
 *
 * With parent pointers there are 6 links to fix instead of 3: the price of
 * O(1) successor and O(1) "erase this node I already hold".
 */
static void rotate_left(struct rb_root *root, struct rb_node *x)
{
    struct rb_node *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    replace_child(root, x, y, x->parent);
    y->left = x;
    x->parent = y;
}

static void rotate_right(struct rb_root *root, struct rb_node *x)
{
    struct rb_node *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    replace_child(root, x, y, x->parent);
    y->right = x;
    x->parent = y;
}

/* Like the kernel's rb_link_node(): put a new RED node in the slot found by
 * the caller's search walk. O(1). */
static void rb_link_node(struct rb_node *node, struct rb_node *parent,
                         struct rb_node **link)
{
    node->parent = parent;
    node->left = node->right = NULL;
    node->color = RB_RED;          /* red: does not change any black-height */
    *link = node;
}

/*
 * rb_insert_color(): fix a possible red-red violation. O(log n) recolourings,
 * at most 2 rotations. z is red; the only rule that can be broken is
 * "a red node has no red child" (z and its parent p both red).
 * (Notation: g = grandparent, u = uncle = g's other child. Shown for p on the
 *  left of g; the code also handles the mirror image.)
 *
 * Case 1: uncle is RED -> recolour, push the problem 2 levels up.
 *
 *          g(B)                  g(R)   <- may now clash with ITS parent:
 *         /    \                /    \     continue the loop with z = g
 *       p(R)   u(R)   ---->   p(B)   u(B)
 *       /                     /
 *     z(R)                  z(R)
 *
 * Case 2: uncle BLACK, z is an "inner" grandchild (zig-zag) -> rotate p
 *         to turn it into case 3.
 *
 *          g(B)                 g(B)
 *         /    \               /    \
 *       p(R)   u(B)  ---->   z(R)   u(B)
 *          \                 /
 *          z(R)            p(R)     (now rename: z <-> p)
 *
 * Case 3: uncle BLACK, z is an "outer" grandchild (straight line) ->
 *         rotate g, swap colours of p and g. Done: subtree root is black.
 *
 *          g(B)                  p(B)
 *         /    \                /    \
 *       p(R)   u(B)  ---->   z(R)    g(R)
 *       /                               \
 *     z(R)                              u(B)
 */
static void rb_insert_color(struct rb_root *root, struct rb_node *z)
{
    struct rb_node *p;
    while ((p = z->parent) && p->color == RB_RED) {
        struct rb_node *g = p->parent;   /* exists: a red node is never root */
        if (p == g->left) {
            struct rb_node *u = g->right;
            if (is_red(u)) {                              /* case 1 */
                p->color = u->color = RB_BLACK;
                g->color = RB_RED;
                z = g;
                continue;
            }
            if (z == p->right) {                          /* case 2 */
                rotate_left(root, p);
                z = p;
                p = z->parent;
            }
            p->color = RB_BLACK;                          /* case 3 */
            g->color = RB_RED;
            rotate_right(root, g);
        } else {                                          /* mirror */
            struct rb_node *u = g->left;
            if (is_red(u)) {
                p->color = u->color = RB_BLACK;
                g->color = RB_RED;
                z = g;
                continue;
            }
            if (z == p->left) {
                rotate_right(root, p);
                z = p;
                p = z->parent;
            }
            p->color = RB_BLACK;
            g->color = RB_RED;
            rotate_left(root, g);
        }
    }
    root->node->color = RB_BLACK;        /* rule 2: root is black */
}

/*
 * Erase fix-up. Removing a BLACK node leaves its side one black short: we
 * pretend x carries an "extra black" and push it up or get rid of it.
 * w = x's sibling. (Shown for x on the left; mirror in code.)
 *
 * Case 1: w RED -> rotate parent, recolour; now x's sibling is black (2,3,4).
 * Case 2: w BLACK, both w's children BLACK -> colour w red, move the extra
 *         black up to the parent (loop; stops if parent was red).
 * Case 3: w BLACK, w's far child BLACK, near child RED -> rotate w so the
 *         red child becomes the far one (turns into case 4).
 * Case 4: w BLACK, w's far child RED -> rotate parent, w takes parent's
 *         colour, parent and far child become black. Extra black absorbed:
 *         DONE.
 *
 *   case 4:      P(?)                    w(?)
 *               /    \                  /    \
 *            x(B+)   w(B)    ---->    P(B)   r(B)
 *                   /   \             /  \
 *                  c    r(R)        x(B)  c
 */
static void rb_erase_color(struct rb_root *root, struct rb_node *x,
                           struct rb_node *parent)
{
    while (x != root->node && !is_red(x)) {
        if (x == parent->left) {          /* works for x == NULL too: the
                                             sibling cannot be NULL here */
            struct rb_node *w = parent->right;
            if (is_red(w)) {                                   /* case 1 */
                w->color = RB_BLACK;
                parent->color = RB_RED;
                rotate_left(root, parent);
                w = parent->right;
            }
            if (!is_red(w->left) && !is_red(w->right)) {       /* case 2 */
                w->color = RB_RED;
                x = parent;
                parent = x->parent;
            } else {
                if (!is_red(w->right)) {                       /* case 3 */
                    w->left->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_right(root, w);
                    w = parent->right;
                }
                w->color = parent->color;                      /* case 4 */
                parent->color = RB_BLACK;
                w->right->color = RB_BLACK;
                rotate_left(root, parent);
                x = root->node;
                break;
            }
        } else {                                               /* mirror */
            struct rb_node *w = parent->left;
            if (is_red(w)) {
                w->color = RB_BLACK;
                parent->color = RB_RED;
                rotate_right(root, parent);
                w = parent->left;
            }
            if (!is_red(w->left) && !is_red(w->right)) {
                w->color = RB_RED;
                x = parent;
                parent = x->parent;
            } else {
                if (!is_red(w->left)) {
                    w->right->color = RB_BLACK;
                    w->color = RB_RED;
                    rotate_left(root, w);
                    w = parent->left;
                }
                w->color = parent->color;
                parent->color = RB_BLACK;
                w->left->color = RB_BLACK;
                rotate_right(root, parent);
                x = root->node;
                break;
            }
        }
    }
    if (x) x->color = RB_BLACK;
}

/* Put v where u was (u's subtree slot). Like CLRS RB-TRANSPLANT. O(1). */
static void transplant(struct rb_root *root, struct rb_node *u, struct rb_node *v)
{
    replace_child(root, u, v, u->parent);
    if (v) v->parent = u->parent;
}

static struct rb_node *rb_first(const struct rb_root *root)   /* O(log n) */
{
    struct rb_node *n = root->node;
    if (!n) return NULL;
    while (n->left) n = n->left;
    return n;
}

/* Successor using parent pointers: amortised O(1) over a full walk.
 * Right subtree exists -> its leftmost node. Otherwise climb while we are
 * a right child; the first parent we reach from its left is the answer. */
static struct rb_node *rb_next(const struct rb_node *n)
{
    if (n->right) {
        n = n->right;
        while (n->left) n = n->left;
        return (struct rb_node *)n;
    }
    while (n->parent && n == n->parent->right) n = n->parent;
    return n->parent;
}

/* rb_erase: remove a node we already hold a pointer to. O(log n).
 * The node is NOT freed: the library does not own it - the user does. */
static void rb_erase(struct rb_root *root, struct rb_node *z)
{
    struct rb_node *x, *xparent;
    int removed_color = z->color;

    if (!z->left) {                                   /* 0 or 1 child (right) */
        x = z->right;
        xparent = z->parent;
        transplant(root, z, z->right);
    } else if (!z->right) {                           /* 1 child (left) */
        x = z->left;
        xparent = z->parent;
        transplant(root, z, z->left);
    } else {                                          /* 2 children */
        struct rb_node *y = z->right;                 /* successor */
        while (y->left) y = y->left;
        removed_color = y->color;                     /* y's slot loses a node */
        x = y->right;
        if (y->parent == z) {
            xparent = y;
        } else {
            xparent = y->parent;
            transplant(root, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        transplant(root, z, y);                       /* RELINK, not key copy:
                                                         user pointers to y stay
                                                         valid */
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }
    if (removed_color == RB_BLACK)
        rb_erase_color(root, x, xparent);
}

/* ========================= the user part ==================================
 * The user embeds rb_node and writes their own search/insert walk, exactly
 * like kernel code does (see Documentation/core-api/rbtree.rst).
 */
struct item {
    int key;
    struct rb_node rb;           /* embedded: no separate allocation */
};

static struct item *item_search(struct rb_root *root, int key)   /* O(log n) */
{
    struct rb_node *n = root->node;
    while (n) {
        struct item *it = container_of(n, struct item, rb);
        if (key < it->key) n = n->left;
        else if (key > it->key) n = n->right;
        else return it;
    }
    return NULL;
}

static bool item_insert(struct rb_root *root, struct item *it)   /* O(log n) */
{
    struct rb_node **link = &root->node, *parent = NULL;
    while (*link) {
        struct item *cur = container_of(*link, struct item, rb);
        parent = *link;
        if (it->key < cur->key) link = &(*link)->left;
        else if (it->key > cur->key) link = &(*link)->right;
        else return false;
    }
    rb_link_node(&it->rb, parent, link);
    rb_insert_color(root, &it->rb);
    return true;
}

/* ---- rule checker: returns black-height, asserts all 5 rules ----------- */
static int rb_check(const struct rb_node *n, const struct rb_node *parent,
                    long long lo, long long hi, int *count)
{
    if (!n) return 1;                                     /* NIL leaves black */
    const struct item *it = container_of(n, const struct item, rb);
    assert(n->parent == parent);                          /* links consistent */
    assert(it->key > lo && it->key < hi);                 /* BST order */
    assert(n->color == RB_RED || n->color == RB_BLACK);   /* rule 1 */
    if (n->color == RB_RED)                               /* rule 4 */
        assert(!is_red(n->left) && !is_red(n->right));
    int bl = rb_check(n->left, n, lo, it->key, count);
    int br = rb_check(n->right, n, it->key, hi, count);
    assert(bl == br);                                     /* rule 5 */
    (*count)++;
    return bl + (n->color == RB_BLACK);
}

static int rb_height(const struct rb_node *n)
{
    if (!n) return 0;
    int a = rb_height(n->left), b = rb_height(n->right);
    return 1 + (a > b ? a : b);
}

static void print_rb(const struct rb_node *n, int indent)
{
    if (!n) return;
    print_rb(n->right, indent + 6);
    printf("%*s%d%s\n", indent, "", container_of(n, const struct item, rb)->key,
           n->color == RB_RED ? "(R)" : "(B)");
    print_rb(n->left, indent + 6);
}

/* ---- mini CFS: pick the task with the smallest vruntime ------------------
 * The Linux CFS scheduler (and its successor EEVDF) keeps runnable tasks in
 * an rbtree keyed by virtual runtime and caches the leftmost node
 * (rb_root_cached) so "who runs next?" is O(1). Equal keys go right, so
 * ties are served FIFO. */
struct task {
    const char *name;
    long vruntime;
    int weight;              /* higher weight -> vruntime grows slower */
    struct rb_node rb;
};

static void task_enqueue(struct rb_root *rq, struct task *t)
{
    struct rb_node **link = &rq->node, *parent = NULL;
    while (*link) {
        struct task *cur = container_of(*link, struct task, rb);
        parent = *link;
        link = t->vruntime < cur->vruntime ? &(*link)->left : &(*link)->right;
    }
    rb_link_node(&t->rb, parent, link);
    rb_insert_color(rq, &t->rb);
}

#define U 4000

int main(void)
{
    /* ---- small demo: insert 1..10 in order -------------------------------- */
    struct rb_root root = { NULL };
    struct item small[10];
    for (int i = 0; i < 10; i++) {
        small[i].key = i + 1;
        assert(item_insert(&root, &small[i]));
    }
    printf("red-black tree after inserting 1..10 (sideways):\n");
    print_rb(root.node, 0);
    int cnt = 0;
    int bh = rb_check(root.node, NULL, LLONG_MIN, LLONG_MAX, &cnt);
    assert(cnt == 10 && root.node->color == RB_BLACK);
    printf("black-height %d, height %d\n\n", bh, rb_height(root.node));

    /* Erasing by pointer: no search needed - this is why intrusive trees are
     * popular in kernels (a timer/vma/task already knows its node). */
    rb_erase(&root, &small[3].rb);           /* key 4 */
    rb_erase(&root, &small[0].rb);           /* key 1 */
    assert(!item_search(&root, 4) && item_search(&root, 5));
    cnt = 0;
    rb_check(root.node, NULL, LLONG_MIN, LLONG_MAX, &cnt);
    assert(cnt == 8);
    /* in-order walk with rb_first/rb_next (parent pointers, no stack) */
    printf("in-order after erasing 4 and 1:");
    int prev = INT_MIN;
    for (struct rb_node *n = rb_first(&root); n; n = rb_next(n)) {
        int k = container_of(n, struct item, rb)->key;
        printf(" %d", k);
        assert(k > prev);
        prev = k;
    }
    printf("\n\n");
    /* nodes live in the `small` array: nothing to free */

    /* ---- sorted inserts: height stays <= 2 log2(n+1) ---------------------- */
    enum { NS = 1 << 15 };
    struct item *many = malloc(NS * sizeof *many);    /* owner: main, freed below */
    if (!many) { perror("malloc"); return 1; }
    root.node = NULL;
    for (int i = 0; i < NS; i++) {
        many[i].key = i;
        item_insert(&root, &many[i]);
    }
    cnt = 0;
    rb_check(root.node, NULL, LLONG_MIN, LLONG_MAX, &cnt);
    printf("%d sorted inserts -> height %d (bound 2*log2(n+1) = 30, AVL would be ~16)\n",
           NS, rb_height(root.node));
    assert(cnt == NS && rb_height(root.node) <= 30);
    free(many);                       /* tree nodes live inside `many` */

    /* ---- randomised test vs presence array, rules checked after every op -- */
    static struct item pool[U];       /* one item per possible key */
    static bool present[U];
    int size = 0;
    root.node = NULL;
    srand(5);
    for (int op = 0; op < 60000; op++) {
        int k = rand() % U;
        if (rand() % 2) {
            if (!present[k]) {
                pool[k].key = k;
                assert(item_insert(&root, &pool[k]));
                present[k] = true;
                size++;
            } else {
                struct item dup = { .key = k };
                assert(!item_insert(&root, &dup));
            }
        } else {
            struct item *it = item_search(&root, k);
            assert((it != NULL) == present[k]);
            if (it) {
                rb_erase(&root, &it->rb);
                present[k] = false;
                size--;
            }
        }
        cnt = 0;
        rb_check(root.node, NULL, LLONG_MIN, LLONG_MAX, &cnt);
        assert(cnt == size);
        assert(!root.node || root.node->color == RB_BLACK);     /* rule 2 */
    }
    printf("random ops: size=%d height=%d\n", size, rb_height(root.node));

    /* ---- mini CFS scheduler ----------------------------------------------- */
    struct task tasks[] = {
        { "gst-pipeline", 0, 3, { 0 } },   /* "nice" task: weight 3 */
        { "sshd",         0, 1, { 0 } },
        { "make -j",      0, 1, { 0 } },
    };
    struct rb_root rq = { NULL };
    for (int i = 0; i < 3; i++) task_enqueue(&rq, &tasks[i]);
    int ran[3] = { 0 };
    printf("\nmini-CFS schedule (12 slices of 6ms):");
    for (int slice = 0; slice < 12; slice++) {
        struct rb_node *left = rb_first(&rq);      /* kernel caches this: O(1) */
        struct task *t = container_of(left, struct task, rb);
        rb_erase(&rq, left);
        printf(" %s", t->name);
        t->vruntime += 6 * 3 / t->weight;          /* heavier -> slower clock */
        ran[t - tasks]++;
        task_enqueue(&rq, t);
    }
    printf("\nslices: gst=%d sshd=%d make=%d (weight 3:1:1 -> 6:3:3)\n",
           ran[0], ran[1], ran[2]);
    assert(ran[0] == 6 && ran[1] == 3 && ran[2] == 3);

    printf("all red-black tree checks passed\n");
    return 0;
}
