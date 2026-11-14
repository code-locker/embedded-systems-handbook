/*
 * 04_union_find.c — connected components with BFS and with union-find (DSU):
 *                   path compression + union by rank
 *
 * Concept : A DSU keeps a forest: every set is a tree, its root is the set's
 *           "name". find(x) walks to the root; union(a,b) hangs one root under
 *           the other. Two tricks make trees nearly flat:
 *             union by rank   : hang the shorter tree under the taller one
 *             path compression: after find, point every visited node at the root
 *           Together: alpha(n) <= 4 amortised per op for any n you will ever see.
 *           Shows the parent[] array after each union on G3, the chain that a
 *           naive DSU builds vs. the flat tree a ranked DSU builds, and
 *           cross-checks DSU components against BFS components on random graphs.
 * Build   : make run P=dsa/13-graphs/c/04_union_find.c
 * Read    : ../README.md and ../notes/04-connectivity.md  (section "Union-find")
 *
 *   G3 (undirected, 10 vertices, 5 components):
 *
 *     0 --- 1 --- 2      3 --- 4        5 --- 6      8      9
 *                                        \   /
 *                                          7
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

/* ------------------------------------------------------------------- DSU */
typedef struct {
    int  n;
    int *parent;    /* owned: parent[x] == x  <=>  x is a root            */
    int *rank;      /* owned: upper bound on tree height (only for roots) */
    int  sets;      /* number of disjoint sets right now                   */
} Dsu;

static Dsu dsu_new(int n)
{
    Dsu d = { n, malloc(sizeof(int) * (size_t)n), calloc((size_t)n, sizeof(int)), n };
    CHECK(d.parent && d.rank);
    for (int i = 0; i < n; i++) d.parent[i] = i;   /* everyone alone */
    return d;
}

static void dsu_free(Dsu *d) { free(d->parent); free(d->rank); }

/*
 * find with path compression (two-pass, iterative: no recursion depth risk).
 *
 *   before find(4):        after:
 *        0                    0
 *        |                 / | | \
 *        1                1  2 3  4
 *        |
 *        2       every node on the path now points straight at the root,
 *        |       so the NEXT find on any of them is one hop.
 *        3
 *        |
 *        4
 */
static int dsu_find(Dsu *d, int x)
{
    int root = x;
    while (d->parent[root] != root)        /* pass 1: locate the root */
        root = d->parent[root];
    while (d->parent[x] != root) {         /* pass 2: re-point the path */
        int next = d->parent[x];
        d->parent[x] = root;
        x = next;
    }
    return root;
}

/* Returns 1 if a and b were in different sets (a real merge), 0 if they were
 * already connected — for an edge that means "this edge closes a cycle". */
static int dsu_union(Dsu *d, int a, int b)
{
    int ra = dsu_find(d, a), rb = dsu_find(d, b);
    if (ra == rb)
        return 0;
    if (d->rank[ra] < d->rank[rb]) { int t = ra; ra = rb; rb = t; }  /* ra = taller */
    d->parent[rb] = ra;                   /* short tree under tall tree: height unchanged */
    if (d->rank[ra] == d->rank[rb])
        d->rank[ra]++;                    /* equal heights: the result is one taller */
    d->sets--;
    return 1;
}

/* Naive DSU: no rank, no compression — to SEE why the tricks matter. */
static int naive_find(const int *parent, int x)
{
    while (parent[x] != x) x = parent[x];
    return x;
}

static int depth(const int *parent, int x)
{
    int dep = 0;
    while (parent[x] != x) { x = parent[x]; dep++; }
    return dep;
}

/* ------------------------------------------------ BFS components (for checks) */
/* comp[v] = component id; adjacency matrix is fine for these small graphs. */
static int bfs_components(int n, const unsigned char *adj, int *comp)
{
    int *queue = malloc(sizeof(int) * (size_t)n);
    CHECK(queue != NULL);
    for (int v = 0; v < n; v++) comp[v] = -1;
    int c = 0;
    for (int s = 0; s < n; s++) {
        if (comp[s] != -1) continue;
        int h = 0, t = 0;
        queue[t++] = s; comp[s] = c;
        while (h < t) {
            int u = queue[h++];
            for (int v = 0; v < n; v++)
                if (adj[u * n + v] && comp[v] == -1) { comp[v] = c; queue[t++] = v; }
        }
        c++;
    }
    free(queue);
    return c;
}

static unsigned rng_state = 2024u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

static void print_arr(const char *name, const int *a, int n)
{
    printf("%s[", name);
    for (int i = 0; i < n; i++) printf("%d%s", a[i], i < n - 1 ? " " : "]");
}

int main(void)
{
    enum { N = 10 };
    static const int E[][2] = { {0,1},{1,2},{3,4},{5,6},{6,7},{7,5} };
    const int m = (int)(sizeof E / sizeof E[0]);

    Dsu d = dsu_new(N);
    printf("DSU on G3 — parent[] / rank[] after each edge\n\n");
    printf("start        : "); print_arr("parent", d.parent, N); printf("\n");
    int cycle_edges = 0;
    for (int i = 0; i < m; i++) {
        int merged = dsu_union(&d, E[i][0], E[i][1]);
        printf("union(%d,%d)   : ", E[i][0], E[i][1]);
        print_arr("parent", d.parent, N);
        printf("  ");
        print_arr("rank", d.rank, N);
        printf("  sets=%d%s\n", d.sets, merged ? "" : "  <- same root: edge closes a CYCLE");
        cycle_edges += !merged;
    }
    static const int expect_parent[N] = {0, 0, 0, 3, 3, 5, 5, 5, 8, 9};
    for (int v = 0; v < N; v++) CHECK(d.parent[v] == expect_parent[v]);
    CHECK(d.sets == 5);
    CHECK(cycle_edges == 1);
    CHECK(dsu_find(&d, 2) == dsu_find(&d, 0));
    CHECK(dsu_find(&d, 7) != dsu_find(&d, 3));
    printf("\n5 components; find(2)==find(0) (connected), find(7)!=find(3)\n");
    dsu_free(&d);

    /* --- Why the tricks matter: union(i, i+1) for a chain of 1024 vertices --- */
    enum { C = 1024 };
    int *naive = malloc(sizeof(int) * C);
    CHECK(naive != NULL);
    for (int i = 0; i < C; i++) naive[i] = i;
    for (int i = 0; i + 1 < C; i++) {
        int ra = naive_find(naive, i), rb = naive_find(naive, i + 1);
        naive[ra] = rb;                 /* always hang the OLD tree under the new node */
    }
    int naive_depth = depth(naive, 0);

    Dsu good = dsu_new(C);
    for (int i = 0; i + 1 < C; i++) dsu_union(&good, i, i + 1);
    int max_rank = 0, max_depth = 0;
    for (int i = 0; i < C; i++) {
        if (good.rank[i] > max_rank) max_rank = good.rank[i];
        if (depth(good.parent, i) > max_depth) max_depth = depth(good.parent, i);
    }
    printf("\nchain of %d unions:\n", C - 1);
    printf("  naive DSU          : deepest node is %d hops from the root (a linked list!)\n", naive_depth);
    printf("  rank + compression : deepest node is %d hops, max rank %d (<= log2 n = 10)\n",
           max_depth, max_rank);
    CHECK(naive_depth == C - 1);
    CHECK(max_depth <= 10 && max_rank <= 10);
    free(naive);
    dsu_free(&good);

    /* --- Random cross-check: DSU components == BFS components --- */
    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rnd() % 20);
        unsigned char *adj = calloc((size_t)(n * n), 1);
        CHECK(adj != NULL);
        Dsu r = dsu_new(n);
        int edges = (int)(rnd() % (unsigned)(n + 5));
        for (int i = 0; i < edges; i++) {
            int a = (int)(rnd() % (unsigned)n), b = (int)(rnd() % (unsigned)n);
            adj[a * n + b] = adj[b * n + a] = 1;
            dsu_union(&r, a, b);
        }
        int comp[20];
        int c = bfs_components(n, adj, comp);
        CHECK(c == r.sets);
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++)
                CHECK((comp[a] == comp[b]) == (dsu_find(&r, a) == dsu_find(&r, b)));
        dsu_free(&r);
        free(adj);
    }
    printf("\nrandom cross-check: 300 graphs, DSU components == BFS components\n");
    printf("OK\n");
    return 0;
}
