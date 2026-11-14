/*
 * 02_bfs.c — breadth-first search: levels, shortest paths in unweighted graphs
 *
 * Concept : BFS explores in rings (level 0, then all vertices 1 hop away, then
 *           2 hops...). Because a FIFO queue processes vertices in
 *           non-decreasing distance, the FIRST time a vertex is discovered is
 *           along a shortest path. Prints the queue / dist / parent state after
 *           every dequeue, rebuilds a path from parent[], and cross-checks BFS
 *           distances against a brute-force relaxation on random graphs.
 * Build   : make run P=dsa/13-graphs/c/02_bfs.c
 * Read    : ../README.md and ../notes/03-traversals.md  (section "BFS")
 *
 *   G1:   0 ----- 1 ----- 2
 *         |       |       |
 *         3 ----- 4       5
 *                 |       |
 *                 6 ----- 7
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

/* ---- adjacency list with growable arrays (same idea as linked lists, but
 *      neighbours are contiguous => fewer cache misses). Owner: g_new/g_free. */
typedef struct {
    int   n;
    int  *len, *cap;
    int **adj;
} Graph;

static Graph g_new(int n)
{
    Graph g = { n, calloc((size_t)n, sizeof(int)), calloc((size_t)n, sizeof(int)),
                calloc((size_t)n, sizeof(int *)) };
    CHECK(g.len && g.cap && g.adj);
    return g;
}

static void g_arc(Graph *g, int u, int v)        /* amortised O(1) append */
{
    if (g->len[u] == g->cap[u]) {
        g->cap[u] = g->cap[u] ? 2 * g->cap[u] : 4;
        int *p = realloc(g->adj[u], sizeof(int) * (size_t)g->cap[u]);
        CHECK(p != NULL);                         /* never lose the old block */
        g->adj[u] = p;
    }
    g->adj[u][g->len[u]++] = v;
}

static void g_edge(Graph *g, int u, int v) { g_arc(g, u, v); g_arc(g, v, u); }

static void g_free(Graph *g)
{
    for (int u = 0; u < g->n; u++)
        free(g->adj[u]);
    free(g->adj); free(g->len); free(g->cap);
}

/*
 * bfs — O(V + E): each vertex is enqueued at most once (we mark it when it is
 * DISCOVERED, not when it is dequeued), and each adjacency list is scanned once.
 *
 * Why mark on discovery? If we marked on dequeue, a vertex could be pushed
 * many times (once per neighbour already in the queue) — still correct, but
 * the queue can grow to O(E) and work is wasted.
 *
 * dist[] and parent[] are caller-owned arrays of n ints.
 */
static void bfs(const Graph *g, int src, int *dist, int *parent, int trace)
{
    int *queue = malloc(sizeof(int) * (size_t)g->n);   /* each vertex once => n slots */
    CHECK(queue != NULL);
    int head = 0, tail = 0;

    for (int v = 0; v < g->n; v++) { dist[v] = -1; parent[v] = -1; }
    dist[src] = 0;
    queue[tail++] = src;

    if (trace)
        printf("step | pop | newly discovered | queue after    | dist[0..7]\n"
               "-----+-----+------------------+----------------+------------------\n");
    int step = 0;
    while (head < tail) {
        int u = queue[head++];
        char found[64]; int flen = 0;
        found[0] = '\0';
        for (int i = 0; i < g->len[u]; i++) {
            int v = g->adj[u][i];
            if (dist[v] != -1)
                continue;                 /* already discovered: never improve */
            dist[v] = dist[u] + 1;        /* one more hop than its discoverer  */
            parent[v] = u;
            queue[tail++] = v;
            flen += snprintf(found + flen, sizeof found - (size_t)flen, "%d(d=%d) ", v, dist[v]);
        }
        if (trace) {
            printf("%4d | %3d | %-16s | ", ++step, u, flen ? found : "-");
            int w = 0;
            for (int k = head; k < tail; k++)
                w += printf("%d ", queue[k]);
            printf("%*s| ", 15 - w, "");
            for (int v = 0; v < g->n; v++)
                if (dist[v] < 0) printf(". "); else printf("%d ", dist[v]);
            printf("\n");
        }
    }
    free(queue);
}

/* Walk parent[] back from dst to the source, then print reversed. O(path). */
static int print_path(const int *parent, int dst, int *out)
{
    int len = 0;
    for (int v = dst; v != -1; v = parent[v])
        out[len++] = v;
    printf("path to %d:", dst);
    for (int i = len - 1; i >= 0; i--)
        printf(" %d%s", out[i], i ? " ->" : "\n");
    return len;
}

/* ---- brute force for the cross-check: Bellman-Ford with every weight = 1.
 *      O(V * E) — slow but obviously correct. */
static void brute_dist(const Graph *g, int src, int *dist)
{
    for (int v = 0; v < g->n; v++) dist[v] = INT_MAX;
    dist[src] = 0;
    for (int round = 0; round < g->n; round++)
        for (int u = 0; u < g->n; u++)
            if (dist[u] != INT_MAX)
                for (int i = 0; i < g->len[u]; i++)
                    if (dist[u] + 1 < dist[g->adj[u][i]])
                        dist[g->adj[u][i]] = dist[u] + 1;
    for (int v = 0; v < g->n; v++)
        if (dist[v] == INT_MAX) dist[v] = -1;
}

static unsigned rng_state = 12345u;
static unsigned rnd(void)                    /* xorshift32: deterministic */
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    enum { N = 8 };
    Graph g = g_new(N);
    static const int E[][2] = { {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7} };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
        g_edge(&g, E[i][0], E[i][1]);

    int dist[N], parent[N], path[N];
    printf("BFS on G1 from vertex 0\n\n");
    bfs(&g, 0, dist, parent, 1);

    static const int expect[N] = {0, 1, 2, 1, 2, 3, 3, 4};
    for (int v = 0; v < N; v++)
        CHECK(dist[v] == expect[v]);

    printf("\nlevels: ");
    for (int d = 0; d <= 4; d++) {
        printf("L%d={", d);
        for (int v = 0, first = 1; v < N; v++)
            if (dist[v] == d) { printf(first ? "%d" : ",%d", v); first = 0; }
        printf("} ");
    }
    printf("\nparent: ");
    for (int v = 0; v < N; v++) printf("%d ", parent[v]);
    printf("\n");
    int len = print_path(parent, 7, path);
    CHECK(len - 1 == dist[7]);          /* edges on path = BFS distance */
    CHECK(path[len - 1] == 0);

    /* Random cross-check: 200 graphs, BFS vs brute force, including
     * disconnected ones (dist -1 must agree too). */
    for (int t = 0; t < 200; t++) {
        int n = 2 + (int)(rnd() % 12);
        Graph r = g_new(n);
        int m = (int)(rnd() % (unsigned)(n * 2));
        for (int i = 0; i < m; i++)
            g_edge(&r, (int)(rnd() % (unsigned)n), (int)(rnd() % (unsigned)n));
        int d1[16], d2[16], p1[16];
        int s = (int)(rnd() % (unsigned)n);
        bfs(&r, s, d1, p1, 0);
        brute_dist(&r, s, d2);
        for (int v = 0; v < n; v++)
            CHECK(d1[v] == d2[v]);
        g_free(&r);
    }
    printf("random cross-check: 200 graphs, BFS == brute force\n");

    g_free(&g);
    printf("OK\n");
    return 0;
}
