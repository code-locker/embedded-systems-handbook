/*
 * 15_euler_hierholzer.c — Euler path / circuit (use every EDGE exactly once)
 *                         with Hierholzer's algorithm
 *
 * Concept : Existence (undirected): all edges in one connected piece, and the
 *           number of odd-degree vertices is 0 (circuit: start anywhere) or 2
 *           (path: start at one odd vertex, end at the other). Why: every
 *           time you pass THROUGH a vertex you use 2 of its edges.
 *           Hierholzer: walk unused edges until stuck (you are stuck exactly
 *           at the end vertex); while backtracking, any vertex with unused
 *           edges starts a sub-tour that gets spliced in. With an explicit
 *           stack: push when you walk, pop to the answer when stuck. O(E).
 *           Examples: Koenigsberg bridges (impossible), the "house" you draw
 *           without lifting the pen (path), and a circuit.
 * Build   : make run P=dsa/13-graphs/c/15_euler_hierholzer.c
 * Read    : ../README.md and ../notes/08-advanced.md  (section "Euler")
 *
 *   House (draw it in one stroke):        Koenigsberg (multigraph, 7 bridges):
 *
 *            4                                  C
 *           / \                               // \
 *          3---2                              A---D       A-B x2, A-C x2,
 *          | X |    X = diagonals 0-2, 1-3    \\ /        A-D, B-D, C-D
 *          0---1                                B         all 4 degrees odd
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXN = 12, MAXM = 32 };

typedef struct {
    int n, m;
    int eu[MAXM], ev[MAXM];
} Graph;

static void add(Graph *g, int u, int v) { CHECK(g->m < MAXM); g->eu[g->m] = u; g->ev[g->m] = v; g->m++; }

static int other(const Graph *g, int e, int x) { return g->eu[e] == x ? g->ev[e] : g->eu[e]; }

/* Checks the degree + connectivity conditions. Returns the start vertex, or
 * -1 if no Euler path exists. *circuit = 1 if it is a closed circuit. */
static int euler_start(const Graph *g, int *circuit)
{
    int deg[MAXN] = {0}, odd = 0, start = -1;
    for (int e = 0; e < g->m; e++) { deg[g->eu[e]]++; deg[g->ev[e]]++; }   /* self-loop adds 2 */
    for (int v = 0; v < g->n; v++) if (deg[v] % 2) { odd++; if (start < 0) start = v; }
    if (odd != 0 && odd != 2) return -1;
    if (g->m == 0) { *circuit = 1; return 0; }
    if (start < 0) for (int v = 0; v < g->n; v++) if (deg[v]) { start = v; break; }

    /* every vertex with an edge must be reachable from start (isolated
     * vertices do not matter: there is nothing to walk there) */
    int seen[MAXN] = {0}, stack[MAXN], top = 0;
    stack[top++] = start; seen[start] = 1;
    while (top) {
        int u = stack[--top];
        for (int e = 0; e < g->m; e++)
            if (g->eu[e] == u || g->ev[e] == u) {
                int v = other(g, e, u);
                if (!seen[v]) { seen[v] = 1; stack[top++] = v; }
            }
    }
    for (int v = 0; v < g->n; v++) if (deg[v] && !seen[v]) return -1;
    *circuit = odd == 0;
    return start;
}

/*
 * Iterative Hierholzer. used[e] marks edges, ptr[v] remembers how far v's edge
 * scan got (so total work is O(E) with adjacency lists; this demo scans the
 * edge list, O(V*E), to stay short). Returns vertex count of the trail.
 *
 *   stack holds the current walk; when the top has no unused edge, it is
 *   "finished" and is moved to the output. Output comes out REVERSED.
 */
static int hierholzer(const Graph *g, int start, int *out, int trace)
{
    int used[MAXM] = {0}, ptr[MAXN] = {0}, stack[MAXM + 1], top = 0, k = 0;
    stack[top++] = start;
    while (top) {
        int u = stack[top - 1], e = -1;
        while (ptr[u] < g->m) {
            int c = ptr[u]++;
            if (!used[c] && (g->eu[c] == u || g->ev[c] == u)) { e = c; break; }
        }
        if (e >= 0) {
            used[e] = 1;
            stack[top++] = other(g, e, u);          /* walk the edge */
        } else {
            out[k++] = stack[--top];                /* stuck: emit */
            if (trace) {
                printf("  stuck at %d -> emit.   stack:", u);
                for (int i = 0; i < top; i++) printf(" %d", stack[i]);
                printf("   output(reversed):");
                for (int i = 0; i < k; i++) printf(" %d", out[i]);
                printf("\n");
            }
        }
    }
    for (int i = 0; i < k / 2; i++) { int t = out[i]; out[i] = out[k - 1 - i]; out[k - 1 - i] = t; }
    return k;
}

/* A trail is valid if it has m+1 vertices and uses each edge exactly once. */
static int valid_trail(const Graph *g, const int *t, int k)
{
    int used[MAXM] = {0};
    if (k != g->m + 1) return 0;
    for (int i = 0; i + 1 < k; i++) {
        int found = 0;
        for (int e = 0; e < g->m && !found; e++)
            if (!used[e] && ((g->eu[e] == t[i] && g->ev[e] == t[i + 1]) || (g->ev[e] == t[i] && g->eu[e] == t[i + 1])))
                used[e] = found = 1;
        if (!found) return 0;
    }
    return 1;
}

/* brute force: does ANY Euler trail exist? Try every start and every order. */
static int brute_rec(const Graph *g, int u, int *used, int left)
{
    if (left == 0) return 1;
    for (int e = 0; e < g->m; e++)
        if (!used[e] && (g->eu[e] == u || g->ev[e] == u)) {
            used[e] = 1;
            int ok = brute_rec(g, other(g, e, u), used, left - 1);
            used[e] = 0;
            if (ok) return 1;
        }
    return 0;
}
static int brute_exists(const Graph *g)
{
    int used[MAXM] = {0};
    if (g->m == 0) return 1;
    for (int s = 0; s < g->n; s++) if (brute_rec(g, s, used, g->m)) return 1;
    return 0;
}

static unsigned rng_state = 1736u;   /* Euler solved Koenigsberg in 1736 */
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    Graph g;
    int out[MAXM + 1], circuit = 0;

    /* 1. Koenigsberg: A=0 B=1 C=2 D=3 */
    memset(&g, 0, sizeof g); g.n = 4;
    add(&g, 0, 1); add(&g, 0, 1); add(&g, 0, 2); add(&g, 0, 2); add(&g, 0, 3); add(&g, 1, 3); add(&g, 2, 3);
    CHECK(euler_start(&g, &circuit) == -1);
    CHECK(!brute_exists(&g));
    printf("1) Koenigsberg: degrees A=5 B=3 C=3 D=3 -> 4 odd vertices -> NO Euler path\n");

    /* 2. House: odd vertices 0 and 1 -> Euler path from 0 to 1 */
    memset(&g, 0, sizeof g); g.n = 5;
    /* edge order chosen so the first walk gets stuck early and a sub-tour
     * 2-3-4-2 has to be spliced in while backtracking */
    add(&g, 0, 1); add(&g, 1, 3); add(&g, 3, 0); add(&g, 0, 2);
    add(&g, 2, 1); add(&g, 2, 3); add(&g, 3, 4); add(&g, 2, 4);
    int s = euler_start(&g, &circuit);
    printf("\n2) House: deg = 3 3 4 4 2 -> exactly two odd vertices (0, 1): Euler PATH from %d\n", s);
    CHECK(s == 0 && !circuit);
    int k = hierholzer(&g, s, out, 1);
    printf("  trail:");
    for (int i = 0; i < k; i++) printf(" %d", out[i]);
    printf("\n");
    CHECK(valid_trail(&g, out, k) && out[0] == 0 && out[k - 1] == 1);
    static const int expect_house[9] = {0, 1, 3, 0, 2, 3, 4, 2, 1};
    CHECK(k == 9 && memcmp(out, expect_house, sizeof expect_house) == 0);

    /* 3. House without the floor 0-1: every degree even -> circuit */
    memset(&g, 0, sizeof g); g.n = 5;
    add(&g, 1, 2); add(&g, 2, 3); add(&g, 3, 0); add(&g, 0, 2); add(&g, 1, 3); add(&g, 2, 4); add(&g, 3, 4);
    s = euler_start(&g, &circuit);
    k = hierholzer(&g, s, out, 0);
    printf("\n3) House without floor 0-1: all degrees even -> Euler CIRCUIT:");
    for (int i = 0; i < k; i++) printf(" %d", out[i]);
    printf("\n");
    CHECK(circuit && valid_trail(&g, out, k) && out[0] == out[k - 1]);

    /* 4. random multigraphs: condition == brute force; trail always valid */
    int yes = 0;
    for (int t = 0; t < 400; t++) {
        memset(&g, 0, sizeof g);
        g.n = 1 + (int)(rnd() % 5);
        int m = (int)(rnd() % 8);
        for (int i = 0; i < m; i++) add(&g, (int)(rnd() % (unsigned)g.n), (int)(rnd() % (unsigned)g.n));
        int st = euler_start(&g, &circuit);
        CHECK((st >= 0) == brute_exists(&g));
        if (st >= 0) {
            k = hierholzer(&g, st, out, 0);
            CHECK(valid_trail(&g, out, k));
            if (circuit) CHECK(out[0] == out[k - 1]);
            yes++;
        }
    }
    printf("\nrandom cross-check: 400 multigraphs (self-loops, parallel edges), degree rule ==\n"
           "brute force; all %d Hierholzer trails valid\n", yes);
    printf("OK\n");
    return 0;
}
