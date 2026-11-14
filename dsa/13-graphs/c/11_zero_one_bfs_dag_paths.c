/*
 * 11_zero_one_bfs_dag_paths.c — two linear-time special cases of shortest
 *                               paths: 0-1 BFS (deque) and DAG shortest /
 *                               longest (critical) path
 *
 * Concept : 0-1 BFS: when every weight is 0 or 1, a deque replaces the heap.
 *             A 0-edge keeps you on the same "level" -> push_FRONT;
 *             a 1-edge goes one level deeper       -> push_BACK.
 *             The deque then always holds at most two distance values
 *             (d and d+1), in order — it IS a priority queue. O(V + E).
 *           DAG paths: process vertices in topological order; when we reach
 *             u, every path into u has been relaxed already, so dist[u] is
 *             final. One pass, O(V + E), and NEGATIVE weights are fine
 *             (no cycles => nothing can loop back and undercut).
 *             Longest path on a DAG = critical path of a task schedule.
 * Build   : make run P=dsa/13-graphs/c/11_zero_one_bfs_dag_paths.c
 * Read    : ../README.md and ../notes/06-shortest-paths.md
 *
 *   0-1 example: fewest one-way streets to REVERSE to drive from 0 to 5.
 *   Streets (one-way):  0->1  2->1  2->3  4->3  4->5  0->6  5->6
 *
 *        0 ---> 1 <--- 2 ---> 3 <--- 4 ---> 5
 *        |                                  |
 *        +-------------> 6 <----------------+
 *
 *   Driving along a street costs 0, against it (= reversing it) costs 1.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16, MAXDEG = 48 };
#define INF (INT_MAX / 4)

typedef struct { int to, w; } Arc;
typedef struct {
    int n;
    int len[MAXN];
    Arc adj[MAXN][MAXDEG];
} Graph;

static void g_init(Graph *g, int n) { memset(g, 0, sizeof *g); g->n = n; }
static void g_arc(Graph *g, int u, int v, int w) { CHECK(g->len[u] < MAXDEG); g->adj[u][g->len[u]++] = (Arc){ v, w }; }

/* ============================================================== 0-1 BFS == */
/*
 * Deque as a ring buffer of capacity CAP. A vertex can be pushed once per
 * improvement, at most E + 1 pushes in total, so CAP >= E + 1 is enough.
 */
enum { CAP = MAXN * MAXDEG + 1 };

static void zero_one_bfs(const Graph *g, int src, int *dist, int trace)
{
    static int dq[CAP];
    int head = 0, size = 0;
    for (int v = 0; v < g->n; v++) dist[v] = INF;
    dist[src] = 0;
    dq[head] = src; size = 1;
    while (size > 0) {
        int u = dq[head]; head = (head + 1) % CAP; size--;          /* pop front */
        for (int i = 0; i < g->len[u]; i++) {
            int v = g->adj[u][i].to, w = g->adj[u][i].w;
            if (dist[u] + w < dist[v]) {
                dist[v] = dist[u] + w;
                if (w == 0) { head = (head - 1 + CAP) % CAP; dq[head] = v; }   /* front */
                else        { dq[(head + size) % CAP] = v; }                    /* back  */
                size++;
            }
        }
        if (trace) {
            printf("  pop %d (d=%d) | deque: ", u, dist[u]);
            for (int k = 0; k < size; k++) { int x = dq[(head + k) % CAP]; printf("%d(d=%d) ", x, dist[x]); }
            printf("\n");
        }
    }
}

/* O(V^2) Dijkstra used only as the reference answer. */
static void dijkstra_ref(const Graph *g, int src, int *dist)
{
    int done[MAXN] = {0};
    for (int v = 0; v < g->n; v++) dist[v] = INF;
    dist[src] = 0;
    for (;;) {
        int u = -1;
        for (int v = 0; v < g->n; v++) if (!done[v] && dist[v] < INF && (u < 0 || dist[v] < dist[u])) u = v;
        if (u < 0) break;
        done[u] = 1;
        for (int i = 0; i < g->len[u]; i++)
            if (dist[u] + g->adj[u][i].w < dist[g->adj[u][i].to]) dist[g->adj[u][i].to] = dist[u] + g->adj[u][i].w;
    }
}

/* ====================================================== DAG shortest path == */
static void topo_dfs(const Graph *g, int u, int *seen, int *post, int *np)
{
    seen[u] = 1;
    for (int i = 0; i < g->len[u]; i++) if (!seen[g->adj[u][i].to]) topo_dfs(g, g->adj[u][i].to, seen, post, np);
    post[(*np)++] = u;
}

static void topo_order(const Graph *g, int *order)
{
    int seen[MAXN] = {0}, post[MAXN], np = 0;
    for (int u = 0; u < g->n; u++) if (!seen[u]) topo_dfs(g, u, seen, post, &np);
    for (int i = 0; i < np; i++) order[i] = post[np - 1 - i];
}

/* sign = +1: shortest paths. sign = -1: longest paths (negate, then negate back). */
static void dag_paths(const Graph *g, int src, int sign, int *dist, int *parent, int trace)
{
    int order[MAXN];
    topo_order(g, order);
    for (int v = 0; v < g->n; v++) { dist[v] = INF; parent[v] = -1; }
    dist[src] = 0;
    if (trace) { printf("  topological order:"); for (int i = 0; i < g->n; i++) printf(" %d", order[i]); printf("\n"); }
    for (int i = 0; i < g->n; i++) {
        int u = order[i];
        if (dist[u] == INF) continue;              /* not reachable from src (yet: ever) */
        for (int k = 0; k < g->len[u]; k++) {
            int v = g->adj[u][k].to, w = sign * g->adj[u][k].w;
            if (dist[u] + w < dist[v]) { dist[v] = dist[u] + w; parent[v] = u; }
        }
        if (trace) {
            printf("  relax out of %d: dist =", u);
            for (int v = 0; v < g->n; v++) { if (dist[v] == INF) printf(" inf"); else printf(" %d", sign * dist[v]); }
            printf("\n");
        }
    }
    if (sign < 0) for (int v = 0; v < g->n; v++) if (dist[v] != INF) dist[v] = -dist[v];
}

static void bellman_ref(const Graph *g, int src, int *dist)
{
    for (int v = 0; v < g->n; v++) dist[v] = INF;
    dist[src] = 0;
    for (int r = 0; r < g->n; r++)
        for (int u = 0; u < g->n; u++)
            if (dist[u] != INF)
                for (int i = 0; i < g->len[u]; i++)
                    if (dist[u] + g->adj[u][i].w < dist[g->adj[u][i].to]) dist[g->adj[u][i].to] = dist[u] + g->adj[u][i].w;
}

static unsigned rng_state = 1337u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static Graph g;
    int dist[MAXN], ref[MAXN], parent[MAXN];

    /* ---- 1. 0-1 BFS: minimum street reversals ---- */
    g_init(&g, 7);
    static const int S[][2] = { {0,1},{2,1},{2,3},{4,3},{4,5},{0,6},{5,6} };
    for (int i = 0; i < 7; i++) {
        g_arc(&g, S[i][0], S[i][1], 0);   /* with the street: free        */
        g_arc(&g, S[i][1], S[i][0], 1);   /* against it: reverse it, cost 1 */
    }
    printf("1) 0-1 BFS from 0 (0-edge -> push front, 1-edge -> push back):\n");
    zero_one_bfs(&g, 0, dist, 1);
    static const int expect01[7] = {0, 0, 1, 1, 2, 1, 0};
    for (int v = 0; v < 7; v++) CHECK(dist[v] == expect01[v]);
    printf("  reversals needed to reach 5: %d (drive 0->6, reverse 5->6)\n", dist[5]);

    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rnd() % 14), m = (int)(rnd() % (unsigned)(3 * n + 1));
        g_init(&g, n);
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (g.len[a] < MAXDEG) g_arc(&g, a, b, (int)(rnd() % 2));
        }
        zero_one_bfs(&g, 0, dist, 0);
        dijkstra_ref(&g, 0, ref);
        for (int v = 0; v < n; v++) CHECK(dist[v] == ref[v]);
    }
    printf("  random cross-check: 300 graphs, 0-1 BFS == Dijkstra\n");

    /* ---- 2. DAG shortest path with negative weights (CLRS fig. 24.5) ----
     *   r=0 s=1 t=2 x=3 y=4 z=5
     *        5      2      7     -1     -2
     *   r ----> s ----> t ----> x ----> y ----> z
     *   r->t:3  s->x:6  t->y:4  t->z:2  x->z:1
     */
    g_init(&g, 6);
    static const int D[][3] = { {0,1,5},{0,2,3},{1,2,2},{1,3,6},{2,3,7},{2,4,4},{2,5,2},{3,4,-1},{3,5,1},{4,5,-2} };
    for (int i = 0; i < 10; i++) g_arc(&g, D[i][0], D[i][1], D[i][2]);
    printf("\n2) DAG shortest paths from s=1 (negative edges allowed):\n");
    dag_paths(&g, 1, +1, dist, parent, 1);
    static const int expect_dag[6] = {INF, 0, 2, 6, 5, 3};
    for (int v = 0; v < 6; v++) CHECK(dist[v] == expect_dag[v]);

    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rnd() % 14), m = (int)(rnd() % (unsigned)(3 * n + 1));
        g_init(&g, n);
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (a == b) continue;
            if (a > b) { int x = a; a = b; b = x; }       /* only "forward" arcs: a DAG */
            if (g.len[a] < MAXDEG) g_arc(&g, a, b, (int)(rnd() % 21) - 10);
        }
        int s = (int)(rnd() % (unsigned)n);
        dag_paths(&g, s, +1, dist, parent, 0);
        bellman_ref(&g, s, ref);
        for (int v = 0; v < n; v++) CHECK(dist[v] == ref[v]);
    }
    printf("  random cross-check: 300 DAGs with negative weights, DAG-SP == Bellman-Ford\n");

    /* ---- 3. critical path: longest path in a task DAG ----
     * Building a board image. Arc weight = duration of the task the arc LEAVES.
     *
     *   fetch(2) -> config(1) -+-> uboot(5)  -+
     *                          +-> kernel(12) +-> package(3) -> done
     *                          +-> rootfs(8)  +
     */
    static const char *const task[] = { "fetch", "config", "uboot", "kernel", "rootfs", "package", "done" };
    static const int dur[] = { 2, 1, 5, 12, 8, 3, 0 };
    g_init(&g, 7);
    static const int T[][2] = { {0,1},{1,2},{1,3},{1,4},{2,5},{3,5},{4,5},{5,6} };
    for (int i = 0; i < 8; i++) g_arc(&g, T[i][0], T[i][1], dur[T[i][0]]);
    printf("\n3) critical path (LONGEST path in the task DAG; negate weights, run DAG-SP):\n");
    dag_paths(&g, 0, -1, dist, parent, 0);
    printf("  earliest start: ");
    for (int v = 0; v < 7; v++) printf("%s=%d ", task[v], dist[v]);
    printf("\n  minimum total time = %d, critical path:", dist[6]);
    int path[MAXN], len = 0;
    for (int v = 6; v != -1; v = parent[v]) path[len++] = v;
    for (int i = len - 1; i >= 0; i--) printf(" %s", task[path[i]]);
    printf("\n  (speeding up uboot or rootfs gains nothing: only the kernel build is critical)\n");
    CHECK(dist[6] == 18);
    CHECK(len == 5 && path[2] == 3);        /* fetch config kernel package done */

    printf("OK\n");
    return 0;
}
