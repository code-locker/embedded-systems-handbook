/*
 * 03_dfs.c — depth-first search: recursive and iterative, discovery/finish
 *            times, edge classification (tree / back / forward / cross)
 *
 * Concept : DFS goes as deep as it can, then backtracks. Colours track state:
 *           WHITE = not seen, GRAY = on the current recursion path (open),
 *           BLACK = finished. Stamping a clock on entry (d) and exit (f) gives
 *           the "parenthesis structure", and the colour of v when we look at
 *           edge u->v tells us what kind of edge it is. A GRAY target means a
 *           back edge, i.e. a CYCLE. The iterative version keeps an explicit
 *           stack of "where was I in u's neighbour list" and must reproduce
 *           the recursive times exactly.
 * Build   : make run P=dsa/13-graphs/c/03_dfs.c
 * Read    : ../README.md and ../notes/03-traversals.md  (section "DFS")
 *
 *   G2 (directed, 6 vertices):
 *
 *            3 -> 0 (closes the cycle 0->1->3->0)
 *         +-----------------------------+
 *         v                             |
 *         0 ---------> 1 -------------> 3
 *         | \          ^                ^
 *         |  +---------|----------------+   0 -> 3 (shortcut)
 *         v            |
 *         2 -----------+   2 -> 1
 *         ^
 *         |
 *         5 <--------- 4
 *
 *   edges: 0->1 0->2 0->3 1->3 2->1 3->0 4->5 5->2
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

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

static void g_arc(Graph *g, int u, int v)
{
    if (g->len[u] == g->cap[u]) {
        g->cap[u] = g->cap[u] ? 2 * g->cap[u] : 4;
        int *p = realloc(g->adj[u], sizeof(int) * (size_t)g->cap[u]);
        CHECK(p != NULL);
        g->adj[u] = p;
    }
    g->adj[u][g->len[u]++] = v;
}

static void g_free(Graph *g)
{
    for (int u = 0; u < g->n; u++) free(g->adj[u]);
    free(g->adj); free(g->len); free(g->cap);
}

enum { WHITE, GRAY, BLACK };
enum { MAXN = 32, MAXE = 64 };

/* Everything one DFS run produces. Edge kinds are recorded per arc, in the
 * order the arcs are examined, so recursive and iterative can be compared. */
typedef struct {
    int  color[MAXN], d[MAXN], f[MAXN], parent[MAXN];
    int  time;
    int  ne;
    int  eu[MAXE], ev[MAXE];
    char kind[MAXE];          /* 'T' tree, 'B' back, 'F' forward, 'C' cross */
    char paren[256];          /* "(0 (1 ... 1) 0)" */
    int  plen;
} Dfs;

static void dfs_init(Dfs *s, int n)
{
    memset(s, 0, sizeof *s);
    for (int v = 0; v < n; v++) { s->color[v] = WHITE; s->parent[v] = -1; }
}

/* The colour of v at the moment we look along u->v decides the edge type:
 *   WHITE -> tree    (we are about to discover v through this edge)
 *   GRAY  -> back    (v is an ancestor still open: u->v closes a cycle)
 *   BLACK -> forward if d[u] < d[v] (v is a finished descendant of u)
 *            cross   otherwise      (v is in an already finished other branch) */
static char classify(const Dfs *s, int u, int v)
{
    if (s->color[v] == WHITE) return 'T';
    if (s->color[v] == GRAY)  return 'B';
    return s->d[u] < s->d[v] ? 'F' : 'C';
}

static void record(Dfs *s, int u, int v, char k)
{
    s->eu[s->ne] = u; s->ev[s->ne] = v; s->kind[s->ne] = k; s->ne++;
}

/* Recursive DFS: O(V + E). Recursion depth can reach V — on a 1e6-vertex path
 * graph that overflows an 8 MB stack (and easily a 4-16 KB kernel/RTOS stack).
 * That is the practical reason the iterative version exists. */
static void dfs_visit(const Graph *g, Dfs *s, int u)
{
    s->color[u] = GRAY;
    s->d[u] = ++s->time;
    s->plen += snprintf(s->paren + s->plen, sizeof s->paren - (size_t)s->plen, "(%d ", u);
    for (int i = 0; i < g->len[u]; i++) {
        int v = g->adj[u][i];
        char k = classify(s, u, v);
        record(s, u, v, k);
        if (k == 'T') {
            s->parent[v] = u;
            dfs_visit(g, s, v);
        }
    }
    s->color[u] = BLACK;
    s->f[u] = ++s->time;
    s->plen += snprintf(s->paren + s->plen, sizeof s->paren - (size_t)s->plen, "%d) ", u);
}

static void dfs_recursive(const Graph *g, Dfs *s)
{
    dfs_init(s, g->n);
    for (int u = 0; u < g->n; u++)       /* restart from every unvisited vertex */
        if (s->color[u] == WHITE)        /* -> a DFS *forest*, one tree per start */
            dfs_visit(g, s, u);
}

/*
 * Iterative DFS that behaves EXACTLY like the recursion.
 * The trick: the recursion's hidden state per frame is "u" and "i, the next
 * neighbour index". Keep both explicitly: stack[] holds u, next[u] holds i.
 *
 *   recursion frame          explicit stack
 *   +-------------+          stack: [0, 1, 3]     next[0]=1  (0->1 done)
 *   | u=3, i=0    |                               next[1]=1  (1->3 done)
 *   | u=1, i=1    |                               next[3]=0
 *   | u=0, i=1    |
 *   +-------------+
 *
 * (The popular shortcut "pop u, push all neighbours" gives a valid DFS ORDER
 *  but wrong finish times and visits neighbours in reverse — not equivalent.)
 */
static void dfs_iterative(const Graph *g, Dfs *s)
{
    dfs_init(s, g->n);
    int *stack = malloc(sizeof(int) * (size_t)g->n);   /* <= n open vertices */
    int *next  = calloc((size_t)g->n, sizeof(int));
    CHECK(stack && next);

    for (int root = 0; root < g->n; root++) {
        if (s->color[root] != WHITE) continue;
        int top = 0;
        stack[top++] = root;
        s->color[root] = GRAY;
        s->d[root] = ++s->time;
        s->plen += snprintf(s->paren + s->plen, sizeof s->paren - (size_t)s->plen, "(%d ", root);
        while (top > 0) {
            int u = stack[top - 1];               /* peek, do not pop yet */
            if (next[u] < g->len[u]) {
                int v = g->adj[u][next[u]++];     /* advance u's cursor   */
                char k = classify(s, u, v);
                record(s, u, v, k);
                if (k == 'T') {                   /* "recursive call"     */
                    s->parent[v] = u;
                    s->color[v] = GRAY;
                    s->d[v] = ++s->time;
                    s->plen += snprintf(s->paren + s->plen, sizeof s->paren - (size_t)s->plen, "(%d ", v);
                    stack[top++] = v;
                }
            } else {                              /* all neighbours done: "return" */
                top--;
                s->color[u] = BLACK;
                s->f[u] = ++s->time;
                s->plen += snprintf(s->paren + s->plen, sizeof s->paren - (size_t)s->plen, "%d) ", u);
            }
        }
    }
    free(stack);
    free(next);
}

static const char *kind_name(char k)
{
    switch (k) {
    case 'T': return "tree";
    case 'B': return "BACK  (cycle!)";
    case 'F': return "forward";
    default:  return "cross";
    }
}

int main(void)
{
    enum { N = 6 };
    Graph g = g_new(N);
    static const int E[][2] = { {0,1},{0,2},{0,3},{1,3},{2,1},{3,0},{4,5},{5,2} };
    for (size_t i = 0; i < sizeof E / sizeof E[0]; i++)
        g_arc(&g, E[i][0], E[i][1]);

    static Dfs rec, it;     /* static: ~1 KB each, keep them off the stack */
    dfs_recursive(&g, &rec);
    dfs_iterative(&g, &it);

    printf("Recursive DFS on G2 (directed)\n\n vertex | d (discover) | f (finish) | parent\n");
    for (int v = 0; v < N; v++)
        printf("   %d    |      %2d      |     %2d     |  %2d\n", v, rec.d[v], rec.f[v], rec.parent[v]);
    printf("\nparenthesis structure: %s\n", rec.paren);
    printf("\nedges in the order DFS examines them:\n");
    for (int i = 0; i < rec.ne; i++)
        printf("  %d -> %d : %s\n", rec.eu[i], rec.ev[i], kind_name(rec.kind[i]));

    /* Known answers (hand-traced in notes/03-traversals.md) */
    static const int D[N] = {1, 2, 6, 3, 9, 10};
    static const int F[N] = {8, 5, 7, 4, 12, 11};
    for (int v = 0; v < N; v++) { CHECK(rec.d[v] == D[v]); CHECK(rec.f[v] == F[v]); }
    CHECK(strcmp(rec.kind, "TTBTCFTC") == 0);   /* order: 0-1,1-3,3-0,0-2,2-1,0-3,4-5,5-2 */

    /* Iterative must match the recursion exactly. */
    for (int v = 0; v < N; v++) {
        CHECK(it.d[v] == rec.d[v]);
        CHECK(it.f[v] == rec.f[v]);
        CHECK(it.parent[v] == rec.parent[v]);
    }
    CHECK(it.ne == rec.ne && strcmp(it.kind, rec.kind) == 0);
    CHECK(strcmp(it.paren, rec.paren) == 0);
    printf("\niterative DFS (explicit stack + per-vertex cursor) == recursive DFS\n");

    /* White-path / parenthesis theorem: for any two vertices the intervals
     * [d,f] are either nested or disjoint — never partially overlapping. */
    for (int a = 0; a < N; a++)
        for (int b = 0; b < N; b++) {
            int nested   = rec.d[a] < rec.d[b] && rec.f[b] < rec.f[a];
            int disjoint = rec.f[a] < rec.d[b] || rec.f[b] < rec.d[a];
            CHECK(a == b || nested || disjoint || (rec.d[b] < rec.d[a] && rec.f[a] < rec.f[b]));
        }
    printf("parenthesis theorem holds for all pairs\n");

    /* Undirected G1 (each edge stored as two arcs): just the visit order. */
    Graph u = g_new(8);
    static const int E1[][2] = { {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7} };
    for (size_t i = 0; i < sizeof E1 / sizeof E1[0]; i++) {
        g_arc(&u, E1[i][0], E1[i][1]);
        g_arc(&u, E1[i][1], E1[i][0]);
    }
    static Dfs ud;
    dfs_recursive(&u, &ud);
    int order[8];
    int k = 0;
    for (int t = 1; t <= 16; t++)
        for (int v = 0; v < 8; v++)
            if (ud.d[v] == t) order[k++] = v;
    printf("\nG1 (undirected) DFS visit order from 0:");
    for (int i = 0; i < 8; i++) printf(" %d", order[i]);
    printf("\n");
    static const int expect_order[8] = {0, 1, 2, 5, 7, 6, 4, 3};
    for (int i = 0; i < 8; i++) CHECK(order[i] == expect_order[i]);

    g_free(&u);
    g_free(&g);
    printf("OK\n");
    return 0;
}
