/*
 * 05_cycles_and_bipartite.c — cycle detection (undirected and directed) and
 *                             the bipartite (2-colouring) check
 *
 * Concept : UNDIRECTED: during DFS, reaching an already-visited vertex through
 *           any edge other than the one we came in on means a cycle.
 *           DIRECTED: that is not enough (a visited vertex may be in another,
 *           finished branch). Only reaching a GRAY vertex — one still on the
 *           recursion stack — proves a cycle. Three colours, not two.
 *           BIPARTITE: BFS-colour alternately 0/1; an edge with both ends the
 *           same colour proves an odd cycle, so the graph is not bipartite.
 *           Each detector prints the cycle / conflict it finds and is
 *           cross-checked against a brute force on random graphs.
 * Build   : make run P=dsa/13-graphs/c/05_cycles_and_bipartite.c
 * Read    : ../README.md and ../notes/04-connectivity.md
 *
 *   G1 (undirected):              G2 (directed):
 *     0 ----- 1 ----- 2            0 -> 1 -> 3 -> 0   (cycle)
 *     |       |       |            0 -> 2 -> 1, 0 -> 3, 4 -> 5 -> 2
 *     3 ----- 4       5
 *             |       |
 *             6 ----- 7
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16 };

/* Small fixed-capacity adjacency list with an edge id per arc. The id lets the
 * undirected DFS skip exactly the edge it arrived on — skipping "the parent
 * vertex" instead would miss a cycle made of two parallel edges u=v. */
typedef struct { int to, id; } Arc;
typedef struct {
    int  n;
    int  len[MAXN];
    Arc  adj[MAXN][2 * MAXN * 2];   /* plenty for the tests below */
} Graph;

static void g_init(Graph *g, int n) { memset(g, 0, sizeof *g); g->n = n; }
static void g_arc(Graph *g, int u, int v, int id)
{
    CHECK(g->len[u] < (int)(sizeof g->adj[0] / sizeof g->adj[0][0]));
    g->adj[u][g->len[u]].to = v;
    g->adj[u][g->len[u]].id = id;
    g->len[u]++;
}

/* ===================================================== undirected cycle == */
static int u_parent[MAXN], u_visited[MAXN];

/* Returns the vertex where the cycle was closed (or -1); *other gets the
 * already-visited ancestor at the other end of the closing edge. */
static int ucycle_dfs(const Graph *g, int u, int in_edge, int *other)
{
    u_visited[u] = 1;
    for (int i = 0; i < g->len[u]; i++) {
        int v = g->adj[u][i].to;
        if (g->adj[u][i].id == in_edge)
            continue;                  /* the edge we came down: not a cycle */
        if (u_visited[v]) { *other = v; return u; }
        u_parent[v] = u;
        int r = ucycle_dfs(g, v, g->adj[u][i].id, other);
        if (r != -1) return r;
    }
    return -1;
}

/* Fills cyc[] with the cycle's vertices, returns its length (0 = acyclic). */
static int undirected_cycle(const Graph *g, int *cyc)
{
    memset(u_visited, 0, sizeof u_visited);
    for (int s = 0; s < g->n; s++) {
        if (u_visited[s]) continue;
        u_parent[s] = -1;
        int other = -1;
        int end = ucycle_dfs(g, s, -1, &other);
        if (end != -1) {
            /* In an undirected DFS a non-tree edge always joins a vertex to an
             * ANCESTOR, so walking parents from `end` reaches `other`. */
            int len = 0;
            for (int v = end; v != other; v = u_parent[v]) cyc[len++] = v;
            cyc[len++] = other;
            return len;
        }
    }
    return 0;
}

/* ======================================================= directed cycle == */
enum { WHITE, GRAY, BLACK };
static int color[MAXN], d_parent[MAXN];

static int dcycle_dfs(const Graph *g, int u, int *from)
{
    color[u] = GRAY;                      /* "I am on the current path" */
    for (int i = 0; i < g->len[u]; i++) {
        int v = g->adj[u][i].to;
        if (color[v] == GRAY) { *from = u; return v; }     /* back edge u->v */
        if (color[v] == WHITE) {
            d_parent[v] = u;
            int r = dcycle_dfs(g, v, from);
            if (r != -1) return r;
        }
        /* BLACK: finished, everything reachable from it was explored and it
         * found no cycle — revisiting it is safe and NOT a cycle. */
    }
    color[u] = BLACK;
    return -1;
}

static int directed_cycle(const Graph *g, int *cyc)
{
    memset(color, 0, sizeof color);
    for (int s = 0; s < g->n; s++) {
        if (color[s] != WHITE) continue;
        d_parent[s] = -1;
        int from = -1;
        int head = dcycle_dfs(g, s, &from);
        if (head != -1) {
            int tmp[MAXN], len = 0;           /* walk from -> ... -> head, then reverse */
            for (int v = from; v != head; v = d_parent[v]) tmp[len++] = v;
            tmp[len++] = head;
            for (int i = 0; i < len; i++) cyc[i] = tmp[len - 1 - i];
            return len;
        }
    }
    return 0;
}

/* ============================================================ bipartite == */
/* BFS 2-colouring. Returns 1 if bipartite (side[] filled), else 0 and the
 * conflicting edge in *ca, *cb. O(V + E). */
static int bipartite(const Graph *g, int *side, int *ca, int *cb, int trace)
{
    int queue[MAXN];
    for (int v = 0; v < g->n; v++) side[v] = -1;
    for (int s = 0; s < g->n; s++) {
        if (side[s] != -1) continue;
        int h = 0, t = 0;
        queue[t++] = s; side[s] = 0;
        while (h < t) {
            int u = queue[h++];
            for (int i = 0; i < g->len[u]; i++) {
                int v = g->adj[u][i].to;
                if (side[v] == -1) {
                    side[v] = 1 - side[u];        /* neighbours get the other side */
                    queue[t++] = v;
                    if (trace) printf("    colour %d := %c (neighbour of %d)\n", v, side[v] ? 'B' : 'A', u);
                } else if (side[v] == side[u]) {
                    *ca = u; *cb = v;             /* same colour on both ends */
                    return 0;
                }
            }
        }
    }
    return 1;
}

/* =============================================================== brute == */
static int brute_bipartite(int n, const unsigned char *m)
{
    for (unsigned mask = 0; mask < (1u << n); mask++) {   /* try all 2^n colourings */
        int ok = 1;
        for (int u = 0; u < n && ok; u++)
            for (int v = 0; v < n && ok; v++)
                if (m[u * n + v] && ((mask >> u) & 1) == ((mask >> v) & 1)) ok = 0;
        if (ok) return 1;
    }
    return 0;
}

/* directed cycle exists <=> some u reaches itself (transitive closure, O(n^3)) */
static int brute_directed_cycle(int n, const unsigned char *m)
{
    unsigned char r[MAXN * MAXN];
    memcpy(r, m, (size_t)(n * n));
    for (int k = 0; k < n; k++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (r[i * n + k] && r[k * n + j]) r[i * n + j] = 1;
    for (int u = 0; u < n; u++) if (r[u * n + u]) return 1;
    return 0;
}

/* undirected (multi)graph is a forest <=> m == n - components */
static int components(int n, const unsigned char *m)
{
    int comp[MAXN], c = 0, stack[MAXN];
    for (int v = 0; v < n; v++) comp[v] = -1;
    for (int s = 0; s < n; s++) {
        if (comp[s] != -1) continue;
        int top = 0; stack[top++] = s; comp[s] = c;
        while (top) {
            int u = stack[--top];
            for (int v = 0; v < n; v++)
                if (m[u * n + v] && comp[v] == -1) { comp[v] = c; stack[top++] = v; }
        }
        c++;
    }
    return c;
}

static unsigned rng_state = 777u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static void print_cycle(const char *sep, const int *c, int len)
{
    for (int i = 0; i < len; i++) printf("%d%s", c[i], sep);
    printf("%d\n", c[0]);
}

static void build_undirected(Graph *g, int n, const int (*e)[2], int m)
{
    g_init(g, n);
    for (int i = 0; i < m; i++) { g_arc(g, e[i][0], e[i][1], i); g_arc(g, e[i][1], e[i][0], i); }
}

int main(void)
{
    static Graph g;
    int cyc[MAXN];

    /* --- 1. undirected cycle --- */
    static const int G1[][2] = { {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7} };
    build_undirected(&g, 8, G1, 9);
    int len = undirected_cycle(&g, cyc);
    printf("1) undirected G1: cycle found: ");
    print_cycle(" - ", cyc, len);
    CHECK(len == 6);                       /* DFS 0,1,2,5,7,6,4 then 4-1 closes it */
    static const int expect_ucyc[6] = {4, 6, 7, 5, 2, 1};
    for (int i = 0; i < 6; i++) CHECK(cyc[i] == expect_ucyc[i]);

    static const int TREE[][2] = { {0,1},{0,3},{1,2},{1,4},{2,5},{4,6},{5,7} };  /* G1 minus 3-4, 6-7 */
    build_undirected(&g, 8, TREE, 7);
    CHECK(undirected_cycle(&g, cyc) == 0);
    printf("   G1 without edges 3-4 and 6-7 is a tree: no cycle (7 edges = n-1)\n");

    static const int MULTI[][2] = { {0,1},{1,0} };   /* two parallel edges = a 2-cycle */
    build_undirected(&g, 2, MULTI, 2);
    CHECK(undirected_cycle(&g, cyc) == 2);
    printf("   multigraph 0=1 (two parallel edges): cycle found (needs edge ids!)\n");

    /* --- 2. directed cycle --- */
    static const int G2[][2] = { {0,1},{0,2},{0,3},{1,3},{2,1},{3,0},{4,5},{5,2} };
    g_init(&g, 6);
    for (int i = 0; i < 8; i++) g_arc(&g, G2[i][0], G2[i][1], i);
    len = directed_cycle(&g, cyc);
    printf("\n2) directed G2: cycle found: ");
    print_cycle(" -> ", cyc, len);
    CHECK(len == 3 && cyc[0] == 0 && cyc[1] == 1 && cyc[2] == 3);

    g_init(&g, 6);
    for (int i = 0; i < 8; i++)
        if (!(G2[i][0] == 3 && G2[i][1] == 0)) g_arc(&g, G2[i][0], G2[i][1], i);
    CHECK(directed_cycle(&g, cyc) == 0);
    printf("   G2 without 3->0 is a DAG. Note 0->3 and 0->1->3 both reach 3:\n"
           "   a visited-but-BLACK vertex is NOT a cycle (two colours would say it is).\n");

    /* --- 3. bipartite --- */
    int side[MAXN], ca = -1, cb = -1;
    build_undirected(&g, 8, G1, 9);
    printf("\n3) bipartite check on G1 (BFS from 0):\n");
    CHECK(bipartite(&g, side, &ca, &cb, 1));
    printf("   G1 IS bipartite: A = {");
    for (int v = 0; v < 8; v++) if (!side[v]) printf(" %d", v);
    printf(" }  B = {");
    for (int v = 0; v < 8; v++) if (side[v]) printf(" %d", v);
    printf(" }\n");
    static const int expect_side[8] = {0, 1, 0, 1, 0, 1, 1, 0};
    for (int v = 0; v < 8; v++) CHECK(side[v] == expect_side[v]);

    static const int G1T[][2] = { {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7},{0,4} };
    build_undirected(&g, 8, G1T, 10);
    CHECK(!bipartite(&g, side, &ca, &cb, 0));
    printf("   G1 + edge 0-4: conflict on edge %d-%d (both side %c) -> odd cycle 0-1-4-0, NOT bipartite\n",
           ca, cb, side[ca] ? 'B' : 'A');
    CHECK(ca == 1 && cb == 4);

    /* --- 4. random cross-checks --- */
    for (int t = 0; t < 400; t++) {
        int n = 1 + (int)(rnd() % 10), m = (int)(rnd() % (unsigned)(2 * n));
        unsigned char um[MAXN * MAXN] = {0}, dm[MAXN * MAXN] = {0};
        int ue[40][2], de[40][2], um_edges = 0, dm_edges = 0;
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (a == b) continue;                                /* no self-loops here */
            if (!um[a * n + b]) { ue[um_edges][0] = a; ue[um_edges][1] = b; um_edges++; }
            um[a * n + b] = um[b * n + a] = 1;
            if (!dm[a * n + b]) { de[dm_edges][0] = a; de[dm_edges][1] = b; dm_edges++; }
            dm[a * n + b] = 1;
        }
        build_undirected(&g, n, (const int (*)[2])ue, um_edges);
        int has = undirected_cycle(&g, cyc) > 0;
        CHECK(has == (um_edges != n - components(n, um)));
        CHECK(bipartite(&g, side, &ca, &cb, 0) == brute_bipartite(n, um));

        g_init(&g, n);
        for (int i = 0; i < dm_edges; i++) g_arc(&g, de[i][0], de[i][1], i);
        CHECK((directed_cycle(&g, cyc) > 0) == brute_directed_cycle(n, dm));
    }
    printf("\nrandom cross-check: 400 graphs — undirected cycle, directed cycle and\n"
           "bipartite answers all match brute force\n");
    printf("OK\n");
    return 0;
}
