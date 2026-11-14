/*
 * 06_topological_sort.c — topological order: Kahn's algorithm (BFS on
 *                         in-degrees) and DFS reverse-postorder
 *
 * Concept : In a DAG, an edge u->v means "u must come before v". A topological
 *           order lists every vertex after all of its prerequisites.
 *           Kahn  : repeatedly output a vertex with in-degree 0, then delete
 *                   its outgoing edges. If vertices remain, there is a cycle.
 *           DFS   : a vertex finishes only after everything it points to has
 *                   finished, so REVERSE finishing order is topological.
 *           Examples: a make-style build graph and a GStreamer playback
 *           pipeline (state changes go sink -> source = reverse topo order).
 * Build   : make run P=dsa/13-graphs/c/06_topological_sort.c
 * Read    : ../README.md and ../notes/05-ordering.md
 *
 *   Build DAG (edge = "is needed by"):
 *
 *     config.h(3) --+--> main.o(1) --+
 *                   |       ^        |
 *     main.c(4) ----|-------+        +--> app(0)
 *                   v                |
 *     util.c(5) --> util.o(2) -------+
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 32, MAXDEG = 32 };

typedef struct {
    int n;
    int len[MAXN];
    int adj[MAXN][MAXDEG];
} Dag;

static void dag_init(Dag *g, int n) { memset(g, 0, sizeof *g); g->n = n; }
static void dag_arc(Dag *g, int u, int v)
{
    CHECK(g->len[u] < MAXDEG);
    g->adj[u][g->len[u]++] = v;
}

/*
 * Kahn — O(V + E). Returns how many vertices were output; < n means a cycle
 * (the vertices on/after the cycle never reach in-degree 0).
 */
static int kahn(const Dag *g, int *order, const char *const *name, int trace)
{
    int indeg[MAXN] = {0}, queue[MAXN], h = 0, t = 0, k = 0;
    for (int u = 0; u < g->n; u++)
        for (int i = 0; i < g->len[u]; i++)
            indeg[g->adj[u][i]]++;
    for (int u = 0; u < g->n; u++)
        if (indeg[u] == 0) queue[t++] = u;       /* no prerequisites: ready now */

    if (trace) {
        printf("  initial in-degree: ");
        for (int u = 0; u < g->n; u++) printf("%d ", indeg[u]);
        printf("\n  pop          | in-degree after      | queue after\n");
    }
    while (h < t) {
        int u = queue[h++];
        order[k++] = u;
        for (int i = 0; i < g->len[u]; i++) {
            int v = g->adj[u][i];
            if (--indeg[v] == 0)                 /* last prerequisite done */
                queue[t++] = v;
        }
        if (trace) {
            printf("  %d %-10s | ", u, name ? name[u] : "");
            for (int v = 0; v < g->n; v++) printf("%d ", indeg[v]);
            printf("%*s| ", 21 - 2 * g->n, "");
            for (int i = h; i < t; i++) printf("%d ", queue[i]);
            printf("\n");
        }
    }
    return k;
}

/* DFS-based: append u to post[] when it FINISHES; reverse at the end.
 * Returns 0 if a GRAY vertex was hit (cycle => no topological order). */
enum { WHITE, GRAY, BLACK };
static int dfs_topo_visit(const Dag *g, int u, int *color, int *post, int *np)
{
    color[u] = GRAY;
    for (int i = 0; i < g->len[u]; i++) {
        int v = g->adj[u][i];
        if (color[v] == GRAY) return 0;
        if (color[v] == WHITE && !dfs_topo_visit(g, v, color, post, np)) return 0;
    }
    color[u] = BLACK;
    post[(*np)++] = u;           /* everything u points to is already in post[] */
    return 1;
}

static int dfs_topo(const Dag *g, int *order)
{
    int color[MAXN] = {0}, post[MAXN], np = 0;
    for (int u = 0; u < g->n; u++)
        if (color[u] == WHITE && !dfs_topo_visit(g, u, color, post, &np))
            return 0;
    for (int i = 0; i < np; i++) order[i] = post[np - 1 - i];   /* reverse postorder */
    return np;
}

/* An order is valid iff every edge u->v has pos[u] < pos[v]. */
static int valid_order(const Dag *g, const int *order, int k)
{
    int pos[MAXN];
    if (k != g->n) return 0;
    for (int i = 0; i < k; i++) pos[order[i]] = i;
    for (int u = 0; u < g->n; u++)
        for (int i = 0; i < g->len[u]; i++)
            if (pos[u] >= pos[g->adj[u][i]]) return 0;
    return 1;
}

static void print_order(const char *label, const int *order, int k, const char *const *name)
{
    printf("%s", label);
    for (int i = 0; i < k; i++) printf("%s%s", name[order[i]], i < k - 1 ? " -> " : "\n");
}

static unsigned rng_state = 99u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static Dag g;
    int order[MAXN], k;

    /* ---- 1. build graph ---- */
    static const char *const bname[] = { "app", "main.o", "util.o", "config.h", "main.c", "util.c" };
    dag_init(&g, 6);
    dag_arc(&g, 3, 1); dag_arc(&g, 4, 1);    /* main.o needs config.h, main.c */
    dag_arc(&g, 3, 2); dag_arc(&g, 5, 2);    /* util.o needs config.h, util.c */
    dag_arc(&g, 1, 0); dag_arc(&g, 2, 0);    /* app needs main.o, util.o      */

    printf("1) build DAG, Kahn's algorithm:\n");
    k = kahn(&g, order, bname, 1);
    print_order("  build order: ", order, k, bname);
    static const int expect_kahn[6] = {3, 4, 5, 1, 2, 0};
    CHECK(k == 6 && memcmp(order, expect_kahn, sizeof expect_kahn) == 0);
    CHECK(valid_order(&g, order, k));

    k = dfs_topo(&g, order);
    print_order("  DFS reverse-postorder: ", order, k, bname);
    static const int expect_dfs[6] = {5, 4, 3, 2, 1, 0};
    CHECK(k == 6 && memcmp(order, expect_dfs, sizeof expect_dfs) == 0);
    CHECK(valid_order(&g, order, k));
    printf("  (different order, both valid: a DAG usually has many topological orders)\n");

    /* ---- 2. a cycle: app also "needed by" config.h (nonsense dependency) ---- */
    dag_arc(&g, 0, 3);
    k = kahn(&g, order, NULL, 0);
    printf("\n2) add bogus edge app -> config.h: Kahn outputs only %d of 6 vertices "
           "-> CYCLE (make: \"Circular dependency dropped\")\n", k);
    CHECK(k == 2);         /* main.c, util.c; then config.h waits for app: stuck */
    CHECK(dfs_topo(&g, order) == 0);

    /* ---- 3. GStreamer playback pipeline (data flows along the edges) ----
     *
     *  filesrc -> qtdemux -+-> h264parse -> avdec_h264 -> videoconvert -> autovideosink
     *                      +-> aacparse  -> avdec_aac  -> audioconvert -> autoaudiosink
     */
    static const char *const gname[] = { "filesrc", "qtdemux", "h264parse", "avdec_h264",
        "videoconvert", "autovideosink", "aacparse", "avdec_aac", "audioconvert", "autoaudiosink" };
    dag_init(&g, 10);
    static const int GE[][2] = { {0,1},{1,2},{2,3},{3,4},{4,5},{1,6},{6,7},{7,8},{8,9} };
    for (int i = 0; i < 9; i++) dag_arc(&g, GE[i][0], GE[i][1]);
    k = kahn(&g, order, gname, 0);
    CHECK(k == 10 && valid_order(&g, order, k));
    print_order("\n3) GStreamer pipeline, data-flow (topological) order:\n   ", order, k, gname);
    printf("   state change NULL->PLAYING goes sink -> source (REVERSE topological), so every\n"
           "   element is ready before upstream pushes buffers into it:\n   ");
    for (int i = k - 1; i >= 0; i--) printf("%s%s", gname[order[i]], i ? " <- " : "\n");

    /* ---- 4. random cross-check ---- */
    for (int t = 0; t < 500; t++) {
        int n = 1 + (int)(rnd() % 15), perm[MAXN];
        for (int i = 0; i < n; i++) perm[i] = i;
        for (int i = n - 1; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); int x = perm[i]; perm[i] = perm[j]; perm[j] = x; }
        dag_init(&g, n);
        int m = (int)(rnd() % (unsigned)(2 * n + 1));
        int allow_cycle = t % 2;
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            if (a == b) continue;
            if (!allow_cycle && a > b) { int x = a; a = b; b = x; }   /* forward in perm => DAG */
            if (g.len[perm[a]] < MAXDEG) dag_arc(&g, perm[a], perm[b]);
        }
        int o1[MAXN], o2[MAXN];
        int k1 = kahn(&g, o1, NULL, 0), k2 = dfs_topo(&g, o2);
        CHECK((k1 == n) == (k2 == n));            /* both agree on "is it a DAG?" */
        if (!allow_cycle) CHECK(k1 == n);
        if (k1 == n) { CHECK(valid_order(&g, o1, n)); CHECK(valid_order(&g, o2, n)); }
    }
    printf("\nrandom cross-check: 500 graphs, Kahn and DFS agree on acyclicity and\n"
           "both produce valid orders\n");
    printf("OK\n");
    return 0;
}
