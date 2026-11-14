/*
 * 01_representations.c — one graph, four ways to store it:
 *                        adjacency matrix, adjacency list, edge list, CSR
 *
 * Concept : The SAME 8-vertex undirected graph (README graph G1) is stored as
 *           (1) an n*n adjacency matrix, (2) an array of singly linked lists,
 *           (3) a plain edge list and (4) CSR (compressed sparse row: two flat
 *           arrays, the format used by HPC / GPU graph code). Every
 *           representation is cross-checked against the others for every
 *           vertex pair, and memory use is compared.
 * Build   : make run P=dsa/13-graphs/c/01_representations.c
 * Read    : ../README.md and ../notes/02-representations.md
 *
 *   G1 (undirected, unweighted, 8 vertices, 9 edges):
 *
 *       0 ----- 1 ----- 2
 *       |       |       |
 *       3 ----- 4       5
 *               |       |
 *               6 ----- 7
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); abort(); } } while (0)

enum { N = 8, M = 9 };

typedef struct { int u, v; } EdgeUV;

/* The edge list IS the input format most problems give you: "m lines of u v". */
static const EdgeUV G1[M] = {
    {0, 1}, {0, 3}, {1, 2}, {1, 4}, {2, 5}, {3, 4}, {4, 6}, {5, 7}, {6, 7},
};

/* ------------------------------------------------------------------ matrix */
/*
 * Adjacency matrix: mat[u*n + v] = 1 if edge u-v exists.
 * One flat malloc (not an array of row pointers) = one allocation, one free,
 * cache-friendly rows. Memory O(n^2) no matter how few edges exist.
 * Owner: main() allocates, main() frees.
 */
static unsigned char *matrix_build(int n, const EdgeUV *e, int m)
{
    unsigned char *mat = calloc((size_t)n * (size_t)n, 1);   /* zeroed = no edges */
    CHECK(mat != NULL);
    for (int i = 0; i < m; i++) {
        mat[e[i].u * n + e[i].v] = 1;   /* undirected: store both directions,    */
        mat[e[i].v * n + e[i].u] = 1;   /* so the matrix is symmetric            */
    }
    return mat;
}

/* ------------------------------------------------------------ linked lists */
/*
 * Adjacency list: head[u] points to a linked list of u's neighbours.
 *
 *   head[1] -> [0] -> [2] -> [4] -> NULL
 *
 * We append at the TAIL so neighbours keep input order (deterministic traces).
 * Head insertion is O(1) without a tail pointer but reverses the order.
 */
typedef struct AdjNode {
    int             to;
    struct AdjNode *next;
} AdjNode;

typedef struct {
    int       n;
    AdjNode **head;   /* owned array of n list heads */
    AdjNode **tail;   /* owned array of n tail pointers (for O(1) append) */
} AdjList;

static void list_add_arc(AdjList *g, int u, int v)
{
    AdjNode *node = malloc(sizeof *node);   /* freed in list_free */
    CHECK(node != NULL);
    node->to = v;
    node->next = NULL;
    if (g->tail[u])
        g->tail[u]->next = node;
    else
        g->head[u] = node;
    g->tail[u] = node;
}

static AdjList list_build(int n, const EdgeUV *e, int m)
{
    AdjList g;
    g.n = n;
    g.head = calloc((size_t)n, sizeof *g.head);
    g.tail = calloc((size_t)n, sizeof *g.tail);
    CHECK(g.head && g.tail);
    for (int i = 0; i < m; i++) {
        list_add_arc(&g, e[i].u, e[i].v);   /* an undirected edge = two arcs */
        list_add_arc(&g, e[i].v, e[i].u);
    }
    return g;
}

static void list_free(AdjList *g)
{
    for (int u = 0; u < g->n; u++) {
        AdjNode *p = g->head[u];
        while (p) {                 /* save next BEFORE free: classic UAF trap */
            AdjNode *next = p->next;
            free(p);
            p = next;
        }
    }
    free(g->head);
    free(g->tail);
}

static bool list_has(const AdjList *g, int u, int v)    /* O(deg(u)) */
{
    for (const AdjNode *p = g->head[u]; p; p = p->next)
        if (p->to == v)
            return true;
    return false;
}

/* --------------------------------------------------------------------- CSR */
/*
 * CSR = Compressed Sparse Row. Two flat arrays:
 *   row_ptr[n+1] : neighbours of u live in col[row_ptr[u] .. row_ptr[u+1]-1]
 *   col[2m]      : all neighbour ids, concatenated, row by row
 *
 * Built in three passes (a counting sort by source vertex):
 *   1. count out-degree of every vertex
 *   2. exclusive prefix sum -> row_ptr
 *   3. scatter each arc into its slot (using a moving cursor per row)
 * Read-only after build: adding an edge means rebuilding. That is the price
 * for perfect cache behaviour and no per-node pointers.
 */
typedef struct {
    int  n;
    int *row_ptr;   /* owned, n+1 ints */
    int *col;       /* owned, 2m ints (undirected) */
} Csr;

static Csr csr_build(int n, const EdgeUV *e, int m)
{
    Csr c;
    c.n = n;
    c.row_ptr = calloc((size_t)n + 1, sizeof *c.row_ptr);
    c.col = malloc(sizeof *c.col * (size_t)(2 * m));
    CHECK(c.row_ptr && c.col);

    for (int i = 0; i < m; i++) {           /* pass 1: degrees, shifted by one */
        c.row_ptr[e[i].u + 1]++;
        c.row_ptr[e[i].v + 1]++;
    }
    for (int u = 0; u < n; u++)             /* pass 2: prefix sum              */
        c.row_ptr[u + 1] += c.row_ptr[u];

    int *cursor = malloc(sizeof *cursor * (size_t)n);   /* temp, freed below */
    CHECK(cursor != NULL);
    memcpy(cursor, c.row_ptr, sizeof *cursor * (size_t)n);
    for (int i = 0; i < m; i++) {           /* pass 3: scatter in input order  */
        c.col[cursor[e[i].u]++] = e[i].v;
        c.col[cursor[e[i].v]++] = e[i].u;
    }
    free(cursor);
    return c;
}

static void csr_free(Csr *c) { free(c->row_ptr); free(c->col); }

static bool csr_has(const Csr *c, int u, int v)   /* O(deg(u)), contiguous scan */
{
    for (int k = c->row_ptr[u]; k < c->row_ptr[u + 1]; k++)
        if (c->col[k] == v)
            return true;
    return false;
}

static bool edgelist_has(const EdgeUV *e, int m, int u, int v)   /* O(m) */
{
    for (int i = 0; i < m; i++)
        if ((e[i].u == u && e[i].v == v) || (e[i].u == v && e[i].v == u))
            return true;
    return false;
}

int main(void)
{
    printf("Graph G1: %d vertices, %d undirected edges\n\n", N, M);

    /* 1. edge list */
    printf("1) Edge list (%d pairs):", M);
    for (int i = 0; i < M; i++)
        printf(" %d-%d", G1[i].u, G1[i].v);
    printf("\n\n");

    /* 2. matrix */
    unsigned char *mat = matrix_build(N, G1, M);
    printf("2) Adjacency matrix (row u, column v):\n     ");
    for (int v = 0; v < N; v++)
        printf("%d ", v);
    printf("\n");
    for (int u = 0; u < N; u++) {
        printf("  %d: ", u);
        for (int v = 0; v < N; v++)
            printf("%d ", mat[u * N + v]);
        printf("\n");
    }

    /* 3. adjacency list */
    AdjList al = list_build(N, G1, M);
    printf("\n3) Adjacency list (array of linked lists):\n");
    for (int u = 0; u < N; u++) {
        printf("  head[%d]", u);
        for (const AdjNode *p = al.head[u]; p; p = p->next)
            printf(" -> %d", p->to);
        printf(" -> NULL\n");
    }

    /* 4. CSR */
    Csr csr = csr_build(N, G1, M);
    printf("\n4) CSR:\n  row_ptr = [");
    for (int u = 0; u <= N; u++)
        printf("%d%s", csr.row_ptr[u], u < N ? ", " : "]\n");
    printf("  col     = [");
    for (int k = 0; k < 2 * M; k++)
        printf("%d%s", csr.col[k], k < 2 * M - 1 ? ", " : "]\n");
    printf("  neighbours of 4 = col[row_ptr[4]..row_ptr[5]) = col[%d..%d) = {",
           csr.row_ptr[4], csr.row_ptr[5]);
    for (int k = csr.row_ptr[4]; k < csr.row_ptr[5]; k++)
        printf("%d%s", csr.col[k], k < csr.row_ptr[5] - 1 ? ", " : "}\n");

    /* Known answers for G1 */
    static const int expect_row_ptr[N + 1] = {0, 2, 5, 7, 9, 12, 14, 16, 18};
    for (int u = 0; u <= N; u++)
        CHECK(csr.row_ptr[u] == expect_row_ptr[u]);

    /* Cross-check: all four agree on every ordered pair (u, v). O(n^2 * stuff) */
    int arcs = 0;
    for (int u = 0; u < N; u++) {
        int deg = 0;
        for (int v = 0; v < N; v++) {
            bool a = mat[u * N + v] != 0;
            bool b = list_has(&al, u, v);
            bool c = csr_has(&csr, u, v);
            bool d = edgelist_has(G1, M, u, v);
            CHECK(a == b && b == c && c == d);
            CHECK(mat[u * N + v] == mat[v * N + u]);   /* undirected = symmetric */
            deg += a;
        }
        CHECK(deg == csr.row_ptr[u + 1] - csr.row_ptr[u]);
        arcs += deg;
    }
    CHECK(arcs == 2 * M);   /* handshake lemma: sum of degrees = 2|E| */
    printf("\nAll 4 representations agree on all %d vertex pairs; "
           "sum of degrees = %d = 2*|E|  (handshake lemma)\n", N * N, arcs);

    /* Memory comparison: here, and for a road-network-sized sparse graph. */
    printf("\nMemory for THIS graph (n=%d, m=%d), 4-byte ints, 8-byte pointers:\n", N, M);
    printf("  matrix (1 byte/cell) : %zu bytes\n", (size_t)N * N);
    printf("  linked lists         : %zu bytes (heads+tails + 2m nodes of %zu)\n",
           2 * N * sizeof(AdjNode *) + 2 * M * sizeof(AdjNode), sizeof(AdjNode));
    printf("  edge list            : %zu bytes\n", M * sizeof(EdgeUV));
    printf("  CSR                  : %zu bytes\n", (N + 1 + 2 * M) * sizeof(int));

    const double n = 1e6, m = 5e6;   /* e.g. a city road graph */
    printf("\nFor n=1e6, m=5e6 (sparse, avg degree 10):\n");
    printf("  matrix (1 bit/cell)  : %.0f GB   <- impossible\n", n * n / 8 / 1e9);
    printf("  linked lists         : %.0f MB  (16-byte nodes, poor locality)\n",
           (2 * m * 16 + n * 16) / 1e6);
    printf("  CSR                  : %.0f MB   (flat, cache/GPU friendly)\n",
           ((n + 1) * 4 + 2 * m * 4) / 1e6);

    csr_free(&csr);
    list_free(&al);
    free(mat);
    printf("\nOK\n");
    return 0;
}
