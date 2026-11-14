/*
 * 09_tree_dp.c — DP on trees: maximum-weight independent set ("house robber
 *                on a tree"), tree diameter, subtree sizes
 *
 * Concept : Root the tree. Each node's answer is computed from its
 *           children's answers (post-order), so the subproblems are the
 *           subtrees. Often a node needs TWO numbers (e.g. "best if I am
 *           taken" / "best if I am not"), because the parent's choice
 *           depends on which one the child picked.
 * Build   : make run P=dsa/17-dynamic-programming/c/09_tree_dp.c
 * Read    : ../README.md  (section "DP on trees")
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__, \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define MAXN 64

static unsigned rng_state = 555u;
static unsigned rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* The tree is given by parent[] with parent[0] = -1 and parent[i] < i.
 * That ordering is convenient: iterating i from n-1 DOWN to 0 visits every
 * child before its parent — a post-order without recursion or a stack.
 * (For a general tree you would DFS once to produce such an order.) */

/* ------------------------------------------------ max independent set ---
 * Pick nodes with max total weight, no two adjacent (parent-child).
 *   take[v] = w[v] + sum over children c of skip[c]     (children forbidden)
 *   skip[v] =        sum over children c of max(take[c], skip[c])
 * Answer: max(take[root], skip[root]). Time O(n), space O(n).
 *
 *          (0) w=3
 *         /    \
 *     (1) w=4  (2) w=5          take/skip shown as  [take|skip]
 *     /  \        \
 *  (3)1  (4)1     (5)6          leaves: [w|0]
 *                               1: [4+0+0 | 1+1] = [4|2]
 *                               2: [5+0   | 6  ] = [5|6]
 *                               0: [3+2+6 | 4+6] = [11|10]  -> 11 = {0,3,4,5}
 */
static int mis(const int *parent, const int *w, int n, int *take, int *skip)
{
    for (int v = 0; v < n; v++) { take[v] = w[v]; skip[v] = 0; }
    for (int v = n - 1; v >= 1; v--) {           /* children before parents */
        int p = parent[v];
        take[p] += skip[v];
        skip[p] += take[v] > skip[v] ? take[v] : skip[v];
    }
    return take[0] > skip[0] ? take[0] : skip[0];
}

static int mis_brute(const int *parent, const int *w, int n)
{
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        int ok = 1, sum = 0;
        for (int v = 1; v < n && ok; v++)
            if ((mask >> v & 1) && (mask >> parent[v] & 1)) ok = 0;
        if (!ok) continue;
        for (int v = 0; v < n; v++) if (mask >> v & 1) sum += w[v];
        if (sum > best) best = sum;
    }
    return best;
}

/* ---------------------------------------------------------- diameter ----
 * Longest path (in edges) between any two nodes.
 *   down[v] = longest downward path from v into its subtree
 * The longest path that has v as its HIGHEST node joins the two deepest
 * child branches:  (down[c1] + 1) + (down[c2] + 1).
 * Track the top two branch lengths per node while children report in.
 */
static int diameter(const int *parent, int n)
{
    int best1[MAXN] = {0}, best2[MAXN] = {0}, ans = 0;
    for (int v = n - 1; v >= 0; v--) {
        int down = best1[v];                     /* all children already done */
        if (best1[v] + best2[v] > ans) ans = best1[v] + best2[v];
        if (v == 0) break;
        int p = parent[v], len = down + 1;
        if (len > best1[p]) { best2[p] = best1[p]; best1[p] = len; }
        else if (len > best2[p]) best2[p] = len;
    }
    return ans;
}

/* Brute force: BFS from every node over an adjacency matrix. O(n^3). */
static int diameter_brute(const int *parent, int n)
{
    static unsigned char adj[MAXN][MAXN];
    memset(adj, 0, sizeof adj);
    for (int v = 1; v < n; v++) adj[v][parent[v]] = adj[parent[v]][v] = 1;
    int ans = 0;
    for (int s = 0; s < n; s++) {
        int dist[MAXN], q[MAXN], head = 0, tail = 0;
        for (int i = 0; i < n; i++) dist[i] = -1;
        dist[s] = 0; q[tail++] = s;
        while (head < tail) {
            int u = q[head++];
            if (dist[u] > ans) ans = dist[u];
            for (int x = 0; x < n; x++)
                if (adj[u][x] && dist[x] < 0) { dist[x] = dist[u] + 1; q[tail++] = x; }
        }
    }
    return ans;
}

/* --------------------------------------------------------- subtree size --
 * size[v] = 1 + sum of children's sizes. The simplest tree DP of all, and
 * the building block of rerooting, centroid decomposition, HLD...
 */
static void subtree_sizes(const int *parent, int n, int *size)
{
    for (int v = 0; v < n; v++) size[v] = 1;
    for (int v = n - 1; v >= 1; v--) size[parent[v]] += size[v];
}

int main(void)
{
    printf("== Max-weight independent set on the 6-node tree from the comment ==\n");
    const int par[] = {-1, 0, 0, 1, 1, 2}, w[] = {3, 4, 5, 1, 1, 6};
    int take[MAXN], skip[MAXN];
    int m = mis(par, w, 6, take, skip);
    for (int v = 0; v < 6; v++) printf("  node %d (w=%d): take=%2d skip=%2d\n", v, w[v], take[v], skip[v]);
    printf("best = %d  (nodes 0,3,4,5)\n", m);
    CHECK(m == 11 && mis_brute(par, w, 6) == 11);

    printf("\n== Diameter and subtree sizes ==\n");
    int size[MAXN];
    subtree_sizes(par, 6, size);
    printf("diameter = %d edges (3-1-0-2-5); sizes:", diameter(par, 6));
    for (int v = 0; v < 6; v++) printf(" %d", size[v]);
    printf("\n");
    CHECK(diameter(par, 6) == 4);
    CHECK(size[0] == 6 && size[1] == 3 && size[2] == 2 && size[5] == 1);

    int p[MAXN], wt[MAXN];
    for (int t = 0; t < 300; t++) {
        int n = 1 + (int)(rng() % 16);
        p[0] = -1;
        for (int v = 1; v < n; v++) p[v] = (int)(rng() % (unsigned)v);   /* random tree */
        for (int v = 0; v < n; v++) wt[v] = (int)(rng() % 20);
        CHECK(mis(p, wt, n, take, skip) == mis_brute(p, wt, n));
        CHECK(diameter(p, n) == diameter_brute(p, n));
        subtree_sizes(p, n, size);
        CHECK(size[0] == n);
    }
    printf("300 random trees: MIS == brute force over subsets, diameter == all-pairs BFS\n");

    printf("\nall checks passed\n");
    return 0;
}
