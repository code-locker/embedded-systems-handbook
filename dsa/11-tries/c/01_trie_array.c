/*
 * 01_trie_array.c — classic trie with a fixed 26-slot child array per node
 *
 * Concept : A trie stores strings by sharing their prefixes. Each node is one
 *           character position; the edge you take is the letter. Here every
 *           node owns `children[26]` so finding the next node is ONE array
 *           index (O(1)), at the cost of 26 pointers per node. We implement
 *           insert, search, starts_with, delete (with pruning) and free, then
 *           verify against a sorted-array reference with random words.
 * Build   : make run P=dsa/11-tries/c/01_trie_array.c
 * Read    : ../README.md  (sections "Node layouts" and "How it works")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA 26 /* 'a'..'z' only; index = c - 'a' */

/*
 * Memory layout of one node (64-bit):
 *
 *   +-----------------------------------------------+
 *   | children[0] ('a') | children[1] ('b') | ...   |  26 * 8 = 208 bytes
 *   +-----------------------------------------------+
 *   | is_end (bool)     | pass (count of words below)|
 *   +-----------------------------------------------+
 *
 * `is_end` marks "a word ends exactly here" — essential, because "car" is a
 * prefix of "cart": the node for 'r' exists for both, only is_end tells you
 * whether "car" itself was inserted.
 */
typedef struct TrieNode {
    struct TrieNode *children[ALPHA];
    bool is_end;
    int pass; /* how many stored words go THROUGH this node (prefix count) */
} TrieNode;

/* Owner: caller of node_new owns it; trie_free releases the whole subtree. */
static TrieNode *node_new(void)
{
    TrieNode *n = calloc(1, sizeof *n); /* calloc => all children NULL */
    if (!n) {
        perror("calloc");
        exit(1);
    }
    return n;
}

static int idx(char c)
{
    assert(c >= 'a' && c <= 'z');
    return c - 'a';
}

/*
 * insert — O(L) time, L = word length; O(L) new nodes worst case.
 *
 * insert "car" then "cat":
 *
 *   root            root            root
 *    |c              |c              |c
 *    o               o               o
 *    |a              |a              |a
 *    o               o               o
 *    |r             / \r            / \
 *    (o)  end     t(o) (o)         ...   "cat" shares "ca"
 *
 * Returns true if the word was new (sets are duplicate-free).
 */
static bool trie_insert(TrieNode *root, const char *w)
{
    TrieNode *cur = root;
    /* First check whether it is already present so `pass` counts stay exact. */
    for (const char *p = w; *p; p++) {
        TrieNode *nx = cur->children[idx(*p)];
        if (!nx) {
            cur = NULL;
            break;
        }
        cur = nx;
    }
    if (cur && cur->is_end)
        return false; /* duplicate: no change */

    cur = root;
    cur->pass++;
    for (const char *p = w; *p; p++) {
        int i = idx(*p);
        if (!cur->children[i])
            cur->children[i] = node_new(); /* O(1): one allocation */
        cur = cur->children[i];
        cur->pass++;
    }
    cur->is_end = true;
    return true;
}

/* Walk the path for `s`; returns the node at the end or NULL. O(L). */
static const TrieNode *walk(const TrieNode *root, const char *s)
{
    const TrieNode *cur = root;
    for (; *s && cur; s++)
        cur = cur->children[idx(*s)];
    return cur;
}

/* search — O(L): exact word present? Path must exist AND end flag set. */
static bool trie_search(const TrieNode *root, const char *w)
{
    const TrieNode *n = walk(root, w);
    return n && n->is_end;
}

/* starts_with — O(L): how many stored words have this prefix? Thanks to the
 * `pass` counter this is O(L) instead of O(L + size of subtree). */
static int trie_count_prefix(const TrieNode *root, const char *prefix)
{
    const TrieNode *n = walk(root, prefix);
    return n ? n->pass : 0;
}

static bool node_is_empty(const TrieNode *n)
{
    for (int i = 0; i < ALPHA; i++)
        if (n->children[i])
            return false;
    return true;
}

/*
 * delete — O(L * 26) worst case (the emptiness check scans 26 slots).
 *
 * Recursive: go down, clear is_end at the bottom, then on the way back up
 * free every node that has become useless (no children, not a word end).
 *
 * Delete "cat" from {car, cat}:
 *
 *      c - a - r(end)          c - a - r(end)
 *            \                 'a' still needed by "car"
 *             t(end)  ---->    't' freed (leaf, no longer an end)
 *
 * Returns true if `n` itself should be freed by the caller.
 */
static bool delete_rec(TrieNode *n, const char *w)
{
    n->pass--;
    if (*w == '\0') {
        n->is_end = false;
    } else {
        int i = idx(*w);
        if (delete_rec(n->children[i], w + 1)) {
            free(n->children[i]);
            n->children[i] = NULL;
        }
    }
    return !n->is_end && node_is_empty(n);
}

static bool trie_delete(TrieNode *root, const char *w)
{
    if (!trie_search(root, w))
        return false; /* nothing to delete; keep counters untouched */
    /* Reuse the recursion from the root; the root itself is never freed
     * (it is the handle the caller holds), so ignore its "free me" answer. */
    (void)delete_rec(root, w);
    return true;
}

/* Post-order free: children first, then the node. O(number of nodes). */
static void trie_free(TrieNode *n)
{
    if (!n)
        return;
    for (int i = 0; i < ALPHA; i++)
        trie_free(n->children[i]);
    free(n);
}

static size_t trie_count_nodes(const TrieNode *n)
{
    if (!n)
        return 0;
    size_t c = 1;
    for (int i = 0; i < ALPHA; i++)
        c += trie_count_nodes(n->children[i]);
    return c;
}

/* ---------------------------------------------------------------------------
 * Reference model: a plain array of strings, searched linearly. Slow (O(n*L))
 * but obviously correct, which is exactly what a self-check needs.
 * ------------------------------------------------------------------------- */
#define MAXW 2000
#define MAXL 8
static char ref[MAXW][MAXL + 1];
static int ref_n;

static int ref_find(const char *w)
{
    for (int i = 0; i < ref_n; i++)
        if (strcmp(ref[i], w) == 0)
            return i;
    return -1;
}

static int ref_prefix(const char *p)
{
    size_t l = strlen(p);
    int c = 0;
    for (int i = 0; i < ref_n; i++)
        if (strncmp(ref[i], p, l) == 0)
            c++;
    return c;
}

/* Small alphabet (a..d) so random words collide and share prefixes a lot. */
static void rand_word(char *buf, int maxlen)
{
    int len = 1 + rand() % maxlen;
    for (int i = 0; i < len; i++)
        buf[i] = (char)('a' + rand() % 4);
    buf[len] = '\0';
}

int main(void)
{
    /* ---- 1. small hand-checked demo ---- */
    TrieNode *root = node_new();
    const char *words[] = {"car", "cart", "care", "cat", "dog", "do"};
    for (size_t i = 0; i < sizeof words / sizeof *words; i++)
        trie_insert(root, words[i]);

    printf("search car  -> %d\n", trie_search(root, "car"));
    printf("search ca   -> %d (prefix only, not a word)\n", trie_search(root, "ca"));
    printf("prefix 'car' count -> %d (car, cart, care)\n", trie_count_prefix(root, "car"));
    assert(trie_search(root, "car") && !trie_search(root, "ca"));
    assert(trie_count_prefix(root, "car") == 3);
    assert(trie_count_prefix(root, "do") == 2);
    assert(!trie_insert(root, "cat")); /* duplicate */

    size_t before = trie_count_nodes(root);
    assert(trie_delete(root, "cart"));
    size_t after = trie_count_nodes(root);
    printf("delete cart: nodes %zu -> %zu (only the 't' leaf freed)\n", before, after);
    assert(after == before - 1);
    assert(trie_search(root, "car") && !trie_search(root, "cart"));
    assert(trie_delete(root, "car")); /* 'r' node stays: "care" goes through it */
    assert(trie_search(root, "care") && !trie_search(root, "car"));
    assert(!trie_delete(root, "zzz"));
    trie_free(root);

    /* ---- 2. randomised check against the reference array ---- */
    srand(11);
    root = node_new();
    char w[MAXL + 1];
    for (int step = 0; step < 20000; step++) {
        rand_word(w, 6);
        int op = rand() % 3;
        if (op == 0 && ref_n < MAXW) { /* insert */
            bool fresh = trie_insert(root, w);
            bool ref_fresh = ref_find(w) < 0;
            assert(fresh == ref_fresh);
            if (ref_fresh)
                strcpy(ref[ref_n++], w);
        } else if (op == 1) { /* delete */
            int at = ref_find(w);
            assert(trie_delete(root, w) == (at >= 0));
            if (at >= 0)
                memmove(ref[at], ref[--ref_n], sizeof ref[0]); /* swap-remove (may be itself) */
        } else { /* query */
            assert(trie_search(root, w) == (ref_find(w) >= 0));
            size_t cut = (size_t)(1 + rand() % 2); /* query a 1-2 char prefix */
            if (cut < strlen(w))
                w[cut] = '\0';
            assert(trie_count_prefix(root, w) == ref_prefix(w));
        }
    }
    /* Delete everything: the trie must shrink back to just the root, which
     * proves delete prunes every dead branch. */
    while (ref_n > 0)
        assert(trie_delete(root, ref[--ref_n]));
    assert(trie_count_nodes(root) == 1 && root->pass == 0);
    trie_free(root);

    printf("randomised check: 20000 ops vs reference OK; trie pruned to root\n");
    printf("node size = %zu bytes (26 pointers dominate)\n", sizeof(TrieNode));
    return 0;
}
