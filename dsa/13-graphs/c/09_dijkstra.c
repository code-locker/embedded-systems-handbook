/*
 * 09_dijkstra.c — Dijkstra's shortest paths with a hand-written binary heap
 *                 (lazy deletion), and a demo of why negative edges break it
 *
 * Concept : Grow a "settled" set outward from the source like water filling
 *           pipes: always settle the unsettled vertex with the smallest
 *           tentative distance. With non-negative weights no later path can
 *           undercut it (every detour only adds length), so it is final.
 *           The heap holds (dist, vertex) pairs; instead of "decrease-key" we
 *           push a new pair and SKIP stale ones when popped (lazy deletion).
 *           One negative edge breaks the "detours only add" argument: the demo
 *           shows a settled vertex that later has a shorter path.
 * Build   : make run P=dsa/13-graphs/c/09_dijkstra.c
 * Read    : ../README.md and ../notes/06-shortest-paths.md  (section "Dijkstra")
 *
 *   G6 (directed, weighted; think one-way streets, weight = minutes):
 *
 *              4            1
 *        0 --------> 1 --------> 3
 *        |           ^         ^ | \ 6
 *      1 |         2 |       5/  |3 \
 *        v           |       /   v   v
 *        2 ----------+------+    4 -> 5
 *        |                       ^  2
 *        +----------- 8 ---------+
 *
 *   arcs: 0->1:4  0->2:1  2->1:2  1->3:1  2->3:5  2->4:8  3->4:3  3->5:6  4->5:2
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16, MAXDEG = 40 };
#define INF INT_MAX

typedef struct { int to, w; } Arc;
typedef struct {
    int n;
    int len[MAXN];
    Arc adj[MAXN][MAXDEG];
} Graph;

static void g_init(Graph *g, int n) { memset(g, 0, sizeof *g); g->n = n; }
static void g_arc(Graph *g, int u, int v, int w)
{
    CHECK(g->len[u] < MAXDEG);
    g->adj[u][g->len[u]++] = (Arc){ v, w };
}

/* ------------------------------------------------ binary min-heap of pairs */
typedef struct { int d, v; } Item;
typedef struct { Item *a; int len, cap; } Heap;   /* a: owned, freed by heap_free */

static void heap_free(Heap *h) { free(h->a); h->a = NULL; h->len = h->cap = 0; }

static void heap_push(Heap *h, int d, int v)      /* O(log n) sift-up */
{
    if (h->len == h->cap) {
        h->cap = h->cap ? 2 * h->cap : 8;
        Item *p = realloc(h->a, sizeof(Item) * (size_t)h->cap);
        CHECK(p != NULL);
        h->a = p;
    }
    int i = h->len++;
    h->a[i] = (Item){ d, v };
    while (i > 0 && h->a[(i - 1) / 2].d > h->a[i].d) {
        Item t = h->a[i]; h->a[i] = h->a[(i - 1) / 2]; h->a[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static Item heap_pop(Heap *h)                     /* O(log n) sift-down */
{
    Item top = h->a[0];
    h->a[0] = h->a[--h->len];
    int i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < h->len && h->a[l].d < h->a[s].d) s = l;
        if (r < h->len && h->a[r].d < h->a[s].d) s = r;
        if (s == i) break;
        Item t = h->a[i]; h->a[i] = h->a[s]; h->a[s] = t;
        i = s;
    }
    return top;
}

static int cmp_item(const void *x, const void *y)
{
    const Item *a = x, *b = y;
    return a->d != b->d ? (a->d > b->d) - (a->d < b->d) : a->v - b->v;
}

static void print_heap(const Heap *h)   /* print a sorted COPY: heap order is not sorted */
{
    Item tmp[64];
    int k = h->len < 64 ? h->len : 64;
    memcpy(tmp, h->a, sizeof(Item) * (size_t)k);
    qsort(tmp, (size_t)k, sizeof(Item), cmp_item);
    for (int i = 0; i < k; i++) printf("(%d,%d)", tmp[i].d, tmp[i].v);
}

/*
 * dijkstra — O((V + E) log V). Each successful relaxation pushes one item, so
 * the heap holds at most E items; each push/pop costs O(log E) = O(log V).
 * Requires: all weights >= 0.
 */
static void dijkstra(const Graph *g, int src, int *dist, int *parent, int trace)
{
    Heap h = {0};
    for (int v = 0; v < g->n; v++) { dist[v] = INF; parent[v] = -1; }
    dist[src] = 0;
    heap_push(&h, 0, src);
    if (trace) printf(" pop (d,v) | action                          | dist[0..5]              | heap (d,v)\n");
    while (h.len > 0) {
        Item it = heap_pop(&h);
        int u = it.v;
        char act[64] = "";
        int alen = 0;
        if (it.d > dist[u]) {
            /* Stale: a shorter path to u was found after this item was pushed. */
            alen = snprintf(act, sizeof act, "stale, skip");
        } else {
            for (int i = 0; i < g->len[u]; i++) {
                int v = g->adj[u][i].to, nd = dist[u] + g->adj[u][i].w;
                if (nd < dist[v]) {                      /* relax u->v */
                    dist[v] = nd;
                    parent[v] = u;
                    heap_push(&h, nd, v);
                    alen += snprintf(act + alen, sizeof act - (size_t)alen, "d[%d]=%d ", v, nd);
                }
            }
            if (alen == 0) snprintf(act, sizeof act, "settle, nothing improves");
        }
        if (trace) {
            printf("  (%2d,%d)   | %-31s | ", it.d, u, act);
            for (int v = 0; v < g->n; v++) {
                if (dist[v] == INF) printf("inf ");
                else printf("%-3d ", dist[v]);
            }
            printf("| ");
            print_heap(&h);
            printf("\n");
        }
    }
    heap_free(&h);
}

/* The textbook variant that marks vertices "done" and never touches them
 * again. Identical to the above for weights >= 0; shows the failure clearly. */
static void dijkstra_settled(const Graph *g, int src, int *dist)
{
    int done[MAXN] = {0};
    for (int v = 0; v < g->n; v++) dist[v] = INF;
    dist[src] = 0;
    for (int round = 0; round < g->n; round++) {
        int u = -1;
        for (int v = 0; v < g->n; v++)               /* O(V) scan: fine for tiny n */
            if (!done[v] && dist[v] != INF && (u == -1 || dist[v] < dist[u])) u = v;
        if (u == -1) break;
        done[u] = 1;
        for (int i = 0; i < g->len[u]; i++) {
            int v = g->adj[u][i].to;
            if (!done[v] && dist[u] + g->adj[u][i].w < dist[v]) dist[v] = dist[u] + g->adj[u][i].w;
        }
    }
}

static void bellman_ford(const Graph *g, int src, int *dist)   /* O(V*E) reference */
{
    for (int v = 0; v < g->n; v++) dist[v] = INF;
    dist[src] = 0;
    for (int r = 0; r < g->n - 1; r++)
        for (int u = 0; u < g->n; u++)
            if (dist[u] != INF)
                for (int i = 0; i < g->len[u]; i++)
                    if (dist[u] + g->adj[u][i].w < dist[g->adj[u][i].to])
                        dist[g->adj[u][i].to] = dist[u] + g->adj[u][i].w;
}

static unsigned rng_state = 8086u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static Graph g;
    g_init(&g, 6);
    static const int E[][3] = { {0,1,4},{0,2,1},{2,1,2},{1,3,1},{2,3,5},{2,4,8},{3,4,3},{3,5,6},{4,5,2} };
    for (int i = 0; i < 9; i++) g_arc(&g, E[i][0], E[i][1], E[i][2]);

    int dist[MAXN], parent[MAXN];
    printf("Dijkstra on G6 from 0 (lazy-deletion binary heap):\n\n");
    dijkstra(&g, 0, dist, parent, 1);

    static const int expect[6] = {0, 3, 1, 4, 7, 9};
    for (int v = 0; v < 6; v++) CHECK(dist[v] == expect[v]);
    int path[MAXN], len = 0;
    for (int v = 5; v != -1; v = parent[v]) path[len++] = v;
    printf("\nshortest 0 -> 5 = %d via", dist[5]);
    for (int i = len - 1; i >= 0; i--) printf(" %d", path[i]);
    printf("\n");
    CHECK(len == 6);   /* 0 2 1 3 4 5 */

    /* ---- negative edge: Dijkstra's "settled is final" promise is broken ---- */
    g_init(&g, 4);
    g_arc(&g, 0, 1, 2); g_arc(&g, 0, 2, 5); g_arc(&g, 2, 1, -4); g_arc(&g, 1, 3, 1);
    int dd[MAXN], bf[MAXN];
    dijkstra_settled(&g, 0, dd);
    bellman_ford(&g, 0, bf);
    printf("\nnegative edge 2->1 = -4 (arcs 0->1:2 0->2:5 2->1:-4 1->3:1):\n");
    printf("  Dijkstra    : d[1]=%d d[3]=%d   (settled 1 at distance 2, then 0->2->1 = 1 came too late)\n", dd[1], dd[3]);
    printf("  Bellman-Ford: d[1]=%d d[3]=%d   (correct)\n", bf[1], bf[3]);
    CHECK(dd[1] == 2 && dd[3] == 3 && bf[1] == 1 && bf[3] == 2);

    /* ---- random cross-check vs Bellman-Ford (non-negative weights) ---- */
    for (int t = 0; t < 400; t++) {
        int n = 1 + (int)(rnd() % 14), m = (int)(rnd() % (unsigned)(3 * n + 1));
        g_init(&g, n);
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (g.len[a] < MAXDEG) g_arc(&g, a, b, (int)(rnd() % 20));
        }
        int s = (int)(rnd() % (unsigned)n);
        dijkstra(&g, s, dist, parent, 0);
        bellman_ford(&g, s, bf);
        for (int v = 0; v < n; v++) CHECK(dist[v] == bf[v]);
    }
    printf("\nrandom cross-check: 400 graphs, Dijkstra == Bellman-Ford\n");
    printf("OK\n");
    return 0;
}
