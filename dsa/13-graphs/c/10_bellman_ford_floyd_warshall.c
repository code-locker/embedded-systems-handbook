/*
 * 10_bellman_ford_floyd_warshall.c — shortest paths WITH negative edges:
 *     Bellman-Ford (single source, finds negative cycles) and
 *     Floyd-Warshall (all pairs, dynamic programming over "via vertices")
 *
 * Concept : Bellman-Ford: a shortest path uses at most V-1 edges, so relaxing
 *           EVERY edge V-1 times must find it (pass k fixes all paths of k
 *           edges). If a V-th pass still improves something, a negative cycle
 *           is reachable: "shortest" is then -infinity.
 *           Floyd-Warshall: d[i][j] after step k = shortest i->j path whose
 *           intermediate vertices are all in {0..k}. Adding vertex k as a
 *           possible stop: d[i][j] = min(d[i][j], d[i][k] + d[k][j]).
 *           A negative d[i][i] at the end means i is on a negative cycle.
 *           Cross-checked against each other on random graphs.
 * Build   : make run P=dsa/13-graphs/c/10_bellman_ford_floyd_warshall.c
 * Read    : ../README.md and ../notes/06-shortest-paths.md
 *
 *   G7 (CLRS fig. 24.4 relabelled s=0 t=1 y=2 x=3 z=4), negative arcs marked *:
 *     0->1:6  0->2:7  1->2:8  1->3:5  1->4:-4*  3->1:-2*  2->3:-3*  2->4:9
 *     4->0:2  4->3:7                  (drawn in ../notes/06-shortest-paths.md)
 *
 *   G8 (Floyd-Warshall, 4 vertices):
 *     0->1:3  0->3:7  1->0:8  1->2:2  2->0:5  2->3:1  3->0:2
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16, MAXM = 64 };
#define INF (INT_MAX / 4)    /* INF + INF must not overflow int (UB!) */

typedef struct { int u, v, w; } Edge;

/* --------------------------------------------------------- Bellman-Ford == */
/*
 * O(V * E). Edge list is the natural input: BF just loops over all edges.
 * Returns 1 if a negative cycle is reachable from src; then *cyc_v is a vertex
 * ON that cycle (found by walking parent[] V times from a vertex that still
 * relaxed: after V steps we must be inside the cycle).
 */
static int bellman_ford(int n, const Edge *e, int m, int src, int *dist, int *parent,
                        int *cyc_v, int trace)
{
    for (int v = 0; v < n; v++) { dist[v] = INF; parent[v] = -1; }
    dist[src] = 0;
    for (int pass = 1; pass <= n - 1; pass++) {
        int changed = 0;
        for (int i = 0; i < m; i++)
            if (dist[e[i].u] != INF && dist[e[i].u] + e[i].w < dist[e[i].v]) {
                dist[e[i].v] = dist[e[i].u] + e[i].w;
                parent[e[i].v] = e[i].u;
                changed = 1;
            }
        if (trace) {
            printf("  after pass %d: ", pass);
            for (int v = 0; v < n; v++) { if (dist[v] == INF) printf(" inf"); else printf("%4d", dist[v]); }
            printf("%s\n", changed ? "" : "   (no change: stop early)");
        }
        if (!changed) return 0;           /* nothing moved: all final already */
    }
    for (int i = 0; i < m; i++)           /* the V-th pass: detection */
        if (dist[e[i].u] != INF && dist[e[i].u] + e[i].w < dist[e[i].v]) {
            int x = e[i].v;
            parent[x] = e[i].u;
            for (int k = 0; k < n; k++) x = parent[x];   /* step back into the cycle */
            *cyc_v = x;
            return 1;
        }
    return 0;
}

/* ------------------------------------------------------- Floyd-Warshall == */
/* O(V^3) time, O(V^2) space. Loop order matters: k MUST be the outer loop. */
static void floyd_warshall(int n, int d[MAXN][MAXN], int trace)
{
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (d[i][k] != INF && d[k][j] != INF && d[i][k] + d[k][j] < d[i][j])
                    d[i][j] = d[i][k] + d[k][j];
        if (trace) {
            printf("  after k=%d (paths may stop at 0..%d):\n", k, k);
            for (int i = 0; i < n; i++) {
                printf("    ");
                for (int j = 0; j < n; j++) { if (d[i][j] == INF) printf(" inf"); else printf("%4d", d[i][j]); }
                printf("\n");
            }
        }
    }
}

static unsigned rng_state = 65537u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    int dist[MAXN], parent[MAXN], cv = -1;

    /* ---- 1. Bellman-Ford with negative edges, no negative cycle ---- */
    static const Edge G7[] = { {0,1,6},{0,2,7},{1,2,8},{1,3,5},{1,4,-4},
                               {3,1,-2},{2,3,-3},{2,4,9},{4,0,2},{4,3,7} };
    printf("1) Bellman-Ford on G7 from 0 (edges relaxed in the listed order):\n");
    printf("                  d0  d1  d2  d3  d4\n");
    int neg = bellman_ford(5, G7, 10, 0, dist, parent, &cv, 1);
    CHECK(!neg);
    static const int expect[5] = {0, 2, 7, 4, -2};
    for (int v = 0; v < 5; v++) CHECK(dist[v] == expect[v]);
    printf("  shortest 0->4 = -2 via");
    {
        int p[8], k = 0;
        for (int v = 4; v != -1; v = parent[v]) p[k++] = v;
        for (int i = k - 1; i >= 0; i--) printf(" %d", p[i]);
        printf("\n");
    }

    /* ---- 2. negative cycle 1 -> 2 -> 3 -> 1 (total -1) ---- */
    static const Edge NC[] = { {0,1,1},{1,2,-1},{2,3,-1},{3,1,1},{3,4,2} };
    printf("\n2) Bellman-Ford, graph with cycle 1->2->3->1 of weight (-1)+(-1)+1 = -1:\n");
    neg = bellman_ford(5, NC, 5, 0, dist, parent, &cv, 1);
    CHECK(neg);
    printf("  pass V still relaxes -> NEGATIVE CYCLE:");
    { int x = cv; do { printf(" %d", x); x = parent[x]; } while (x != cv); printf(" %d (walked via parent, reversed)\n", cv); }
    CHECK(cv >= 1 && cv <= 3);

    /* ---- 3. Floyd-Warshall ---- */
    int d[MAXN][MAXN];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) d[i][j] = (i == j) ? 0 : INF;
    static const Edge G8[] = { {0,1,3},{0,3,7},{1,0,8},{1,2,2},{2,0,5},{2,3,1},{3,0,2} };
    for (int i = 0; i < 7; i++) d[G8[i].u][G8[i].v] = G8[i].w;
    printf("\n3) Floyd-Warshall on G8:\n");
    floyd_warshall(4, d, 1);
    static const int FW[4][4] = { {0,3,5,6}, {5,0,2,3}, {3,6,0,1}, {2,5,7,0} };
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) CHECK(d[i][j] == FW[i][j]);

    /* ---- 4. random cross-check: FW (all pairs) vs BF from every source ---- */
    int checked_neg = 0;
    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rnd() % 9), m = (int)(rnd() % (unsigned)(2 * n + 1));
        Edge e[MAXM];
        for (int i = 0; i < n * n; i++) d[i / n][i % n] = (i / n == i % n) ? 0 : INF;
        for (int i = 0; i < m; i++) {
            e[i].u = (int)(rnd() % (unsigned)n); e[i].v = (int)(rnd() % (unsigned)n);
            e[i].w = (int)(rnd() % 15) - 3;                 /* some negatives */
            if (e[i].w < d[e[i].u][e[i].v]) d[e[i].u][e[i].v] = e[i].w;
        }
        floyd_warshall(n, d, 0);
        for (int s = 0; s < n; s++) {
            int bf_neg = bellman_ford(n, e, m, s, dist, parent, &cv, 0);
            int fw_neg = 0;           /* a negative cycle reachable from s? */
            for (int v = 0; v < n; v++) if (d[s][v] != INF && d[v][v] < 0) fw_neg = 1;
            CHECK(bf_neg == fw_neg);
            checked_neg += bf_neg;
            if (!bf_neg) for (int v = 0; v < n; v++) CHECK(dist[v] == d[s][v]);
        }
    }
    printf("\nrandom cross-check: 300 graphs with negative weights; Floyd-Warshall ==\n"
           "Bellman-Ford from every source (%d sources saw a negative cycle, both agreed)\n", checked_neg);
    CHECK(checked_neg > 0);
    printf("OK\n");
    return 0;
}
