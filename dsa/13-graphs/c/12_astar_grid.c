/*
 * 12_astar_grid.c — A* search on a grid (an IMPLICIT graph) with the
 *                   Manhattan-distance heuristic, compared to Dijkstra
 *
 * Concept : The grid is never stored as a graph: neighbours of cell (r,c)
 *           are computed on the fly (up/down/left/right, not a wall).
 *           Dijkstra expands cells in order of g = cost so far — a circle
 *           growing in all directions. A* expands in order of
 *           f = g + h, where h = an optimistic guess of the remaining cost
 *           (Manhattan distance never overestimates on a 4-neighbour grid with
 *           unit steps). That pulls the search toward the goal while staying
 *           optimal. h = 0 turns A* back into Dijkstra.
 *           The program draws which cells each one expanded, asserts both find
 *           the same length as BFS, and cross-checks on random mazes.
 * Build   : make run P=dsa/13-graphs/c/12_astar_grid.c
 * Read    : ../README.md and ../notes/06-shortest-paths.md  (section "A*")
 *
 *   Grid (S start, G goal, # wall). The U-shaped wall is a trap: the
 *   heuristic first pulls the search straight right into the pocket.
 *
 *        0 1 2 3 4 5 6 7 8 9
 *     0  . . . . . . . . . .
 *     1  . . . . . . . . . .
 *     2  . . # # # # # . . .
 *     3  S . . . . . # . . G
 *     4  . . # # # # # . . .
 *     5  . . . . . . . . . .
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { MAXR = 12, MAXC = 12, MAXCELLS = MAXR * MAXC };

typedef struct {
    int  rows, cols;
    char cell[MAXR][MAXC + 1];
} Grid;

static const int DR[4] = { -1, 1, 0, 0 }, DC[4] = { 0, 0, -1, 1 };

static int iabs(int x) { return x < 0 ? -x : x; }

/* ---- min-heap ordered by (f, then LARGER g): among equal f, prefer the node
 *      that is deeper = closer to the goal. Cuts expansions on open grids. */
typedef struct { int f, g, cell; } Node;
typedef struct { Node a[4 * MAXCELLS + 8]; int len; } Heap;   /* <= 4 pushes per cell */

static int less(Node x, Node y) { return x.f != y.f ? x.f < y.f : x.g > y.g; }

static void push(Heap *h, Node x)
{
    CHECK(h->len < (int)(sizeof h->a / sizeof h->a[0]));
    int i = h->len++;
    h->a[i] = x;
    while (i > 0 && less(h->a[i], h->a[(i - 1) / 2])) {
        Node t = h->a[i]; h->a[i] = h->a[(i - 1) / 2]; h->a[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static Node pop(Heap *h)
{
    Node top = h->a[0];
    h->a[0] = h->a[--h->len];
    for (int i = 0;;) {
        int l = 2 * i + 1, r = l + 1, s = i;
        if (l < h->len && less(h->a[l], h->a[s])) s = l;
        if (r < h->len && less(h->a[r], h->a[s])) s = r;
        if (s == i) break;
        Node t = h->a[i]; h->a[i] = h->a[s]; h->a[s] = t;
        i = s;
    }
    return top;
}

/*
 * astar — returns path length (or -1). use_h = 0 makes it plain Dijkstra.
 * expanded[cell] = 1 for every cell taken out of the heap as "closed".
 * parent[cell] lets us draw the path.
 * Complexity: O(E log V) worst case like Dijkstra; in practice far fewer
 * cells when h is informative.
 */
static int astar(const Grid *gr, int s, int t, int use_h, unsigned char *expanded, int *parent, int *n_expanded)
{
    static Heap h;
    int g[MAXCELLS];
    unsigned char closed[MAXCELLS] = {0};
    int n = gr->rows * gr->cols, tr = t / gr->cols, tc = t % gr->cols;
    for (int i = 0; i < n; i++) { g[i] = -1; parent[i] = -1; expanded[i] = 0; }
    h.len = 0;
    *n_expanded = 0;

    g[s] = 0;
    int h0 = use_h ? iabs(s / gr->cols - tr) + iabs(s % gr->cols - tc) : 0;
    push(&h, (Node){ h0, 0, s });
    while (h.len) {
        Node x = pop(&h);
        if (closed[x.cell]) continue;              /* stale duplicate */
        closed[x.cell] = 1;
        expanded[x.cell] = 1;
        (*n_expanded)++;
        if (x.cell == t) return x.g;               /* goal popped: optimal (consistent h) */
        int r = x.cell / gr->cols, c = x.cell % gr->cols;
        for (int k = 0; k < 4; k++) {              /* neighbours computed, not stored */
            int nr = r + DR[k], nc = c + DC[k];
            if (nr < 0 || nr >= gr->rows || nc < 0 || nc >= gr->cols) continue;
            if (gr->cell[nr][nc] == '#') continue;
            int v = nr * gr->cols + nc, ng = x.g + 1;
            if (g[v] == -1 || ng < g[v]) {
                g[v] = ng;
                parent[v] = x.cell;
                int hv = use_h ? iabs(nr - tr) + iabs(nc - tc) : 0;
                push(&h, (Node){ ng + hv, ng, v });
            }
        }
    }
    return -1;
}

static int bfs_len(const Grid *gr, int s, int t)      /* reference answer */
{
    int dist[MAXCELLS], q[MAXCELLS], hd = 0, tl = 0, n = gr->rows * gr->cols;
    for (int i = 0; i < n; i++) dist[i] = -1;
    dist[s] = 0; q[tl++] = s;
    while (hd < tl) {
        int u = q[hd++], r = u / gr->cols, c = u % gr->cols;
        for (int k = 0; k < 4; k++) {
            int nr = r + DR[k], nc = c + DC[k];
            if (nr < 0 || nr >= gr->rows || nc < 0 || nc >= gr->cols || gr->cell[nr][nc] == '#') continue;
            int v = nr * gr->cols + nc;
            if (dist[v] == -1) { dist[v] = dist[u] + 1; q[tl++] = v; }
        }
    }
    return dist[t];
}

static void draw(const Grid *gr, const unsigned char *expanded, const int *parent, int s, int t)
{
    char out[MAXR][MAXC + 1];
    memcpy(out, gr->cell, sizeof out);
    for (int i = 0; i < gr->rows * gr->cols; i++)
        if (expanded[i] && out[i / gr->cols][i % gr->cols] == '.') out[i / gr->cols][i % gr->cols] = 'o';
    for (int v = parent[t]; v != -1 && v != s; v = parent[v]) out[v / gr->cols][v % gr->cols] = '*';
    for (int r = 0; r < gr->rows; r++) {
        printf("     ");
        for (int c = 0; c < gr->cols; c++) printf("%c ", out[r][c]);
        printf("\n");
    }
}

static unsigned rng_state = 2718u;
static unsigned rnd(void)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return rng_state;
}

int main(void)
{
    static const char *const MAP[] = {
        "..........",
        "..........",
        "..#####...",
        "S.....#..G",
        "..#####...",
        "..........",
    };
    static Grid gr;
    gr.rows = 6; gr.cols = 10;
    for (int r = 0; r < gr.rows; r++) memcpy(gr.cell[r], MAP[r], (size_t)gr.cols + 1);
    int s = 3 * gr.cols + 0, t = 3 * gr.cols + 9;

    unsigned char exp_d[MAXCELLS], exp_a[MAXCELLS];
    int par_d[MAXCELLS], par_a[MAXCELLS], nd, na;
    int len_d = astar(&gr, s, t, 0, exp_d, par_d, &nd);
    int len_a = astar(&gr, s, t, 1, exp_a, par_a, &na);
    int len_b = bfs_len(&gr, s, t);

    printf("Dijkstra (h = 0): path length %d, expanded %d cells   (o = expanded, * = path)\n", len_d, nd);
    draw(&gr, exp_d, par_d, s, t);
    printf("\nA* (h = Manhattan): path length %d, expanded %d cells\n", len_a, na);
    draw(&gr, exp_a, par_a, s, t);
    printf("\nBFS reference length: %d. Manhattan(S,G) = 9 is a lower bound; the wall forces 13.\n", len_b);

    CHECK(len_b == 13 && len_d == 13 && len_a == 13);
    CHECK(na < nd);
    printf("A* expanded %d of the %d cells Dijkstra needed.\n", na, nd);

    /* Random mazes: A* and Dijkstra must both match BFS exactly. */
    for (int k = 0; k < 300; k++) {
        gr.rows = 2 + (int)(rnd() % (MAXR - 1)); gr.cols = 2 + (int)(rnd() % (MAXC - 1));
        for (int r = 0; r < gr.rows; r++) {
            for (int c = 0; c < gr.cols; c++) gr.cell[r][c] = (rnd() % 100 < 28) ? '#' : '.';
            gr.cell[r][gr.cols] = '\0';
        }
        int a = (int)(rnd() % (unsigned)(gr.rows * gr.cols)), b = (int)(rnd() % (unsigned)(gr.rows * gr.cols));
        gr.cell[a / gr.cols][a % gr.cols] = '.';
        gr.cell[b / gr.cols][b % gr.cols] = '.';
        int lb = bfs_len(&gr, a, b);
        CHECK(astar(&gr, a, b, 1, exp_a, par_a, &na) == lb);
        CHECK(astar(&gr, a, b, 0, exp_d, par_d, &nd) == lb);
    }
    printf("random cross-check: 300 random mazes, A* == Dijkstra == BFS lengths\n");
    printf("OK\n");
    return 0;
}
