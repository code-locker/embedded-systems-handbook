/*
 * 03_huffman.c — Huffman coding: build the tree with a min-heap, encode and decode
 *
 * Concept : Repeatedly merge the two LEAST frequent symbols. Rare symbols end up
 *           deep (long codes), common ones shallow (short codes). The result is
 *           a prefix-free code with the minimum possible total bit length.
 *           Used by DEFLATE (gzip, zlib, PNG), JPEG baseline, MP3, and is the
 *           idea behind H.264 CAVLC's variable-length code tables.
 * Build   : make run P=dsa/16-greedy/c/03_huffman.c
 * Read    : ../README.md  (section "Huffman coding")
 *
 * Memory  : all nodes live in one fixed array `pool` inside a HuffTree (no malloc
 *           per node), so there is nothing to free node by node. The encoded bit
 *           string is malloc'd by encode() and freed by the caller.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NSYM 256
#define MAXNODES (2 * NSYM) /* a full binary tree with L leaves has 2L-1 nodes */

typedef struct {
    long freq;
    int sym;          /* leaf: 0..255, internal: -1 */
    int left, right;  /* indices into pool, -1 for none */
    int order;        /* creation order: deterministic tie-break */
} Node;

typedef struct {
    Node pool[MAXNODES];
    int count;
    int root;
    char code[NSYM][NSYM]; /* code[c] = "0101..." as text, for teaching clarity */
} HuffTree;

/* ---------------- tiny binary min-heap of node indices ----------------
 * Ordered by (freq, order). The tie-break on `order` makes the tree — and so
 * the codes — deterministic, which matters if encoder and decoder must agree. */
typedef struct {
    int a[MAXNODES];
    int n;
    const Node *pool;
} Heap;

static int less(const Heap *h, int i, int j)
{
    const Node *x = &h->pool[h->a[i]], *y = &h->pool[h->a[j]];
    if (x->freq != y->freq) return x->freq < y->freq;
    return x->order < y->order;
}
static void swap_int(int *x, int *y) { int t = *x; *x = *y; *y = t; }

static void heap_push(Heap *h, int node)          /* O(log n) sift-up */
{
    int i = h->n++;
    h->a[i] = node;
    while (i > 0 && less(h, i, (i - 1) / 2)) { swap_int(&h->a[i], &h->a[(i - 1) / 2]); i = (i - 1) / 2; }
}
static int heap_pop(Heap *h)                      /* O(log n) sift-down */
{
    int top = h->a[0];
    h->a[0] = h->a[--h->n];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && less(h, l, m)) m = l;
        if (r < h->n && less(h, r, m)) m = r;
        if (m == i) break;
        swap_int(&h->a[i], &h->a[m]);
        i = m;
    }
    return top;
}

static void assign_codes(HuffTree *t, int node, char *buf, int depth)
{
    const Node *nd = &t->pool[node];
    if (nd->sym >= 0) {
        /* Single-symbol input: the root is a leaf, give it code "0" so it still
         * takes 1 bit per symbol (a 0-bit code could not be decoded). */
        if (depth == 0) buf[depth++] = '0';
        buf[depth] = '\0';
        strcpy(t->code[nd->sym], buf);
        return;
    }
    buf[depth] = '0'; assign_codes(t, nd->left, buf, depth + 1);   /* left edge  = 0 */
    buf[depth] = '1'; assign_codes(t, nd->right, buf, depth + 1);  /* right edge = 1 */
}

/*
 * Build: O(k log k) for k distinct symbols.
 *
 *   text "abracadabra":  a:5  b:2  r:2  c:1  d:1
 *
 *   heap: c1 d1 b2 r2 a5
 *   pop c1,d1 -> (cd)2            heap: b2 r2 (cd)2 a5
 *   pop b2,r2 -> (br)4            heap: (cd)2 (br)4 a5
 *   pop (cd)2,(br)4 -> 6          heap: a5 (cdbr)6
 *   pop a5,6  -> 11 = root
 *
 *              (11)
 *             0/  \1
 *             a5   (6)
 *                0/   \1
 *               (2)    (4)
 *              0/ \1  0/ \1
 *              c   d  b   r
 *
 *   a=0  c=100  d=101  b=110  r=111
 *   bits = 5*1 + 1*3 + 1*3 + 2*3 + 2*3 = 23   (fixed 3-bit code: 33, ASCII: 88)
 */
static void huff_build(HuffTree *t, const char *text)
{
    long freq[NSYM] = {0};
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) freq[*p]++;

    Heap h = {.n = 0, .pool = t->pool};
    t->count = 0;
    memset(t->code, 0, sizeof t->code);
    for (int c = 0; c < NSYM; c++) {
        if (!freq[c]) continue;
        t->pool[t->count] = (Node){freq[c], c, -1, -1, t->count};
        heap_push(&h, t->count++);
    }
    assert(h.n > 0 && "empty input has no code");
    while (h.n > 1) {
        /* THE greedy step: the two rarest become siblings at the deepest level. */
        int x = heap_pop(&h), y = heap_pop(&h);
        t->pool[t->count] = (Node){t->pool[x].freq + t->pool[y].freq, -1, x, y, t->count};
        heap_push(&h, t->count++);
    }
    t->root = heap_pop(&h);
    char buf[NSYM + 1];
    assign_codes(t, t->root, buf, 0);
}

/* Returns a malloc'd '0'/'1' string. Caller frees. O(total bits). */
static char *huff_encode(const HuffTree *t, const char *text)
{
    size_t bits = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) bits += strlen(t->code[*p]);
    char *out = malloc(bits + 1);
    assert(out);
    char *w = out;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        size_t L = strlen(t->code[*p]);
        memcpy(w, t->code[*p], L);
        w += L;
    }
    *w = '\0';
    return out;
}

/* Walk the tree bit by bit; every time a leaf is reached, emit and restart at root.
 * Works without separators BECAUSE the code is prefix-free (symbols only at leaves).
 * Writes into out (size cap). Returns decoded length. */
static size_t huff_decode(const HuffTree *t, const char *bits, char *out, size_t cap)
{
    size_t n = 0;
    int node = t->root;
    int single = t->pool[t->root].sym >= 0;
    for (const char *p = bits; *p; p++) {
        if (!single) node = (*p == '0') ? t->pool[node].left : t->pool[node].right;
        if (t->pool[node].sym >= 0) {
            assert(n + 1 < cap);
            out[n++] = (char)t->pool[node].sym;
            node = t->root;
        }
    }
    out[n] = '\0';
    return n;
}

/* Prefix-free check: no code is a prefix of another. O(k^2 * L). */
static int is_prefix_free(const HuffTree *t)
{
    for (int a = 0; a < NSYM; a++) {
        if (!t->code[a][0]) continue;
        for (int b = 0; b < NSYM; b++) {
            if (a == b || !t->code[b][0]) continue;
            if (strncmp(t->code[a], t->code[b], strlen(t->code[a])) == 0) return 0;
        }
    }
    return 1;
}

/* Cost = sum over symbols of freq * code length. Equals sum of internal node weights. */
static long internal_weight_sum(const HuffTree *t)
{
    long s = 0;
    for (int i = 0; i < t->count; i++) if (t->pool[i].sym < 0) s += t->pool[i].freq;
    return s;
}

/* Reference cost computed WITHOUT a tree or heap: keep the frequencies in a plain
 * array, repeatedly find the two smallest by linear scan (O(k^2)), merge them and
 * add the merged weight to the cost. Different code, same theorem -> same number. */
static long reference_cost(const char *text)
{
    long w[NSYM];
    int k = 0;
    long freq[NSYM] = {0};
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) freq[*p]++;
    for (int c = 0; c < NSYM; c++) if (freq[c]) w[k++] = freq[c];
    if (k == 1) return w[0]; /* 1 bit per symbol */
    long cost = 0;
    while (k > 1) {
        int i1 = 0;
        for (int i = 1; i < k; i++) if (w[i] < w[i1]) i1 = i;
        long a = w[i1]; w[i1] = w[--k];
        int i2 = 0;
        for (int i = 1; i < k; i++) if (w[i] < w[i2]) i2 = i;
        w[i2] += a;
        cost += w[i2];
    }
    return cost;
}

static void roundtrip(HuffTree *t, const char *text, int verbose)
{
    huff_build(t, text);
    char *bits = huff_encode(t, text);
    size_t len = strlen(text);
    assert((long)strlen(bits) == reference_cost(text));
    char *back = malloc(len + 1);
    assert(back);
    size_t got = huff_decode(t, bits, back, len + 1);
    assert(got == len && strcmp(back, text) == 0);
    assert(is_prefix_free(t));
    assert((long)strlen(bits) == (len == 0 ? 0 : (t->pool[t->root].sym >= 0 ? (long)len : internal_weight_sum(t))));
    if (verbose) {
        printf("text   : \"%s\"\n", text);
        for (int c = 0; c < NSYM; c++)
            if (t->code[c][0]) printf("  '%c' -> %s\n", c, t->code[c]);
        printf("encoded: %s\n", bits);
        printf("bits   : %zu (vs %zu bits as 8-bit ASCII)\n", strlen(bits), len * 8);
        printf("decoded: \"%s\"\n\n", back);
    }
    free(bits);
    free(back);
}

int main(void)
{
    static HuffTree t; /* ~66 KB: static so it does not sit on the stack */

    roundtrip(&t, "abracadabra", 1);
    assert(strcmp(t.code['a'], "0") == 0);
    assert(strlen(t.code['b']) == 3 && strlen(t.code['c']) == 3);
    char *bits = huff_encode(&t, "abracadabra");
    assert(strlen(bits) == 23);
    free(bits);

    roundtrip(&t, "aaaaaaa", 1);           /* edge case: one distinct symbol */
    roundtrip(&t, "the quick brown fox jumps over the lazy dog", 0);

    /* random strings over small alphabets (skewed, like real data) */
    srand(1603);
    char buf[400];
    for (int trial = 0; trial < 300; trial++) {
        int len = 1 + rand() % 300, alpha = 1 + rand() % 26;
        for (int i = 0; i < len; i++) {
            int r = rand() % alpha;
            buf[i] = (char)('a' + (r * r) % alpha); /* skewed distribution */
        }
        buf[len] = '\0';
        roundtrip(&t, buf, 0);
    }
    printf("300 random round-trips, prefix-free and cost checks: OK\n");
    return 0;
}
