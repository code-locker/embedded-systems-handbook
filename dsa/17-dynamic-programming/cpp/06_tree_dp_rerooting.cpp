/*
 * 06_tree_dp_rerooting.cpp — DP on trees with adjacency lists: minimum
 *                            vertex cover, and "rerooting" to get the sum
 *                            of distances from EVERY node in O(n)
 *
 * Concept : Pass 1 (post-order, children before parent) computes answers for
 *           each subtree. Pass 2 (pre-order, parent before children) pushes
 *           the "rest of the tree" information down, so every node gets the
 *           answer as if it were the root — n answers for the price of two
 *           DFS passes instead of n separate O(n) DFS runs.
 * Build   : make run P=dsa/17-dynamic-programming/cpp/06_tree_dp_rerooting.cpp
 * Read    : ../README.md  (section "DP on trees")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <queue>
#include <random>
#include <vector>

#define CHECK(cond) assert(cond)

using Graph = std::vector<std::vector<int>>;

// Iterative DFS from root 0: returns nodes in pre-order and fills parent[].
// Reversing the pre-order gives a valid post-order (children before parent),
// which is all a bottom-up tree DP needs — and no recursion means no stack
// overflow on a 10^6-node path-shaped tree.
std::vector<int> preorder(const Graph &g, std::vector<int> &parent)
{
    std::vector<int> order, stack{0};
    parent.assign(g.size(), -1);
    parent[0] = 0;                      // marks the root as visited
    while (!stack.empty()) {
        const int v = stack.back();
        stack.pop_back();
        order.push_back(v);
        for (int c : g[v])
            if (parent[c] == -1) { parent[c] = v; stack.push_back(c); }
    }
    parent[0] = -1;
    return order;
}

// Min vertex cover: fewest nodes such that every edge touches one of them.
//   in[v]  = 1 + sum over children of min(in[c], out[c])
//   out[v] =     sum over children of in[c]   (v not chosen -> each child edge
//                                              must be covered by the child)
int min_vertex_cover(const Graph &g)
{
    std::vector<int> parent;
    const auto order = preorder(g, parent);
    std::vector<int> in(g.size(), 1), out(g.size(), 0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {   // post-order
        const int v = *it, p = parent[v];
        if (p < 0) continue;
        in[p] += std::min(in[v], out[v]);
        out[p] += in[v];
    }
    return std::min(in[0], out[0]);
}

int cover_brute(const Graph &g)
{
    const unsigned n = static_cast<unsigned>(g.size());
    int best = static_cast<int>(n);
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        bool ok = true;
        for (unsigned v = 0; v < n && ok; ++v)
            for (int u : g[v])
                if (!(mask >> v & 1u) && !(mask >> u & 1u)) ok = false;
        if (ok) best = std::min(best, __builtin_popcount(mask));
    }
    return best;
}

// ------------------------------------------------------------ rerooting --
// ans[v] = sum of distances from v to every other node.
//
// Pass 1 (rooted at 0):
//   size[v] = nodes in v's subtree
//   down[v] = sum of distances from v to nodes in its subtree
//           = sum over children c of (down[c] + size[c])   (+1 per node below c)
// ans[0] = down[0].
//
// Pass 2: moving the root from parent p to child c:
//   the size[c] nodes in c's subtree get 1 closer,
//   the n - size[c] other nodes get 1 farther:
//       ans[c] = ans[p] - size[c] + (n - size[c])
//
/*        p                     p
 *       / \       re-root     / \
 *      c   x      ------>    c   x      c's subtree: -1 each
 *     / \                   / \         everything else: +1 each
 */
// Total O(n) instead of O(n^2).
std::vector<long long> sum_of_distances(const Graph &g)
{
    const long long n = static_cast<long long>(g.size());
    std::vector<int> parent;
    const auto order = preorder(g, parent);
    std::vector<long long> size(g.size(), 1), down(g.size(), 0), ans(g.size(), 0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const int v = *it, p = parent[v];
        if (p < 0) continue;
        size[p] += size[v];
        down[p] += down[v] + size[v];
    }
    ans[0] = down[0];
    for (int v : order)                                       // pre-order: parent first
        if (parent[v] >= 0) ans[v] = ans[parent[v]] - size[v] + (n - size[v]);
    return ans;
}

std::vector<long long> distances_brute(const Graph &g)
{
    std::vector<long long> ans(g.size(), 0);
    for (std::size_t s = 0; s < g.size(); ++s) {
        std::vector<int> dist(g.size(), -1);
        std::queue<int> q;
        dist[s] = 0;
        q.push(static_cast<int>(s));
        while (!q.empty()) {
            const int u = q.front();
            q.pop();
            ans[s] += dist[u];
            for (int x : g[u])
                if (dist[x] < 0) { dist[x] = dist[u] + 1; q.push(x); }
        }
    }
    return ans;
}

Graph random_tree(int n, std::mt19937 &gen)
{
    Graph g(static_cast<std::size_t>(n));
    for (int v = 1; v < n; ++v) {
        const int p = std::uniform_int_distribution<int>(0, v - 1)(gen);
        g[v].push_back(p);
        g[p].push_back(v);
    }
    return g;
}

int main()
{
    //        0
    //      / | \          sum of distances from 0 = 1+1+1+2+2+2 = 9
    //     1  2  3         from 4: to 2 is 1, to 0 is 2, to 5 and 6 is 2,
    //        /|\               to 1 and 3 is 3:  1+2+2+2+3+3 = 13
    //       4 5 6
    Graph g(7);
    auto edge = [&g](int a, int b) { g[a].push_back(b); g[b].push_back(a); };
    edge(0, 1); edge(0, 2); edge(0, 3); edge(2, 4); edge(2, 5); edge(2, 6);

    std::cout << "== Sum of distances via rerooting ==\n";
    const auto ans = sum_of_distances(g);
    for (std::size_t v = 0; v < ans.size(); ++v) std::cout << "  node " << v << ": " << ans[v] << '\n';
    CHECK(ans[0] == 9 && ans[4] == 13 && ans == distances_brute(g));

    std::cout << "\n== Minimum vertex cover ==\n";
    std::cout << "cover size = " << min_vertex_cover(g) << "  ({0, 2})\n";
    CHECK(min_vertex_cover(g) == 2);

    std::mt19937 gen(31);
    for (int t = 0; t < 200; ++t) {
        const Graph r = random_tree(std::uniform_int_distribution<int>(1, 14)(gen), gen);
        CHECK(sum_of_distances(r) == distances_brute(r));
        CHECK(min_vertex_cover(r) == cover_brute(r));
    }
    std::cout << "\n200 random trees: rerooting == BFS from every node; cover == brute force\n";

    const Graph big = random_tree(200000, gen);                  // O(n): instant
    const auto big_ans = sum_of_distances(big);
    std::cout << "200000-node tree: min sum of distances = "
              << *std::min_element(big_ans.begin(), big_ans.end())
              << " (O(n) rerooting; all-pairs BFS would be 4*10^10 steps)\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
