/*
 * 02_autocomplete.c — prefix search and top-k autocomplete with a trie
 *
 * Concept : Autocomplete = (1) walk down the prefix, O(P); (2) DFS the subtree
 *           under it. Visiting children in index order 'a'..'z' yields words
 *           in SORTED order for free — a trie is an implicitly sorted set.
 *           We also store a frequency per word and return the k most frequent
 *           completions, like a shell, IDE or search box would.
 * Build   : make run P=dsa/11-tries/c/02_autocomplete.c
 * Read    : ../README.md  (section "Autocomplete")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALPHA 26
#define MAXL 16

typedef struct Node {
    struct Node *child[ALPHA];
    int freq; /* 0 = not a word; >0 = word, with how often it was used */
} Node;

static Node *node_new(void)
{
    Node *n = calloc(1, sizeof *n);
    if (!n) {
        perror("calloc");
        exit(1);
    }
    return n;
}

static void trie_free(Node *n) /* O(nodes), post-order */
{
    if (!n)
        return;
    for (int i = 0; i < ALPHA; i++)
        trie_free(n->child[i]);
    free(n);
}

/* O(L): add `times` uses of word w. */
static void trie_add(Node *root, const char *w, int times)
{
    Node *cur = root;
    for (; *w; w++) {
        int i = *w - 'a';
        if (!cur->child[i])
            cur->child[i] = node_new();
        cur = cur->child[i];
    }
    cur->freq += times;
}

/* ---- result list (caller-owned, fixed capacity) ---- */
typedef struct {
    char word[MAXL + 1];
    int freq;
} Hit;

typedef struct {
    Hit *v;
    int n, cap;
} Hits;

/*
 * DFS collecting every word under `n`. `buf[0..depth)` holds the path so far,
 * i.e. the word spelled by the edges from the root — we never store strings
 * in nodes; the PATH is the key.
 *
 *   prefix "ca":   root -c-> o -a-> [o]  <- start DFS here, buf = "ca"
 *                                  / | \
 *                                 r  t  ...   visit 'a'..'z' in order
 *
 * O(size of subtree). Output is lexicographically sorted.
 */
static void collect(const Node *n, char *buf, int depth, Hits *out)
{
    if (n->freq > 0 && out->n < out->cap) {
        buf[depth] = '\0';
        strcpy(out->v[out->n].word, buf);
        out->v[out->n].freq = n->freq;
        out->n++;
    }
    for (int i = 0; i < ALPHA; i++) {
        if (n->child[i]) {
            buf[depth] = (char)('a' + i);
            collect(n->child[i], buf, depth + 1, out);
        }
    }
}

/* All completions of `prefix`, sorted. O(P + subtree). */
static int complete_all(const Node *root, const char *prefix, Hits *out)
{
    char buf[MAXL + 1];
    const Node *cur = root;
    int d = 0;
    for (const char *p = prefix; *p && cur; p++) {
        cur = cur->child[*p - 'a'];
        buf[d++] = *p;
    }
    out->n = 0;
    if (cur)
        collect(cur, buf, d, out);
    return out->n;
}

/* Rank: higher frequency first, ties broken alphabetically (deterministic). */
static int by_rank(const void *a, const void *b)
{
    const Hit *x = a, *y = b;
    if (x->freq != y->freq)
        return y->freq - x->freq;
    return strcmp(x->word, y->word);
}

/*
 * Top-k: collect all, sort by rank, keep k. O(P + S + m log m), S = subtree
 * size, m = matches. Production systems (search engines) instead cache the
 * top-k list IN each node so a query is O(P + k) — see the README exercises.
 */
static int complete_topk(const Node *root, const char *prefix, int k, Hits *out)
{
    complete_all(root, prefix, out);
    qsort(out->v, (size_t)out->n, sizeof(Hit), by_rank);
    if (out->n > k)
        out->n = k;
    return out->n;
}

/* ---- reference: flat array of (word,freq) ---- */
#define MAXW 4000
static Hit ref[MAXW];
static int ref_n;

static void ref_add(const char *w, int t)
{
    for (int i = 0; i < ref_n; i++)
        if (strcmp(ref[i].word, w) == 0) {
            ref[i].freq += t;
            return;
        }
    strcpy(ref[ref_n].word, w);
    ref[ref_n++].freq = t;
}

static int by_word(const void *a, const void *b)
{
    return strcmp(((const Hit *)a)->word, ((const Hit *)b)->word);
}

int main(void)
{
    Node *root = node_new();
    static const struct {
        const char *w;
        int f;
    } dict[] = {{"git", 50},    {"gitk", 3},   {"gcc", 40},     {"gdb", 35},
                {"gst", 20},    {"grep", 45},  {"gzip", 5},     {"gstinspect", 8},
                {"gstlaunch", 30}, {"make", 60}, {"man", 25},   {"mount", 10}};
    for (size_t i = 0; i < sizeof dict / sizeof *dict; i++)
        trie_add(root, dict[i].w, dict[i].f);

    Hit store[64];
    Hits h = {store, 0, 64};

    complete_all(root, "gs", &h);
    printf("all completions of 'gs' (sorted by DFS order):");
    for (int i = 0; i < h.n; i++)
        printf(" %s", h.v[i].word);
    printf("\n");
    assert(h.n == 3 && strcmp(h.v[0].word, "gst") == 0 &&
           strcmp(h.v[1].word, "gstinspect") == 0 && strcmp(h.v[2].word, "gstlaunch") == 0);

    complete_topk(root, "g", 3, &h);
    printf("top-3 for 'g':");
    for (int i = 0; i < h.n; i++)
        printf(" %s(%d)", h.v[i].word, h.v[i].freq);
    printf("\n");
    assert(h.n == 3 && strcmp(h.v[0].word, "git") == 0 && strcmp(h.v[1].word, "grep") == 0 &&
           strcmp(h.v[2].word, "gcc") == 0);
    assert(complete_all(root, "x", &h) == 0); /* path missing -> none */
    trie_free(root);

    /* ---- randomised check vs reference ---- */
    srand(1234);
    root = node_new();
    static Hit big[MAXW];
    Hits hb = {big, 0, MAXW};
    static Hit exp[MAXW];
    for (int step = 0; step < 3000; step++) {
        char w[8];
        int len = 1 + rand() % 6;
        for (int i = 0; i < len; i++)
            w[i] = (char)('a' + rand() % 3);
        w[len] = '\0';
        int t = 1 + rand() % 5;
        trie_add(root, w, t);
        ref_add(w, t);

        if (step % 50 == 0) {
            char p[3] = {(char)('a' + rand() % 3), (char)('a' + rand() % 3), 0};
            p[rand() % 3 == 0 ? 1 : 2] = '\0'; /* prefix of length 1 or 2 */
            /* Expected: filter by prefix, sort by word (=DFS order). */
            int m = 0;
            for (int i = 0; i < ref_n; i++)
                if (strncmp(ref[i].word, p, strlen(p)) == 0)
                    exp[m++] = ref[i];
            qsort(exp, (size_t)m, sizeof(Hit), by_word);
            complete_all(root, p, &hb);
            assert(hb.n == m);
            for (int i = 0; i < m; i++)
                assert(strcmp(hb.v[i].word, exp[i].word) == 0 && hb.v[i].freq == exp[i].freq);
            /* top-5 must equal first 5 of the rank-sorted reference */
            qsort(exp, (size_t)m, sizeof(Hit), by_rank);
            complete_topk(root, p, 5, &hb);
            assert(hb.n == (m < 5 ? m : 5));
            for (int i = 0; i < hb.n; i++)
                assert(strcmp(hb.v[i].word, exp[i].word) == 0);
        }
    }
    trie_free(root);
    printf("randomised check: sorted completions + top-k match reference (%d words)\n", ref_n);
    return 0;
}
