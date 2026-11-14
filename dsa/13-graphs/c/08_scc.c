/*
 * 08_scc.c — strongly connected components: Kosaraju (two passes) and
 *            Tarjan (one pass, low-link + stack)
 *
 * Concept : An SCC is a maximal set where every vertex reaches every other.
 *           Collapsing each SCC to one node always leaves a DAG (the
 *           "condensation").
 *           Kosaraju: pass 1 = DFS, record finish order. Pass 2 = DFS on the
 *             REVERSED graph in decreasing finish time; each tree is one SCC.
 *             (The latest-finishing vertex lies in a "source" SCC; reversing
 *             the edges turns it into a sink, so DFS cannot leak out of it.)
 *           Tarjan: one DFS; vertices stay on a stack until the root of their
 *             SCC (low[u] == disc[u]) finishes, then pop down to u.
 *           Both are cross-checked against reachability brute force.
 * Build   : make run P=dsa/13-graphs/c/08_scc.c
 * Read    : ../README.md and ../notes/04-connectivity.md  (section "SCC")
 *
 *   G5 (directed, 8 vertices, 3 SCCs):
 *
 *     +----------+          +----------+          +--------+
 *     | 0 -> 1   |          | 3 -> 4   |          | 6 <-> 7|
 *     |  ^   |   | --1->3-> |  ^   |   | --5->6-> |        |
 *     |   \  v   |          |   \  v   |          +--------+
 *     |     2    |          |     5    |
 *     +----------+          +----------+
 *       SCC A                 SCC B                 SCC C
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 16, MAXDEG = 40 };

typedef struct {
    int n;
    int len[MAXN];
    int adj[MAXN][MAXDEG];
} Graph;

static void g_init(Graph *g, int n) { memset(g, 0, sizeof *g); g->n = n; }
static void g_arc(Graph *g, int u, int v) { CHECK(g->len[u] < MAXDEG); g->adj[u][g->len[u]++] = v; }

/* ============================================================ Kosaraju == */
static void k_dfs1(const Graph *g, int u, int *seen, int *order, int *k)
{
    seen[u] = 1;
    for (int i = 0; i < g->len[u]; i++)
        if (!seen[g->adj[u][i]]) k_dfs1(g, g->adj[u][i], seen, order, k);
    order[(*k)++] = u;                      /* record at FINISH time */
}

static void k_dfs2(const Graph *gt, int u, int c, int *comp)
{
    comp[u] = c;
    for (int i = 0; i < gt->len[u]; i++)
        if (comp[gt->adj[u][i]] == -1) k_dfs2(gt, gt->adj[u][i], c, comp);
}

/* O(V + E) twice. Returns the number of SCCs; comp[v] = SCC id, numbered in
 * topological order of the condensation (A before B before C). */
static int kosaraju(const Graph *g, int *comp, int trace)
{
    static Graph gt;                        /* transpose: every arc reversed */
    g_init(&gt, g->n);
    for (int u = 0; u < g->n; u++)
        for (int i = 0; i < g->len[u]; i++) g_arc(&gt, g->adj[u][i], u);

    int seen[MAXN] = {0}, order[MAXN], k = 0;
    for (int u = 0; u < g->n; u++)
        if (!seen[u]) k_dfs1(g, u, seen, order, &k);
    if (trace) {
        printf("  pass 1 finish order : ");
        for (int i = 0; i < k; i++) printf("%d ", order[i]);
        printf("\n  pass 2 on transpose, in DECREASING finish time:\n");
    }
    for (int v = 0; v < g->n; v++) comp[v] = -1;
    int c = 0;
    for (int i = k - 1; i >= 0; i--) {
        int u = order[i];
        if (comp[u] != -1) continue;
        k_dfs2(&gt, u, c, comp);
        if (trace) {
            printf("    start %d -> SCC %d = {", u, c);
            for (int v = 0; v < g->n; v++) if (comp[v] == c) printf(" %d", v);
            printf(" }\n");
        }
        c++;
    }
    return c;
}

/* ============================================================== Tarjan == */
typedef struct {
    int disc[MAXN], low[MAXN], on_stack[MAXN], stack[MAXN], top, timer, ncomp;
    int *comp;
    int trace;
} TState;

static int imin(int a, int b) { return a < b ? a : b; }

static void t_dfs(const Graph *g, TState *s, int u)
{
    s->disc[u] = s->low[u] = s->timer++;
    s->stack[s->top++] = u; s->on_stack[u] = 1;
    for (int i = 0; i < g->len[u]; i++) {
        int v = g->adj[u][i];
        if (s->disc[v] == -1) {
            t_dfs(g, s, v);
            s->low[u] = imin(s->low[u], s->low[v]);
        } else if (s->on_stack[v]) {
            /* Only vertices still on the stack count: a finished SCC that is
             * already popped is a different component (cross edge into it). */
            s->low[u] = imin(s->low[u], s->disc[v]);
        }
    }
    if (s->low[u] == s->disc[u]) {          /* u is the root of an SCC: pop it */
        if (s->trace) printf("    low[%d]==disc[%d]==%d -> pop SCC %d = {", u, u, s->disc[u], s->ncomp);
        int v;
        do {
            v = s->stack[--s->top];
            s->on_stack[v] = 0;
            s->comp[v] = s->ncomp;
            if (s->trace) printf(" %d", v);
        } while (v != u);
        if (s->trace) printf(" }\n");
        s->ncomp++;
    }
}

/* Tarjan emits SCCs in REVERSE topological order (sinks first). */
static int tarjan_scc(const Graph *g, int *comp, int trace)
{
    static TState s;
    memset(&s, 0, sizeof s);
    s.comp = comp; s.trace = trace;
    for (int v = 0; v < g->n; v++) s.disc[v] = -1;
    for (int u = 0; u < g->n; u++)
        if (s.disc[u] == -1) t_dfs(g, &s, u);
    return s.ncomp;
}

/* ============================================================ brute force == */
/* u, v in the same SCC <=> u reaches v and v reaches u. */
static void reach(const Graph *g, unsigned char r[MAXN][MAXN])
{
    memset(r, 0, sizeof(unsigned char) * MAXN * MAXN);
    for (int u = 0; u < g->n; u++) {
        r[u][u] = 1;
        for (int i = 0; i < g->len[u]; i++) r[u][g->adj[u][i]] = 1;
    }
    for (int k = 0; k < g->n; k++)            /* Warshall transitive closure */
        for (int i = 0; i < g->n; i++)
            for (int j = 0; j < g->n; j++)
                if (r[i][k] && r[k][j]) r[i][j] = 1;
}

static void check_against_brute(const Graph *g, const int *comp)
{
    static unsigned char r[MAXN][MAXN];
    reach(g, r);
    for (int u = 0; u < g->n; u++)
        for (int v = 0; v < g->n; v++)
            CHECK((comp[u] == comp[v]) == (r[u][v] && r[v][u]));
}

static unsigned rng_state = 4242u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static Graph g;
    g_init(&g, 8);
    static const int E[][2] = { {0,1},{1,2},{2,0},{1,3},{3,4},{4,5},{5,3},{5,6},{6,7},{7,6} };
    for (int i = 0; i < 10; i++) g_arc(&g, E[i][0], E[i][1]);

    int kc[MAXN], tc[MAXN];
    printf("Kosaraju on G5:\n");
    int nk = kosaraju(&g, kc, 1);
    printf("\nTarjan on G5:\n");
    int nt = tarjan_scc(&g, tc, 1);

    CHECK(nk == 3 && nt == 3);
    static const int expect_k[8] = {0, 0, 0, 1, 1, 1, 2, 2};   /* A=0, B=1, C=2 */
    static const int expect_t[8] = {2, 2, 2, 1, 1, 1, 0, 0};   /* reversed topo */
    for (int v = 0; v < 8; v++) { CHECK(kc[v] == expect_k[v]); CHECK(tc[v] == expect_t[v]); }
    check_against_brute(&g, kc);
    check_against_brute(&g, tc);

    /* Condensation: one node per SCC, edge if any arc crosses SCCs. */
    unsigned char cond[3][3] = {{0}};
    for (int u = 0; u < 8; u++)
        for (int i = 0; i < g.len[u]; i++)
            if (kc[u] != kc[g.adj[u][i]]) cond[kc[u]][kc[g.adj[u][i]]] = 1;
    printf("\ncondensation DAG:");
    for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++)
        if (cond[a][b]) printf("  %c -> %c", 'A' + a, 'A' + b);
    printf("\n");
    CHECK(cond[0][1] && cond[1][2] && !cond[1][0] && !cond[2][1]);

    for (int t = 0; t < 500; t++) {
        int n = 1 + (int)(rnd() % 14), m = (int)(rnd() % (unsigned)(2 * n + 1));
        g_init(&g, n);
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (g.len[a] < MAXDEG) g_arc(&g, a, b);
        }
        int c1 = kosaraju(&g, kc, 0), c2 = tarjan_scc(&g, tc, 0);
        CHECK(c1 == c2);
        check_against_brute(&g, kc);
        check_against_brute(&g, tc);
    }
    printf("random cross-check: 500 digraphs, Kosaraju == Tarjan == mutual reachability\n");
    printf("OK\n");
    return 0;
}
