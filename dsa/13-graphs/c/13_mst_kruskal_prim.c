/*
 * 13_mst_kruskal_prim.c — minimum spanning tree: Kruskal (sort + DSU) and
 *                         Prim (grow from one vertex with a min-heap)
 *
 * Concept : CUT PROPERTY: split the vertices into any two groups; the
 *           cheapest edge crossing between the groups is safe to put in the
 *           MST. Both algorithms are just different ways to pick cuts:
 *             Kruskal: take edges cheapest-first, skip any edge whose ends are
 *                      already connected (DSU says same set => would make a
 *                      cycle). The cut = "the component of one end vs rest".
 *             Prim   : the cut = "tree so far vs everything else"; repeatedly
 *                      take the cheapest edge leaving the tree (min-heap).
 *           Verified on the README graph (weight 39), against each other on
 *           random connected graphs, and against brute force (try every
 *           subset of n-1 edges) on tiny graphs.
 * Build   : make run P=dsa/13-graphs/c/13_mst_kruskal_prim.c
 * Read    : ../README.md and ../notes/07-mst.md
 *
 *   G9 (undirected, weighted; A..G = 0..6):
 *
 *            7         8
 *       0 ------ 1 ------ 2
 *        \      / \      /
 *        5\   9/   \7   /5
 *          \  /     \  /
 *           3 ------ 4         3-4:15  3-5:6  4-5:8  4-6:9  5-6:11
 *            \  15  / \
 *            6\   8/   \9
 *              \  /     \
 *               5 ------ 6
 *                   11
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16, MAXM = 64 };

typedef struct { int u, v, w; } Edge;

/* ------------------------------------------------------------------ DSU */
typedef struct { int parent[MAXN], rank[MAXN]; } Dsu;
static void dsu_init(Dsu *d, int n) { for (int i = 0; i < n; i++) { d->parent[i] = i; d->rank[i] = 0; } }
static int dsu_find(Dsu *d, int x)
{
    while (d->parent[x] != x) { d->parent[x] = d->parent[d->parent[x]]; x = d->parent[x]; }  /* path halving */
    return x;
}
static int dsu_union(Dsu *d, int a, int b)
{
    a = dsu_find(d, a); b = dsu_find(d, b);
    if (a == b) return 0;
    if (d->rank[a] < d->rank[b]) { int t = a; a = b; b = t; }
    d->parent[b] = a;
    if (d->rank[a] == d->rank[b]) d->rank[a]++;
    return 1;
}

/* -------------------------------------------------------------- Kruskal */
static int cmp_edge(const void *x, const void *y)
{
    const Edge *a = x, *b = y;
    if (a->w != b->w) return (a->w > b->w) - (a->w < b->w);
    if (a->u != b->u) return a->u - b->u;           /* deterministic ties */
    return a->v - b->v;
}

/* O(E log E) for the sort + O(E alpha(V)) for the DSU work. Returns total
 * weight; chosen[] receives the tree edges. Works on a COPY of the edges. */
static int kruskal(int n, const Edge *in, int m, Edge *chosen, int *k, int trace)
{
    Edge e[MAXM];
    memcpy(e, in, sizeof(Edge) * (size_t)m);
    qsort(e, (size_t)m, sizeof(Edge), cmp_edge);
    Dsu d;
    dsu_init(&d, n);
    int total = 0;
    *k = 0;
    for (int i = 0; i < m && *k < n - 1; i++) {     /* n-1 edges => done early */
        int ok = dsu_union(&d, e[i].u, e[i].v);
        if (ok) { chosen[(*k)++] = e[i]; total += e[i].w; }
        if (trace)
            printf("  %2d: %d-%d  %s\n", e[i].w, e[i].u, e[i].v,
                   ok ? "TAKE" : "skip (ends already connected: would close a cycle)");
    }
    return total;
}

/* ----------------------------------------------------------------- Prim */
typedef struct { int w, from, to; } HItem;
typedef struct { HItem a[2 * MAXM + 4]; int len; } Heap;

static int hless(HItem x, HItem y) { return x.w != y.w ? x.w < y.w : x.to < y.to; }
static void hpush(Heap *h, HItem x)
{
    CHECK(h->len < (int)(sizeof h->a / sizeof h->a[0]));
    int i = h->len++;
    h->a[i] = x;
    while (i && hless(h->a[i], h->a[(i - 1) / 2])) { HItem t = h->a[i]; h->a[i] = h->a[(i - 1) / 2]; h->a[(i - 1) / 2] = t; i = (i - 1) / 2; }
}
static HItem hpop(Heap *h)
{
    HItem top = h->a[0];
    h->a[0] = h->a[--h->len];
    for (int i = 0;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < h->len && hless(h->a[l], h->a[s])) s = l;
        if (r < h->len && hless(h->a[r], h->a[s])) s = r;
        if (s == i) break;
        HItem t = h->a[i]; h->a[i] = h->a[s]; h->a[s] = t; i = s;
    }
    return top;
}

/* O(E log E) with a lazy heap (stale entries skipped when their target is
 * already in the tree). Needs adjacency: built here from the edge list. */
static int prim(int n, const Edge *e, int m, int *k, int trace)
{
    static Heap h;
    int in_tree[MAXN] = {0}, total = 0;
    h.len = 0;
    *k = 0;
    in_tree[0] = 1;
    for (int i = 0; i < m; i++) {                   /* edges leaving vertex 0 */
        if (e[i].u == 0) hpush(&h, (HItem){ e[i].w, 0, e[i].v });
        if (e[i].v == 0) hpush(&h, (HItem){ e[i].w, 0, e[i].u });
    }
    while (h.len && *k < n - 1) {
        HItem x = hpop(&h);
        if (in_tree[x.to]) {
            if (trace) printf("  pop %2d: %d-%d  stale (%d already in tree)\n", x.w, x.from, x.to, x.to);
            continue;
        }
        in_tree[x.to] = 1;
        total += x.w;
        (*k)++;
        if (trace) printf("  pop %2d: %d-%d  TAKE, tree now has %d vertices\n", x.w, x.from, x.to, *k + 1);
        for (int i = 0; i < m; i++) {               /* O(m) scan: fine for tiny demo graphs */
            if (e[i].u == x.to && !in_tree[e[i].v]) hpush(&h, (HItem){ e[i].w, x.to, e[i].v });
            if (e[i].v == x.to && !in_tree[e[i].u]) hpush(&h, (HItem){ e[i].w, x.to, e[i].u });
        }
    }
    return total;
}

/* ----------------------------------------------------------- brute force */
static int brute_mst(int n, const Edge *e, int m)
{
    int best = -1;
    for (unsigned mask = 0; mask < (1u << m); mask++) {
        if (__builtin_popcount(mask) != n - 1) continue;
        Dsu d; dsu_init(&d, n);
        int w = 0, ok = 1;
        for (int i = 0; i < m && ok; i++)
            if (mask >> i & 1) { ok = dsu_union(&d, e[i].u, e[i].v); w += e[i].w; }
        if (ok && (best < 0 || w < best)) best = w;   /* n-1 edges, no cycle => spanning tree */
    }
    return best;
}

static unsigned rng_state = 5150u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

/* random CONNECTED graph: a random spanning tree first, then extra edges */
static int random_connected(int n, int extra, Edge *e, int maxw)
{
    int m = 0;
    for (int v = 1; v < n; v++) e[m++] = (Edge){ (int)(rnd() % (unsigned)v), v, 1 + (int)(rnd() % (unsigned)maxw) };
    for (int i = 0; i < extra; i++) {
        int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
        if (a != b) e[m++] = (Edge){ a, b, 1 + (int)(rnd() % (unsigned)maxw) };
    }
    return m;
}

int main(void)
{
    static const Edge G9[] = { {0,1,7},{0,3,5},{1,2,8},{1,3,9},{1,4,7},{2,4,5},
                               {3,4,15},{3,5,6},{4,5,8},{4,6,9},{5,6,11} };
    Edge tree[MAXN];
    int k;
    printf("Kruskal on G9 (edges sorted by weight):\n");
    int wk = kruskal(7, G9, 11, tree, &k, 1);
    printf("  MST weight = %d with %d edges:", wk, k);
    for (int i = 0; i < k; i++) printf(" %d-%d(%d)", tree[i].u, tree[i].v, tree[i].w);
    printf("\n\nPrim on G9 from vertex 0 (lazy min-heap):\n");
    int kp;
    int wp = prim(7, G9, 11, &kp, 1);
    printf("  MST weight = %d\n", wp);

    CHECK(wk == 39 && wp == 39 && k == 6 && kp == 6);
    CHECK(brute_mst(7, G9, 11) == 39);

    for (int t = 0; t < 300; t++) {
        Edge e[MAXM];
        int n = 2 + (int)(rnd() % 6);
        int m = random_connected(n, (int)(rnd() % 6), e, 9);   /* m <= 12 => 4096 subsets */
        int a = kruskal(n, e, m, tree, &k, 0), b = prim(n, e, m, &kp, 0), c = brute_mst(n, e, m);
        CHECK(a == b && b == c && k == n - 1 && kp == n - 1);
    }
    for (int t = 0; t < 200; t++) {
        Edge e[MAXM];
        int n = 2 + (int)(rnd() % 14);
        int m = random_connected(n, (int)(rnd() % 30), e, 50);
        CHECK(kruskal(n, e, m, tree, &k, 0) == prim(n, e, m, &kp, 0));
    }
    printf("\nrandom cross-check: Kruskal == Prim == brute force (300 tiny graphs),\n"
           "Kruskal == Prim (200 larger graphs)\n");
    printf("OK\n");
    return 0;
}
