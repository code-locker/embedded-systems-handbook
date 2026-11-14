/*
 * 14_max_flow_matching.c — maximum flow (Edmonds-Karp), the matching minimum
 *                          cut, and maximum bipartite matching
 *
 * Concept : Pipes with capacities from source s to sink t: how much can flow?
 *           Ford-Fulkerson: while there is an s->t path with spare capacity in
 *           the RESIDUAL graph, push the bottleneck amount along it. Pushing f
 *           along u->v also adds f of capacity to v->u — the "undo" edge that
 *           lets a later path re-route earlier flow. Edmonds-Karp = choose
 *           the SHORTEST augmenting path (BFS): O(V * E^2), always terminates.
 *           MAX-FLOW = MIN-CUT: when no path is left, the vertices reachable
 *           from s form one side of a cut whose capacity equals the flow.
 *           Bipartite matching = max flow with all capacities 1; the classic
 *           shorter code is Kuhn's augmenting-path algorithm, shown too.
 * Build   : make run P=dsa/13-graphs/c/14_max_flow_matching.c
 * Read    : ../README.md and ../notes/08-advanced.md
 *
 *   G10 (CLRS fig. 26.1; s = 0, t = 5), capacities on the arcs:
 *
 *            16             12
 *     0 ----------> 1 -------------> 3
 *     |             ^            _.' ^ \
 *     |13          4|     9  _.-'    |7 \ 20
 *     |             |   _.-'         |   v
 *     |             |<-'             |    5
 *     +-----------> 2 -------------> 4 ---^
 *                           14          4
 *
 *   arcs: 0->1:16 0->2:13 1->3:12 2->1:4 2->4:14 3->2:9 3->5:20 4->3:7 4->5:4
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXV = 24 };

/* Residual capacities in a matrix: cap[u][v] = how much MORE can go u->v.
 * O(V^2) memory is fine here and makes the reverse edge trivial (cap[v][u]). */
typedef struct { int n; int cap[MAXV][MAXV]; } Net;

static int min2(int a, int b) { return a < b ? a : b; }

/* BFS in the residual graph; fills parent[], returns 1 if t was reached. */
static int bfs_path(const Net *g, int s, int t, int *parent)
{
    int q[MAXV], h = 0, tl = 0;
    for (int v = 0; v < g->n; v++) parent[v] = -1;
    parent[s] = s;
    q[tl++] = s;
    while (h < tl) {
        int u = q[h++];
        for (int v = 0; v < g->n; v++)
            if (parent[v] == -1 && g->cap[u][v] > 0) {
                parent[v] = u;
                if (v == t) return 1;
                q[tl++] = v;
            }
    }
    return 0;
}

/* Edmonds-Karp: modifies g into the final residual network. Returns max flow. */
static int edmonds_karp(Net *g, int s, int t, int trace)
{
    int flow = 0, parent[MAXV], round = 0;
    while (bfs_path(g, s, t, parent)) {
        int b = 1 << 30;
        for (int v = t; v != s; v = parent[v]) b = min2(b, g->cap[parent[v]][v]);   /* bottleneck */
        for (int v = t; v != s; v = parent[v]) {
            g->cap[parent[v]][v] -= b;      /* use forward capacity            */
            g->cap[v][parent[v]] += b;      /* create "undo" capacity backwards */
        }
        flow += b;
        if (trace) {
            int p[MAXV], k = 0;
            for (int v = t; v != s; v = parent[v]) p[k++] = v;
            printf("  path %d: %d", ++round, s);
            for (int i = k - 1; i >= 0; i--) printf(" -> %d", p[i]);
            printf("   bottleneck %d, total flow %d\n", b, flow);
        }
    }
    return flow;
}

/* After max flow: S side = vertices still reachable from s in the residual. */
static void min_cut_side(const Net *g, int s, int *in_s)
{
    int parent[MAXV];
    bfs_path(g, s, -1, parent);                       /* t = -1: explore everything */
    for (int v = 0; v < g->n; v++) in_s[v] = parent[v] != -1;
}

/* Brute force min cut: every subset containing s but not t. O(2^V * V^2). */
static int brute_min_cut(const Net *orig, int s, int t)
{
    int best = 1 << 30;
    for (unsigned mask = 0; mask < (1u << orig->n); mask++) {
        if (!(mask >> s & 1) || (mask >> t & 1)) continue;
        int c = 0;
        for (int u = 0; u < orig->n; u++)
            for (int v = 0; v < orig->n; v++)
                if ((mask >> u & 1) && !(mask >> v & 1)) c += orig->cap[u][v];
        best = min2(best, c);
    }
    return best;
}

/* ---------------------------------------------------- Kuhn's matching == */
/* adj[l] = bitmask of right vertices that left vertex l may take. */
static int kuhn_try(int l, const unsigned *adj, int nr, int *match_r, unsigned char *seen, int trace, int depth)
{
    for (int r = 0; r < nr; r++) {
        if (!(adj[l] >> r & 1) || seen[r]) continue;
        seen[r] = 1;
        if (trace) printf("  %*sL%d tries R%d%s\n", 2 * depth, "", l, r,
                          match_r[r] == -1 ? " (free: take it)" : " (taken: ask its owner to move)");
        /* r is free, or its current partner can move somewhere else */
        if (match_r[r] == -1 || kuhn_try(match_r[r], adj, nr, match_r, seen, trace, depth + 1)) {
            match_r[r] = l;
            return 1;
        }
    }
    return 0;
}

/* O(V * E). Returns the matching size; match_r[r] = left partner or -1. */
static int kuhn(int nl, int nr, const unsigned *adj, int *match_r, int trace)
{
    int size = 0;
    for (int r = 0; r < nr; r++) match_r[r] = -1;
    for (int l = 0; l < nl; l++) {
        unsigned char seen[MAXV] = {0};
        if (trace) printf(" augment from L%d:\n", l);
        size += kuhn_try(l, adj, nr, match_r, seen, trace, 1);
    }
    return size;
}

/* the same problem as a flow network: s -> L (1), L -> R (1), R -> t (1) */
static int matching_by_flow(int nl, int nr, const unsigned *adj)
{
    static Net g;
    memset(&g, 0, sizeof g);
    g.n = nl + nr + 2;
    int s = nl + nr, t = s + 1;
    for (int l = 0; l < nl; l++) {
        g.cap[s][l] = 1;
        for (int r = 0; r < nr; r++) if (adj[l] >> r & 1) g.cap[l][nl + r] = 1;
    }
    for (int r = 0; r < nr; r++) g.cap[nl + r][t] = 1;
    return edmonds_karp(&g, s, t, 0);
}

static int brute_matching(int nl, int nr, const unsigned *adj, int l, unsigned used)
{
    if (l == nl) return 0;
    int best = brute_matching(nl, nr, adj, l + 1, used);          /* leave l unmatched */
    for (int r = 0; r < nr; r++)
        if ((adj[l] >> r & 1) && !(used >> r & 1)) {
            int c = 1 + brute_matching(nl, nr, adj, l + 1, used | 1u << r);
            if (c > best) best = c;
        }
    return best;
}

static unsigned rng_state = 9001u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static Net g, orig;
    memset(&g, 0, sizeof g);
    g.n = 6;
    static const int A[][3] = { {0,1,16},{0,2,13},{1,3,12},{2,1,4},{2,4,14},{3,2,9},{3,5,20},{4,3,7},{4,5,4} };
    for (int i = 0; i < 9; i++) g.cap[A[i][0]][A[i][1]] = A[i][2];
    orig = g;

    printf("1) Edmonds-Karp on G10 (s=0, t=5), shortest augmenting path each round:\n");
    int f = edmonds_karp(&g, 0, 5, 1);
    printf("  max flow = %d\n", f);
    CHECK(f == 23);

    int in_s[MAXV], cut = 0;
    min_cut_side(&g, 0, in_s);
    printf("\n2) min cut: S = {");
    for (int v = 0; v < 6; v++) if (in_s[v]) printf(" %d", v);
    printf(" }, cut edges:");
    for (int u = 0; u < 6; u++)
        for (int v = 0; v < 6; v++)
            if (in_s[u] && !in_s[v] && orig.cap[u][v] > 0) { printf(" %d->%d(%d)", u, v, orig.cap[u][v]); cut += orig.cap[u][v]; }
    printf("  total %d == max flow\n", cut);
    CHECK(cut == 23 && in_s[0] && in_s[1] && in_s[2] && in_s[4] && !in_s[3] && !in_s[5]);
    CHECK(brute_min_cut(&orig, 0, 5) == 23);

    for (int t = 0; t < 300; t++) {
        memset(&g, 0, sizeof g);
        g.n = 2 + (int)(rnd() % 8);
        int m = (int)(rnd() % (unsigned)(3 * g.n));
        for (int i = 0; i < m; i++) {
            int a = (int)(rnd() % (unsigned)g.n), b = (int)(rnd() % (unsigned)g.n);
            if (a != b) g.cap[a][b] += 1 + (int)(rnd() % 10);
        }
        orig = g;
        int s = 0, tt = g.n - 1;
        CHECK(edmonds_karp(&g, s, tt, 0) == brute_min_cut(&orig, s, tt));
    }
    printf("   random cross-check: 300 networks, max flow == brute-force min cut\n");

    /* 3. Bipartite matching: 4 camera streams (L) onto 4 hardware video
     *    encoders (R); each stream can only use some encoders.
     *      L0: R0 R1   L1: R0   L2: R1 R2   L3: R2 R3
     */
    static const unsigned adj[4] = { 0x3, 0x1, 0x6, 0xC };
    int match_r[MAXV];
    printf("\n3) bipartite matching (Kuhn), streams L -> encoders R:\n");
    int msize = kuhn(4, 4, adj, match_r, 1);
    printf("  matching size %d:", msize);
    for (int r = 0; r < 4; r++) printf(" L%d-R%d", match_r[r], r);
    printf("\n");
    CHECK(msize == 4 && match_r[0] == 1 && match_r[1] == 0 && match_r[2] == 2 && match_r[3] == 3);
    CHECK(matching_by_flow(4, 4, adj) == 4);

    for (int t = 0; t < 300; t++) {
        int nl = 1 + (int)(rnd() % 7), nr = 1 + (int)(rnd() % 7);
        unsigned ra[MAXV];
        for (int l = 0; l < nl; l++) ra[l] = rnd() & ((1u << nr) - 1) & rnd();
        int a = kuhn(nl, nr, ra, match_r, 0);
        CHECK(a == matching_by_flow(nl, nr, ra));
        CHECK(a == brute_matching(nl, nr, ra, 0, 0));
    }
    printf("   random cross-check: 300 bipartite graphs, Kuhn == max flow == brute force\n");
    printf("OK\n");
    return 0;
}
