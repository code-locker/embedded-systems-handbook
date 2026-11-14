/*
 * 07_bridges_articulation.c — bridges and articulation points with Tarjan's
 *                             low-link values, in one DFS
 *
 * Concept : disc[u] = when DFS first reached u. low[u] = the earliest disc
 *           reachable from u's DFS subtree using tree edges down and AT MOST
 *           ONE back edge up. If a child v cannot climb above u
 *           (low[v] >= disc[u]), removing u cuts v's subtree off: u is an
 *           articulation point (a single point of failure). If it cannot even
 *           reach u (low[v] > disc[u]), the edge u-v is a bridge.
 *           Root special case: the DFS root is a cut vertex iff it has >= 2
 *           DFS children. Brute-force cross-check: delete each vertex / edge
 *           and count components.
 * Build   : make run P=dsa/13-graphs/c/07_bridges_articulation.c
 * Read    : ../README.md and ../notes/04-connectivity.md  (section "Bridges")
 *
 *   G4 (undirected): two triangles joined by a 2-edge "bridge chain"
 *
 *     0               5
 *     | \           / |
 *     |   1 - 3 - 4   |      bridges: 1-3, 3-4
 *     | /           \ |      articulation points: 1, 3, 4
 *     2               6
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16, MAXM = 40 };

typedef struct { int to, id; } Arc;
typedef struct {
    int n, m;
    int eu[MAXM], ev[MAXM];
    int len[MAXN];
    Arc adj[MAXN][2 * MAXM];
} Graph;

static void g_init(Graph *g, int n) { memset(g, 0, sizeof *g); g->n = n; }
static void g_edge(Graph *g, int u, int v)
{
    int id = g->m++;
    g->eu[id] = u; g->ev[id] = v;
    g->adj[u][g->len[u]++] = (Arc){ v, id };
    g->adj[v][g->len[v]++] = (Arc){ u, id };
}

typedef struct {
    int disc[MAXN], low[MAXN], timer;
    int is_ap[MAXN];
    int is_bridge[MAXM];
} Tarjan;

static int imin(int a, int b) { return a < b ? a : b; }

/* O(V + E). in_edge = id of the tree edge we arrived on (skip only THAT edge,
 * so a doubled edge u=v correctly counts as a back edge -> not a bridge). */
static void tarjan_dfs(const Graph *g, Tarjan *t, int u, int in_edge, int trace)
{
    t->disc[u] = t->low[u] = t->timer++;
    int children = 0;
    for (int i = 0; i < g->len[u]; i++) {
        int v = g->adj[u][i].to, id = g->adj[u][i].id;
        if (id == in_edge) continue;
        if (t->disc[v] == -1) {                     /* tree edge: recurse */
            children++;
            tarjan_dfs(g, t, v, id, trace);
            t->low[u] = imin(t->low[u], t->low[v]); /* whatever v can reach, u can */
            if (in_edge != -1 && t->low[v] >= t->disc[u]) t->is_ap[u] = 1;
            if (t->low[v] > t->disc[u])                 t->is_bridge[id] = 1;
            if (trace)
                printf("  back at %d from child %d: low[%d]=%d vs disc[%d]=%d -> %s%s\n",
                       u, v, v, t->low[v], u, t->disc[u],
                       t->low[v] >= t->disc[u] ? (in_edge == -1 ? "(root: count children)" : "AP")
                                               : "not AP",
                       t->low[v] > t->disc[u] ? ", BRIDGE" : "");
        } else {                                    /* back edge: one hop up allowed */
            t->low[u] = imin(t->low[u], t->disc[v]);
        }
    }
    if (in_edge == -1 && children >= 2) t->is_ap[u] = 1;   /* root rule */
}

static void tarjan(const Graph *g, Tarjan *t, int trace)
{
    memset(t, 0, sizeof *t);
    for (int v = 0; v < g->n; v++) t->disc[v] = -1;
    for (int s = 0; s < g->n; s++)
        if (t->disc[s] == -1) tarjan_dfs(g, t, s, -1, trace);
}

/* ---- brute force: components with one vertex or one edge removed. O(V*(V+E)) */
static int count_components(const Graph *g, int skip_v, int skip_e)
{
    int seen[MAXN] = {0}, stack[MAXN], c = 0;
    for (int s = 0; s < g->n; s++) {
        if (s == skip_v || seen[s]) continue;
        int top = 0; stack[top++] = s; seen[s] = 1; c++;
        while (top) {
            int u = stack[--top];
            for (int i = 0; i < g->len[u]; i++) {
                int v = g->adj[u][i].to;
                if (v == skip_v || g->adj[u][i].id == skip_e || seen[v]) continue;
                seen[v] = 1; stack[top++] = v;
            }
        }
    }
    return c;
}

static void brute_check(const Graph *g, const Tarjan *t)
{
    int base = count_components(g, -1, -1);
    for (int v = 0; v < g->n; v++) {
        /* removing v removes one component if v was isolated: compare fairly */
        int isolated = g->len[v] == 0;
        int after = count_components(g, v, -1);
        int ap = after > base - isolated;
        CHECK(ap == t->is_ap[v]);
    }
    for (int e = 0; e < g->m; e++) {
        int bridge = g->eu[e] != g->ev[e] && count_components(g, -1, e) > base;
        CHECK(bridge == t->is_bridge[e]);
    }
}

static unsigned rng_state = 31337u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static Graph g;
    static Tarjan t;
    g_init(&g, 7);
    static const int E[][2] = { {0,1},{0,2},{1,2},{1,3},{3,4},{4,5},{4,6},{5,6} };
    for (int i = 0; i < 8; i++) g_edge(&g, E[i][0], E[i][1]);

    printf("Tarjan low-link on G4, DFS from 0:\n");
    tarjan(&g, &t, 1);
    printf("\n vertex : ");
    for (int v = 0; v < 7; v++) printf("%2d ", v);
    printf("\n disc   : ");
    for (int v = 0; v < 7; v++) printf("%2d ", t.disc[v]);
    printf("\n low    : ");
    for (int v = 0; v < 7; v++) printf("%2d ", t.low[v]);
    printf("\n\narticulation points:");
    for (int v = 0; v < 7; v++) if (t.is_ap[v]) printf(" %d", v);
    printf("\nbridges:");
    for (int e = 0; e < g.m; e++) if (t.is_bridge[e]) printf(" %d-%d", g.eu[e], g.ev[e]);
    printf("\n");

    static const int disc[7] = {0, 1, 2, 3, 4, 5, 6}, low[7] = {0, 0, 0, 3, 4, 4, 4};
    static const int ap[7] = {0, 1, 0, 1, 1, 0, 0};
    for (int v = 0; v < 7; v++) { CHECK(t.disc[v] == disc[v]); CHECK(t.low[v] == low[v]); CHECK(t.is_ap[v] == ap[v]); }
    for (int e = 0; e < g.m; e++) CHECK(t.is_bridge[e] == ((e == 3) || (e == 4)));   /* 1-3, 3-4 */
    brute_check(&g, &t);

    /* A doubled edge is never a bridge: add a second 3-4 cable. */
    g_edge(&g, 3, 4);
    tarjan(&g, &t, 0);
    CHECK(!t.is_bridge[4] && !t.is_bridge[8] && t.is_bridge[3]);
    printf("\nadd a parallel 3-4 edge (redundant link): 3-4 is no longer a bridge, 1-3 still is\n");

    for (int r = 0; r < 500; r++) {
        int n = 1 + (int)(rnd() % 12), m = (int)(rnd() % (unsigned)(2 * n));
        g_init(&g, n);
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (a != b) g_edge(&g, a, b);        /* parallel edges allowed */
        }
        tarjan(&g, &t, 0);
        brute_check(&g, &t);
    }
    printf("random cross-check: 500 multigraphs, Tarjan == delete-and-count brute force\n");
    printf("OK\n");
    return 0;
}
