/*
 * 04_serialize.c — serialise a binary tree to text and rebuild it exactly
 *
 * Concept : Pointers are meaningless outside the process, so to save a tree
 *           (file, socket, device-tree blob) you flatten it. One traversal is
 *           NOT enough to rebuild a shape - unless you also write the NULLs.
 *           Pre-order + '#' for NULL is unambiguous and rebuilds in one pass.
 *           Level-order with "null" (the LeetCode format) is shown as well.
 *           Both are round-tripped on 1000 random trees.
 * Build   : make run P=dsa/09-trees/c/04_serialize.c
 * Read    : ../README.md  (section "Serialise / deserialise")
 */
#include <assert.h>
#include <stdbool.h>
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

static void tree_free(Node *t)
{
    if (!t) return;
    tree_free(t->left);
    tree_free(t->right);
    free(t);
}

static bool tree_equal(const Node *a, const Node *b)   /* same shape + keys, O(n) */
{
    if (!a || !b) return a == b;
    return a->key == b->key && tree_equal(a->left, b->left) &&
           tree_equal(a->right, b->right);
}

static int tree_size(const Node *t)
{
    return t ? 1 + tree_size(t->left) + tree_size(t->right) : 0;
}

/* ---- growable string buffer (owner: caller frees sb.s) ------------------- */
typedef struct { char *s; size_t len, cap; } StrBuf;

static void sb_append(StrBuf *sb, const char *txt)
{
    size_t n = strlen(txt);
    if (sb->len + n + 1 > sb->cap) {               /* amortised O(1): doubling */
        size_t nc = sb->cap ? sb->cap * 2 : 64;
        while (nc < sb->len + n + 1) nc *= 2;
        char *p = realloc(sb->s, nc);
        if (!p) { perror("realloc"); exit(1); }
        sb->s = p;
        sb->cap = nc;
    }
    memcpy(sb->s + sb->len, txt, n + 1);
    sb->len += n;
}

/* =========================== format 1: pre-order with '#' ==================
 *
 *          1
 *         / \           pre-order, writing every NULL as '#':
 *        2   3              1 2 # # 3 4 # # 5 # #
 *           / \
 *          4   5         n nodes -> exactly n+1 '#' (every tree with n nodes
 *                        has n+1 NULL child slots)
 *
 * Why it is unambiguous: the reader recurses exactly like the writer, so
 * each token is consumed by the same "call" that produced it.
 * O(n) time both ways.
 */
static void ser_pre(const Node *t, StrBuf *sb)
{
    if (!t) { sb_append(sb, "# "); return; }
    char num[16];
    snprintf(num, sizeof num, "%d ", t->key);
    sb_append(sb, num);
    ser_pre(t->left, sb);
    ser_pre(t->right, sb);
}

/* *pp is a cursor into the string; each call consumes one subtree. */
static Node *deser_pre(const char **pp)
{
    const char *p = *pp;
    while (*p == ' ') p++;
    if (*p == '#') { *pp = p + 1; return NULL; }
    char *end;
    long v = strtol(p, &end, 10);
    assert(end != p && "malformed input");
    *pp = end;
    Node *x = node_new((int)v);
    x->left = deser_pre(pp);       /* order must match ser_pre exactly */
    x->right = deser_pre(pp);
    return x;
}

/* =========================== format 2: level-order ==========================
 *   [1,2,3,null,null,4,5]   (trailing nulls trimmed)
 * Writer: BFS, enqueue NULL children too, print "null" for them.
 * Reader: BFS again; each dequeued real node takes the next two tokens as
 *         its left and right children.
 */
static void ser_level(const Node *t, StrBuf *sb)
{
    sb_append(sb, "[");
    if (t) {
        int n = tree_size(t);
        const Node **q = malloc((size_t)(2 * n + 1) * sizeof *q);
        if (!q) { perror("malloc"); exit(1); }
        int head = 0, tail = 0, last_real = 0;
        q[tail++] = t;
        while (head < tail) {
            const Node *x = q[head++];
            if (x) {
                last_real = head;                  /* index+1 of last real node */
                q[tail++] = x->left;
                q[tail++] = x->right;
            }
        }
        for (int i = 0; i < last_real; i++) {      /* trim trailing nulls */
            char num[16];
            if (q[i]) snprintf(num, sizeof num, "%s%d", i ? "," : "", q[i]->key);
            else      snprintf(num, sizeof num, "%snull", i ? "," : "");
            sb_append(sb, num);
        }
        free(q);
    }
    sb_append(sb, "]");
}

/* Reads the next token: returns false at ']' ; *is_null set for "null". */
static bool next_token(const char **pp, bool *is_null, int *val)
{
    const char *p = *pp;
    while (*p == ',' || *p == ' ' || *p == '[') p++;
    if (*p == ']' || *p == '\0') { *pp = p; return false; }
    if (strncmp(p, "null", 4) == 0) { *is_null = true; *pp = p + 4; return true; }
    char *end;
    *val = (int)strtol(p, &end, 10);
    assert(end != p);
    *is_null = false;
    *pp = end;
    return true;
}

static Node *deser_level(const char *s)
{
    const char *p = s;
    bool is_null;
    int v;
    if (!next_token(&p, &is_null, &v) || is_null) return NULL;
    Node *root = node_new(v);
    /* queue capacity: number of tokens bounds number of nodes */
    size_t cap = strlen(s) / 2 + 2;
    Node **q = malloc(cap * sizeof *q);
    if (!q) { perror("malloc"); exit(1); }
    size_t head = 0, tail = 0;
    q[tail++] = root;
    while (head < tail) {
        Node *x = q[head++];
        if (!next_token(&p, &is_null, &v)) break;          /* rest are null */
        if (!is_null) { x->left = node_new(v); q[tail++] = x->left; }
        if (!next_token(&p, &is_null, &v)) break;
        if (!is_null) { x->right = node_new(v); q[tail++] = x->right; }
    }
    free(q);
    return root;
}

/* Random shape with random (possibly negative, possibly repeated) keys. */
static Node *random_tree(int n)
{
    Node *root = NULL;
    for (int k = 0; k < n; k++) {
        Node **link = &root;
        while (*link) link = (rand() & 1) ? &(*link)->left : &(*link)->right;
        *link = node_new(rand() % 2001 - 1000);
    }
    return root;
}

int main(void)
{
    Node *t = node_new(1);
    t->left = node_new(2);
    t->right = node_new(3);
    t->right->left = node_new(4);
    t->right->right = node_new(5);

    StrBuf sb = { 0 };
    ser_pre(t, &sb);
    printf("pre-order : %s\n", sb.s);
    assert(strcmp(sb.s, "1 2 # # 3 4 # # 5 # # ") == 0);
    const char *cur = sb.s;
    Node *back = deser_pre(&cur);
    assert(tree_equal(t, back));
    tree_free(back);
    free(sb.s);

    sb = (StrBuf){ 0 };
    ser_level(t, &sb);
    printf("level     : %s\n", sb.s);
    assert(strcmp(sb.s, "[1,2,3,null,null,4,5]") == 0);
    back = deser_level(sb.s);
    assert(tree_equal(t, back));
    tree_free(back);
    free(sb.s);
    tree_free(t);

    /* Why NULL markers matter: these two trees have the same pre-order
     * WITHOUT markers ("1 2") but different shapes:
     *      1        1
     *     /          \
     *    2            2          with markers: "1 2 # # #" vs "1 # 2 # #" */
    Node *a = node_new(1); a->left = node_new(2);
    Node *b = node_new(1); b->right = node_new(2);
    StrBuf sa = { 0 }, sbb = { 0 };
    ser_pre(a, &sa);
    ser_pre(b, &sbb);
    printf("left chain: %s | right chain: %s\n", sa.s, sbb.s);
    assert(strcmp(sa.s, sbb.s) != 0);
    free(sa.s); free(sbb.s);
    tree_free(a); tree_free(b);

    /* Empty tree round-trips too. */
    sb = (StrBuf){ 0 };
    ser_level(NULL, &sb);
    assert(strcmp(sb.s, "[]") == 0 && deser_level(sb.s) == NULL);
    free(sb.s);

    srand(99);
    for (int iter = 0; iter < 1000; iter++) {
        int n = rand() % 200;
        Node *r = random_tree(n);

        StrBuf s1 = { 0 };
        ser_pre(r, &s1);
        const char *c1 = s1.s;
        Node *r1 = deser_pre(&c1);
        assert(tree_equal(r, r1));

        StrBuf s2 = { 0 };
        ser_level(r, &s2);
        Node *r2 = deser_level(s2.s);
        assert(tree_equal(r, r2));

        free(s1.s); free(s2.s);
        tree_free(r); tree_free(r1); tree_free(r2);
    }
    printf("all serialise/deserialise round trips passed\n");
    return 0;
}
