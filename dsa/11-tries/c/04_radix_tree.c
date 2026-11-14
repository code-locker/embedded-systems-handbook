/*
 * 04_radix_tree.c — compressed trie (radix tree / Patricia-style) for strings
 *
 * Concept : In a plain trie, long chains of single-child nodes waste memory
 *           ("romane" -> r-o-m-a-n-e, six nodes). A radix tree COMPRESSES each
 *           such chain into one edge labelled with a whole string. Invariant:
 *           every non-root node either ends a word or has >= 2 children.
 *           Insert may SPLIT an edge; delete may MERGE a node into its only
 *           child. Same O(L) lookups, far fewer nodes.
 * Build   : make run P=dsa/11-tries/c/04_radix_tree.c
 * Read    : ../README.md  (section "Compressed trie / radix tree")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA 26

/*
 * Each node owns the label of the edge LEADING INTO it. In a radix tree no two
 * children of a node start with the same character (otherwise they would share
 * a prefix and be merged), so we can index children by their FIRST char.
 */
typedef struct RNode {
    char *label; /* heap string, owned by this node; "" for the root */
    bool end;
    struct RNode *kid[ALPHA];
} RNode;

static size_t g_nodes; /* live node count, for the demo and leak checks */

static RNode *rnode_new(const char *label, size_t len)
{
    RNode *n = calloc(1, sizeof *n);
    char *s = malloc(len + 1);
    if (!n || !s) {
        perror("alloc");
        exit(1);
    }
    memcpy(s, label, len);
    s[len] = '\0';
    n->label = s;
    g_nodes++;
    return n;
}

static void rnode_free(RNode *n)
{
    free(n->label);
    free(n);
    g_nodes--;
}

static void rtree_free(RNode *n) /* O(nodes) post-order */
{
    if (!n)
        return;
    for (int i = 0; i < ALPHA; i++)
        rtree_free(n->kid[i]);
    rnode_free(n);
}

static size_t common_prefix(const char *a, const char *b)
{
    size_t i = 0;
    while (a[i] && a[i] == b[i])
        i++;
    return i;
}

/*
 * insert — O(L) character comparisons (+ O(L) copying when splitting).
 *
 * Case A: no child starts with w[0]  -> hang a new leaf with ALL of w.
 * Case B: child label is a full prefix of w -> consume it, descend.
 * Case C: partial match of k chars -> SPLIT the edge:
 *
 *   insert "romulus" into a tree holding "romane":
 *
 *      (root)                     (root)
 *        |"romane"                  |"rom"        <- new middle node
 *       [*]            ===>        [ ]
 *                                 /    \
 *                          "ane"[*]   [*]"ulus"
 *
 *   If w ends exactly at the split point ("rom"), the middle node gets end=true
 *   and no new leaf is needed.
 */
static bool rtree_insert(RNode *root, const char *w)
{
    RNode *cur = root;
    for (;;) {
        if (*w == '\0') {
            bool fresh = !cur->end;
            cur->end = true;
            return fresh;
        }
        int i = *w - 'a';
        RNode *c = cur->kid[i];
        if (!c) { /* case A */
            c = rnode_new(w, strlen(w));
            c->end = true;
            cur->kid[i] = c;
            return true;
        }
        size_t k = common_prefix(c->label, w);
        size_t clen = strlen(c->label);
        if (k == clen) { /* case B */
            cur = c;
            w += k;
            continue;
        }
        /* case C: split c's edge at k (1 <= k < clen) */
        RNode *mid = rnode_new(c->label, k);
        memmove(c->label, c->label + k, clen - k + 1); /* c keeps the tail */
        mid->kid[c->label[0] - 'a'] = c;
        cur->kid[i] = mid;
        w += k;
        if (*w == '\0') {
            mid->end = true;
        } else {
            RNode *leaf = rnode_new(w, strlen(w));
            leaf->end = true;
            mid->kid[*w - 'a'] = leaf;
        }
        return true;
    }
}

/* search — O(L): every edge label must match completely. */
static bool rtree_search(const RNode *root, const char *w)
{
    const RNode *cur = root;
    while (*w) {
        const RNode *c = cur->kid[*w - 'a'];
        if (!c)
            return false;
        size_t clen = strlen(c->label);
        if (strncmp(c->label, w, clen) != 0)
            return false; /* diverged inside the edge */
        w += clen;
        cur = c;
    }
    return cur->end;
}

static int kid_count(const RNode *n, int *only)
{
    int cnt = 0;
    for (int i = 0; i < ALPHA; i++)
        if (n->kid[i]) {
            cnt++;
            *only = i;
        }
    return cnt;
}

/*
 * Merge `n` with its single child to restore the invariant:
 *
 *        |"rom"                |"romane"
 *       [ ]          ===>     [*]
 *        |"ane"
 *       [*]
 *
 * The child's label is appended to n's, n adopts the child's flag and kids.
 */
static void merge_with_only_child(RNode *n, int only)
{
    RNode *c = n->kid[only];
    size_t a = strlen(n->label), b = strlen(c->label);
    char *s = malloc(a + b + 1);
    if (!s) {
        perror("malloc");
        exit(1);
    }
    memcpy(s, n->label, a);
    memcpy(s + a, c->label, b + 1);
    free(n->label);
    n->label = s;
    n->end = c->end;
    memcpy(n->kid, c->kid, sizeof n->kid);
    memset(c->kid, 0, sizeof c->kid); /* ownership moved to n */
    rnode_free(c);
}

/*
 * delete — O(L). Recurse down; on the way up fix the child we came from:
 *   - not a word and no kids   -> remove it
 *   - not a word and one kid   -> merge it with that kid
 */
static bool del_rec(RNode *parent, const char *w)
{
    int i = *w - 'a';
    RNode *c = parent->kid[i];
    if (!c)
        return false;
    size_t clen = strlen(c->label);
    if (strncmp(c->label, w, clen) != 0)
        return false;
    w += clen;
    if (*w == '\0') {
        if (!c->end)
            return false;
        c->end = false;
    } else if (!del_rec(c, w)) {
        return false;
    }
    int only = -1;
    int nk = kid_count(c, &only);
    if (!c->end && nk == 0) {
        parent->kid[i] = NULL;
        rnode_free(c);
    } else if (!c->end && nk == 1) {
        merge_with_only_child(c, only);
    }
    return true;
}

static bool rtree_delete(RNode *root, const char *w)
{
    if (*w == '\0') {
        bool f = root->end;
        root->end = false;
        return f;
    }
    return del_rec(root, w);
}

/* Check the radix invariant everywhere: non-root nodes are words or branch. */
static void check_invariant(const RNode *n, bool is_root)
{
    int only = -1;
    int nk = kid_count(n, &only);
    if (!is_root) {
        assert(n->label[0] != '\0');
        assert(n->end || nk >= 2);
    }
    for (int i = 0; i < ALPHA; i++)
        if (n->kid[i]) {
            assert(n->kid[i]->label[0] == 'a' + i);
            check_invariant(n->kid[i], false);
        }
}

static void print_tree(const RNode *n, int depth)
{
    for (int i = 0; i < ALPHA; i++) {
        const RNode *c = n->kid[i];
        if (!c)
            continue;
        printf("%*s\"%s\"%s\n", depth * 2 + 2, "", c->label, c->end ? " *" : "");
        print_tree(c, depth + 1);
    }
}

#define MAXW 1500
static char ref[MAXW][12];
static int ref_n;

static int ref_find(const char *w)
{
    for (int i = 0; i < ref_n; i++)
        if (strcmp(ref[i], w) == 0)
            return i;
    return -1;
}

int main(void)
{
    RNode *root = rnode_new("", 0);
    const char *ws[] = {"romane", "romanus", "romulus", "rubens", "ruber", "rubicon", "rubicundus"};
    for (size_t i = 0; i < sizeof ws / sizeof *ws; i++)
        rtree_insert(root, ws[i]);
    printf("radix tree of the classic Wikipedia example (* = word end), %zu nodes:\n", g_nodes);
    print_tree(root, 0);
    /* plain trie would need one node per distinct prefix: */
    assert(g_nodes == 14); /* root + 13; a plain trie needs 1 + 27 nodes */
    check_invariant(root, true);
    assert(rtree_search(root, "romulus") && !rtree_search(root, "rom"));

    assert(rtree_insert(root, "rom")); /* ends exactly at a split point */
    assert(rtree_search(root, "rom"));
    assert(rtree_delete(root, "romulus"));
    printf("after insert 'rom' and delete 'romulus' ('om' is a word with 1 child):\n");
    print_tree(root, 0);
    check_invariant(root, true);
    assert(rtree_delete(root, "rom")); /* 'om' now non-word with 1 kid -> MERGE */
    printf("after delete 'rom' ('om' + 'an' merged into 'oman'):\n");
    print_tree(root, 0);
    check_invariant(root, true);
    rtree_free(root);
    assert(g_nodes == 0);

    /* ---- randomised check vs reference + invariant after every op ---- */
    srand(77);
    root = rnode_new("", 0);
    for (int step = 0; step < 20000; step++) {
        char w[12];
        int len = 1 + rand() % 7;
        for (int i = 0; i < len; i++)
            w[i] = (char)('a' + rand() % 3);
        w[len] = '\0';
        int at = ref_find(w);
        switch (rand() % 3) {
        case 0:
            if (ref_n < MAXW) {
                assert(rtree_insert(root, w) == (at < 0));
                if (at < 0)
                    strcpy(ref[ref_n++], w);
            }
            break;
        case 1:
            assert(rtree_delete(root, w) == (at >= 0));
            if (at >= 0)
                memmove(ref[at], ref[--ref_n], sizeof ref[0]);
            break;
        default:
            assert(rtree_search(root, w) == (at >= 0));
        }
        if (step % 100 == 0)
            check_invariant(root, true);
    }
    check_invariant(root, true);
    while (ref_n > 0)
        assert(rtree_delete(root, ref[--ref_n]));
    assert(g_nodes == 1); /* only the root survives */
    rtree_free(root);
    printf("randomised check: 20000 ops vs reference, invariant held, no leaks\n");
    return 0;
}
