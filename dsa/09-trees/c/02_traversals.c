/*
 * 02_traversals.c — pre/in/post-order (recursive + iterative), level-order, Morris
 *
 * Concept : Every depth-first traversal visits each node 3 times (arrive, come
 *           back from left, come back from right); pre/in/post just choose WHICH
 *           of the three moments to print. Recursion uses the call stack; the
 *           iterative versions use an explicit stack; level-order uses a queue;
 *           Morris uses no stack at all by temporarily threading NULL pointers.
 *           All versions are checked against each other on 2000 random trees.
 * Build   : make run P=dsa/09-trees/c/02_traversals.c
 * Read    : ../README.md  (section "Traversals")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void tree_free(Node *t)        /* O(n), post-order: children first */
{
    if (!t) return;
    tree_free(t->left);
    tree_free(t->right);
    free(t);
}

/* Output collector: traversals append keys here so we can compare them. */
typedef struct { int *v; int n; } Out;
static void emit(Out *o, int k) { o->v[o->n++] = k; }

/* ======================= recursive: O(n) time, O(h) stack ================= */
/*
 *   The "Euler tour": walk around the tree keeping it on your left hand.
 *   You pass each node 3 times:
 *
 *            (pre) A (post)          pre  : on the way DOWN into A
 *                 /|\                in   : BETWEEN left and right subtree
 *                / in \              post : on the way back UP out of A
 *               B      C
 */
static void pre_rec(const Node *t, Out *o)
{
    if (!t) return;
    emit(o, t->key);            /* 1st visit */
    pre_rec(t->left, o);
    pre_rec(t->right, o);
}
static void in_rec(const Node *t, Out *o)
{
    if (!t) return;
    in_rec(t->left, o);
    emit(o, t->key);            /* 2nd visit */
    in_rec(t->right, o);
}
static void post_rec(const Node *t, Out *o)
{
    if (!t) return;
    post_rec(t->left, o);
    post_rec(t->right, o);
    emit(o, t->key);            /* 3rd visit */
}

/* ======================= iterative: explicit stack, O(h) space ============
 * Why bother? Recursion depth = tree height. A degenerate tree of 1e6 nodes
 * overflows an 8 MB user stack - and in the kernel the stack is 16 KB, so
 * kernel code never recurses on data-dependent depth. */

/* A tiny fixed-capacity stack of node pointers (capacity = n is always enough). */
typedef struct { const Node **a; int top; } Stack;
static void push(Stack *s, const Node *x) { s->a[s->top++] = x; }
static const Node *pop(Stack *s) { return s->a[--s->top]; }
static const Node *peek(const Stack *s) { return s->a[s->top - 1]; }

/* Pre-order: pop, emit, push RIGHT then LEFT (so left is popped first). */
static void pre_iter(const Node *t, Out *o, Stack *s)
{
    s->top = 0;
    if (t) push(s, t);
    while (s->top) {
        const Node *x = pop(s);
        emit(o, x->key);
        if (x->right) push(s, x->right);   /* pushed first -> handled last */
        if (x->left)  push(s, x->left);
    }
}

/* In-order: "go left as far as you can, pushing; pop one, emit it, step right".
 *
 *        4        stack after diving left: [4,2,1]
 *       / \       pop 1 -> emit 1, go right (NULL)
 *      2   6      pop 2 -> emit 2, go right to 3, dive left: [4,3]
 *     / \         pop 3 -> emit 3 ... pop 4 -> emit 4, go right to 6 ...
 *    1   3                                                                  */
static void in_iter(const Node *t, Out *o, Stack *s)
{
    s->top = 0;
    const Node *cur = t;
    while (cur || s->top) {
        while (cur) { push(s, cur); cur = cur->left; }  /* dive left */
        cur = pop(s);
        emit(o, cur->key);
        cur = cur->right;                               /* then the right subtree */
    }
}

/* Post-order with ONE stack: a node may be emitted only when its right subtree
 * is done. We know it is done if right is NULL or right was the node we
 * emitted last ("prev"). */
static void post_iter(const Node *t, Out *o, Stack *s)
{
    s->top = 0;
    const Node *cur = t, *prev = NULL;
    while (cur || s->top) {
        while (cur) { push(s, cur); cur = cur->left; }
        const Node *top = peek(s);
        if (top->right && top->right != prev) {
            cur = top->right;            /* right subtree not done yet: go there */
        } else {
            emit(o, top->key);           /* both subtrees done */
            prev = pop(s);
        }
    }
}

/* ======================= level-order (BFS): queue, O(w) space =============
 *   w = maximum width; up to n/2 for a perfect tree (the whole last level). */
static void level_order(const Node *t, Out *o, int n, int *levels_out)
{
    if (!t) { *levels_out = 0; return; }
    const Node **q = malloc((size_t)n * sizeof *q);   /* each node enqueued once */
    if (!q) { perror("malloc"); exit(1); }
    int head = 0, tail = 0, levels = 0;
    q[tail++] = t;
    while (head < tail) {
        int width = tail - head;          /* everything queued now = one level */
        for (int i = 0; i < width; i++) {
            const Node *x = q[head++];
            emit(o, x->key);
            if (x->left)  q[tail++] = x->left;
            if (x->right) q[tail++] = x->right;
        }
        levels++;
    }
    free(q);
    *levels_out = levels;
}

/* ======================= Morris: O(n) time, O(1) extra space ==============
 * Idea: before descending into cur->left, find cur's in-order PREDECESSOR
 * (rightmost node of the left subtree). Its right pointer is NULL - wasted.
 * Point it back at cur: a temporary "thread" that replaces the stack.
 * When we later arrive at cur again through that thread we know the left
 * subtree is finished: remove the thread (restore NULL), emit, go right.
 *
 *   step 1: cur=4, pred(4)=3          step 2: cur=2, pred(2)=1
 *          4                                  4
 *         / \                                / \
 *        2   6     thread 3->4              2   6    thread 1->2
 *       / \                                / \
 *      1   3 - - > 4                      1   3 - -> 4
 *                                          \
 *                                           - -> 2
 *   Each edge is walked at most 3 times -> O(n) total, despite the inner loop.
 */
static void morris_in(Node *t, Out *o)
{
    Node *cur = t;
    while (cur) {
        if (!cur->left) {
            emit(o, cur->key);
            cur = cur->right;              /* may follow a thread back up */
            continue;
        }
        Node *pred = cur->left;
        while (pred->right && pred->right != cur) pred = pred->right;
        if (!pred->right) {                /* first time here: make thread */
            pred->right = cur;
            cur = cur->left;
        } else {                           /* second time: left done */
            pred->right = NULL;            /* restore the tree! */
            emit(o, cur->key);
            cur = cur->right;
        }
    }
}

/* Morris pre-order: identical walk, but emit when the thread is CREATED. */
static void morris_pre(Node *t, Out *o)
{
    Node *cur = t;
    while (cur) {
        if (!cur->left) {
            emit(o, cur->key);
            cur = cur->right;
            continue;
        }
        Node *pred = cur->left;
        while (pred->right && pred->right != cur) pred = pred->right;
        if (!pred->right) {
            emit(o, cur->key);             /* first arrival = pre-order */
            pred->right = cur;
            cur = cur->left;
        } else {
            pred->right = NULL;
            cur = cur->right;
        }
    }
}

/* ======================= test harness ===================================== */

/* Random shape: insert keys 0..n-1 by a random walk from the root until we
 * fall off at a NULL slot. Produces bushy, lopsided and chain-like shapes. */
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

static void print_out(const char *name, const Out *o)
{
    printf("%-12s:", name);
    for (int i = 0; i < o->n; i++) printf(" %d", o->v[i]);
    putchar('\n');
}

#define MAXN 300

int main(void)
{
    /*
     *            F
     *          /   \
     *         B     G
     *        / \     \
     *       A   D     I
     *          / \   /
     *         C   E H
     */
    Node *t = node_new('F');
    t->left = node_new('B');
    t->left->left = node_new('A');
    t->left->right = node_new('D');
    t->left->right->left = node_new('C');
    t->left->right->right = node_new('E');
    t->right = node_new('G');
    t->right->right = node_new('I');
    t->right->right->left = node_new('H');

    int buf[16];
    const Node *sbuf[16];
    Stack s = { sbuf, 0 };
    Out o = { buf, 0 };
    char str[16];

#define SHOW(name, call, expect)                                   \
    do {                                                           \
        o.n = 0; call;                                             \
        for (int i = 0; i < o.n; i++) str[i] = (char)o.v[i];       \
        str[o.n] = '\0';                                           \
        printf("%-14s %s\n", name, str);                           \
        assert(strcmp(str, expect) == 0);                          \
    } while (0)

    int lv;
    SHOW("pre  (rec)",  pre_rec(t, &o),         "FBADCEGIH");
    SHOW("pre  (iter)", pre_iter(t, &o, &s),    "FBADCEGIH");
    SHOW("pre  (morris)", morris_pre(t, &o),    "FBADCEGIH");
    SHOW("in   (rec)",  in_rec(t, &o),          "ABCDEFGHI");
    SHOW("in   (iter)", in_iter(t, &o, &s),     "ABCDEFGHI");
    SHOW("in   (morris)", morris_in(t, &o),     "ABCDEFGHI");
    SHOW("post (rec)",  post_rec(t, &o),        "ACEDBHIGF");
    SHOW("post (iter)", post_iter(t, &o, &s),   "ACEDBHIGF");
    SHOW("level",       level_order(t, &o, 9, &lv), "FBGADICEH");
    assert(lv == 4);
    tree_free(t);

    /* ---- randomised cross-check: all methods agree on 2000 random trees */
    srand(12345);
    static int ref[MAXN], got[MAXN];
    static const Node *stk[MAXN];
    Stack st = { stk, 0 };
    for (int iter = 0; iter < 2000; iter++) {
        int n = rand() % MAXN;
        Node *r = random_tree(n);
        Out a = { ref, 0 }, b = { got, 0 };

        pre_rec(r, &a);
        b.n = 0; pre_iter(r, &b, &st);
        assert(a.n == n && b.n == n && memcmp(ref, got, (size_t)n * sizeof(int)) == 0);
        b.n = 0; morris_pre(r, &b);
        assert(b.n == n && memcmp(ref, got, (size_t)n * sizeof(int)) == 0);

        a.n = 0; in_rec(r, &a);
        b.n = 0; in_iter(r, &b, &st);
        assert(b.n == n && memcmp(ref, got, (size_t)n * sizeof(int)) == 0);
        b.n = 0; morris_in(r, &b);
        assert(b.n == n && memcmp(ref, got, (size_t)n * sizeof(int)) == 0);
        /* Morris must leave the tree untouched: run in_rec again */
        b.n = 0; in_rec(r, &b);
        assert(memcmp(ref, got, (size_t)n * sizeof(int)) == 0);

        a.n = 0; post_rec(r, &a);
        b.n = 0; post_iter(r, &b, &st);
        assert(b.n == n && memcmp(ref, got, (size_t)n * sizeof(int)) == 0);

        /* level order visits every node exactly once */
        b.n = 0; level_order(r, &b, n > 0 ? n : 1, &lv);
        assert(b.n == n);
        if (n) assert(got[0] == r->key);
        tree_free(r);
    }

    Out demo = { got, 0 };
    srand(7);
    Node *r = random_tree(10);
    in_rec(r, &demo);
    print_out("random in", &demo);
    tree_free(r);

    printf("all traversal checks passed (2000 random trees)\n");
    return 0;
}
