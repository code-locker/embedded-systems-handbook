/*
 * 03_trie_linked_children.c — memory-lean trie: first-child / next-sibling
 *
 * Concept : The 26-pointer array of 01 wastes memory when nodes have few
 *           children (most do!). Here each node holds ONE character, a pointer
 *           to its first child and a pointer to its next sibling. Children form
 *           a sorted singly linked list, so any byte value 1..255 is allowed.
 *           Trade-off: finding a child is O(sigma) list walk instead of O(1).
 *           We measure bytes used by both layouts for the same word set.
 * Build   : make run P=dsa/11-tries/c/03_trie_linked_children.c
 * Read    : ../README.md  (section "Node layouts")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Words {"to", "tea", "ten", "i"}:
 *
 *   logical trie            first-child (down) / next-sibling (right)
 *
 *        root               root
 *       /    \               |
 *      i      t              i ---------> t
 *            / \                          |
 *           e   o                         e ------> o
 *          / \                            |
 *         a   n                           a -> n
 *
 * Each node: { char c; bool end; Node *child; Node *next; }  = 24 bytes
 * vs array layout: 26 * 8 + flags = 216 bytes.
 */
typedef struct Node {
    unsigned char c;
    bool end;
    struct Node *child; /* first child, children sorted by c */
    struct Node *next;  /* next sibling */
} Node;

static size_t g_bytes; /* bytes currently allocated, to compare layouts */

static Node *node_new(unsigned char c)
{
    Node *n = calloc(1, sizeof *n);
    if (!n) {
        perror("calloc");
        exit(1);
    }
    n->c = c;
    g_bytes += sizeof *n;
    return n;
}

static void node_free(Node *n)
{
    g_bytes -= sizeof *n;
    free(n);
}

/*
 * Find child `c` of `parent`, optionally creating it IN SORTED POSITION.
 * O(sigma) where sigma = number of children (<= 255).
 * Uses the pointer-to-pointer idiom: `link` is the address of the pointer we
 * might rewrite (parent->child or some sibling's ->next), so inserting at the
 * head and in the middle is the same code — no special case.
 */
static Node *child_get(Node *parent, unsigned char c, bool create)
{
    Node **link = &parent->child;
    while (*link && (*link)->c < c)
        link = &(*link)->next;
    if (*link && (*link)->c == c)
        return *link;
    if (!create)
        return NULL;
    Node *n = node_new(c);
    n->next = *link; /*  prev -> [n] -> old  */
    *link = n;
    return n;
}

static bool trie_insert(Node *root, const char *w) /* O(L * sigma) */
{
    Node *cur = root;
    for (const unsigned char *p = (const unsigned char *)w; *p; p++)
        cur = child_get(cur, *p, true);
    bool fresh = !cur->end;
    cur->end = true;
    return fresh;
}

static bool trie_search(Node *root, const char *w) /* O(L * sigma) */
{
    Node *cur = root;
    for (const unsigned char *p = (const unsigned char *)w; *p && cur; p++)
        cur = child_get(cur, *p, false);
    return cur && cur->end;
}

/*
 * Recursive delete with pruning. `link` points at the pointer that holds
 * `n` (parent's child pointer or left sibling's next) so we can unlink it.
 * Returns true if the word was found.
 */
static bool delete_rec(Node **link, const unsigned char *w)
{
    /* locate the child holding *w in the sibling list starting at *link */
    while (*link && (*link)->c < *w)
        link = &(*link)->next;
    Node *n = *link;
    if (!n || n->c != *w)
        return false;
    bool found;
    if (w[1] == '\0') {
        found = n->end;
        n->end = false;
    } else {
        found = delete_rec(&n->child, w + 1);
    }
    if (found && !n->end && !n->child) { /* useless now: unlink and free */
        *link = n->next;
        node_free(n);
    }
    return found;
}

static bool trie_delete(Node *root, const char *w)
{
    if (!*w) {
        bool f = root->end;
        root->end = false;
        return f;
    }
    return delete_rec(&root->child, (const unsigned char *)w);
}

/* Free everything below root. Iterates siblings, recurses on children only,
 * so recursion depth = word length, not number of siblings. */
static void trie_free(Node *n)
{
    while (n) {
        Node *nx = n->next;
        trie_free(n->child);
        node_free(n);
        n = nx;
    }
}

/* DFS visits siblings in sorted order -> words come out sorted. */
static void dump(const Node *n, char *buf, int d, char (*out)[16], int *cnt)
{
    for (; n; n = n->next) {
        buf[d] = (char)n->c;
        if (n->end) {
            memcpy(out[*cnt], buf, (size_t)d + 1);
            out[*cnt][d + 1] = '\0';
            (*cnt)++;
        }
        dump(n->child, buf, d + 1, out, cnt);
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

#define MAXW 3000
static char ref[MAXW][16];
static char got[MAXW][16];

int main(void)
{
    Node *root = node_new(0);
    const char *ws[] = {"to", "tea", "ten", "i", "inn", "Zoo", "caf\xc3\xa9"};
    for (size_t i = 0; i < sizeof ws / sizeof *ws; i++)
        assert(trie_insert(root, ws[i]));
    assert(trie_search(root, "tea") && !trie_search(root, "te"));
    assert(trie_search(root, "caf\xc3\xa9")); /* raw UTF-8 bytes just work */
    assert(trie_delete(root, "tea") && !trie_search(root, "tea") && trie_search(root, "ten"));
    assert(!trie_delete(root, "tea"));
    printf("linked-children trie: any byte works (UTF-8 'cafe' stored as bytes)\n");
    trie_free(root->child);
    root->child = NULL;

    /* ---- random words over a..z: compare memory and correctness ---- */
    srand(3);
    int n = 0;
    while (n < MAXW) {
        char w[16];
        int len = 3 + rand() % 8;
        for (int i = 0; i < len; i++)
            w[i] = (char)('a' + rand() % 26);
        w[len] = '\0';
        if (trie_insert(root, w))
            strcpy(ref[n++], w);
    }
    size_t lean = g_bytes;
    /* The array layout would need one 216-byte node per linked node. */
    size_t nodes = lean / sizeof(Node);
    printf("%d random words: %zu nodes, linked layout %zu KiB vs array layout ~%zu KiB\n", n,
           nodes, lean / 1024, nodes * (26 * sizeof(void *) + 8) / 1024);

    qsort(ref, (size_t)n, sizeof ref[0], cmp_str);
    char buf[16];
    int cnt = 0;
    dump(root->child, buf, 0, got, &cnt);
    assert(cnt == n);
    for (int i = 0; i < n; i++)
        assert(strcmp(got[i], ref[i]) == 0); /* DFS order == sorted order */

    /* delete every other word, check membership of all */
    for (int i = 0; i < n; i += 2)
        assert(trie_delete(root, ref[i]));
    for (int i = 0; i < n; i++)
        assert(trie_search(root, ref[i]) == (i % 2 == 1));
    for (int i = 1; i < n; i += 2)
        assert(trie_delete(root, ref[i]));
    assert(root->child == NULL && g_bytes == sizeof(Node)); /* fully pruned */
    node_free(root);
    assert(g_bytes == 0);
    printf("sorted dump, delete-all and pruning verified\n");
    return 0;
}
