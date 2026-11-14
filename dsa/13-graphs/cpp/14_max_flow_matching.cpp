/*
 * 14_max_flow_matching.cpp — Edmonds-Karp on an ADJACENCY-LIST residual
 *                            graph (paired forward/reverse edges), min cut,
 *                            and bipartite matching as a flow problem
 *
 * Concept : The C version uses a V x V capacity matrix (simple, O(V^2)
 *           memory). Real code stores edges in a vector and keeps, for each
 *           edge, the index of its reverse twin: pushing f on e means
 *           cap[e] -= f, cap[twin] += f. Edges are added in pairs, so the
 *           twin of edge i is simply i ^ 1.
 *           Min cut = the edges from {reachable from s in the residual} to
 *           the rest. Matching: s -> left (cap 1), left -> right (cap 1),
 *           right -> t (cap 1); the max flow is the max matching.
 * Build   : make run P=dsa/13-graphs/cpp/14_max_flow_matching.cpp
 * Read    : ../README.md and ../notes/08-advanced.md  (section "Max flow")
 *
 *   G10 arcs: 0->1:16 0->2:13 1->3:12 2->1:4 2->4:14 3->2:9 3->5:20 4->3:7 4->5:4
 */
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

class MaxFlow {
public:
    explicit MaxFlow(int n) : adj_(n) {}

    int add_edge(int u, int v, int cap)          // returns the forward edge's index
    {
        int id = static_cast<int>(to_.size());
        to_.push_back(v); cap_.push_back(cap); adj_[u].push_back(id);   // id     : u->v
        to_.push_back(u); cap_.push_back(0);   adj_[v].push_back(id + 1); // id ^ 1 : v->u (undo)
        return id;
    }

    // Edmonds-Karp: BFS shortest augmenting paths. O(V * E^2).
    long long run(int s, int t, bool trace = false)
    {
        long long flow = 0;
        for (;;) {
            std::vector<int> via(adj_.size(), -1);   // edge used to reach each vertex
            std::queue<int> q;
            q.push(s);
            via[s] = -2;
            while (!q.empty() && via[t] == -1) {
                int u = q.front();
                q.pop();
                for (int e : adj_[u])
                    if (cap_[e] > 0 && via[to_[e]] == -1) { via[to_[e]] = e; q.push(to_[e]); }
            }
            if (via[t] == -1) return flow;           // no augmenting path: done
            int b = INT_MAX;
            for (int v = t; v != s; v = to_[via[v] ^ 1]) b = std::min(b, cap_[via[v]]);
            std::string path;
            for (int v = t; v != s; v = to_[via[v] ^ 1]) {
                cap_[via[v]] -= b;
                cap_[via[v] ^ 1] += b;               // the twin trick
                path = " -> " + std::to_string(v) + path;
            }
            flow += b;
            if (trace) std::printf("  augment %d%s  (+%d, flow %lld)\n", s, path.c_str(), b, flow);
        }
    }

    std::vector<char> reachable_from(int s) const   // S side of the min cut
    {
        std::vector<char> seen(adj_.size(), 0);
        std::queue<int> q;
        q.push(s);
        seen[s] = 1;
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int e : adj_[u]) if (cap_[e] > 0 && !seen[to_[e]]) { seen[to_[e]] = 1; q.push(to_[e]); }
        }
        return seen;
    }

    int flow_on(int e) const { return cap_[e ^ 1]; }   // reverse capacity == flow pushed

private:
    std::vector<std::vector<int>> adj_;
    std::vector<int> to_, cap_;
};

int main()
{
    const std::vector<std::tuple<int, int, int>> g10 = {
        {0,1,16},{0,2,13},{1,3,12},{2,1,4},{2,4,14},{3,2,9},{3,5,20},{4,3,7},{4,5,4} };
    MaxFlow mf(6);
    std::vector<int> ids;
    for (auto [u, v, c] : g10) ids.push_back(mf.add_edge(u, v, c));
    std::printf("Edmonds-Karp on G10:\n");
    long long f = mf.run(0, 5, true);
    CHECK(f == 23);
    std::printf("flow per edge:");
    for (size_t i = 0; i < g10.size(); i++)
        std::printf(" %d->%d:%d/%d", std::get<0>(g10[i]), std::get<1>(g10[i]), mf.flow_on(ids[i]), std::get<2>(g10[i]));
    auto side = mf.reachable_from(0);
    int cut = 0;
    std::printf("\nmin cut edges:");
    for (auto [u, v, c] : g10) if (side[u] && !side[v]) { std::printf(" %d->%d(%d)", u, v, c); cut += c; }
    std::printf("  = %d = max flow\n", cut);
    CHECK(cut == 23);

    // Matching: which hardware block runs which video stream?
    const std::vector<std::string> streams = { "cam0", "cam1", "cam2", "cam3" };
    const std::vector<std::string> blocks = { "encoder-A", "encoder-B", "ISP", "DSP" };
    const std::vector<std::pair<int, int>> can = { {0,0},{0,1},{1,0},{2,1},{2,2},{3,2},{3,3} };
    const int L = 4, R = 4, S = L + R, T = S + 1;
    MaxFlow bm(L + R + 2);
    std::vector<int> pair_edge;
    for (int l = 0; l < L; l++) bm.add_edge(S, l, 1);
    for (auto [l, r] : can) pair_edge.push_back(bm.add_edge(l, L + r, 1));
    for (int r = 0; r < R; r++) bm.add_edge(L + r, T, 1);
    long long m = bm.run(S, T);
    std::printf("\nmatching size %lld:", m);
    for (size_t i = 0; i < can.size(); i++)
        if (bm.flow_on(pair_edge[i])) std::printf(" %s->%s", streams[can[i].first].c_str(), blocks[can[i].second].c_str());
    std::printf("\n");
    CHECK(m == 4);

    std::mt19937 rng(9001);
    for (int it = 0; it < 300; it++) {
        int n = 2 + static_cast<int>(rng() % 8), mm = static_cast<int>(rng() % (3 * n));
        std::vector<std::vector<int>> cap(n, std::vector<int>(n, 0));
        MaxFlow r(n);
        for (int i = 0; i < mm; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n), c = 1 + static_cast<int>(rng() % 10);
            if (a == b) continue;
            cap[a][b] += c;
            r.add_edge(a, b, c);                     // parallel edges are fine
        }
        long long best = LLONG_MAX;                  // brute-force min cut
        for (unsigned mask = 0; mask < (1u << n); mask++) {
            if (!(mask & 1u) || (mask >> (n - 1) & 1u)) continue;
            long long c = 0;
            for (int u = 0; u < n; u++) for (int v = 0; v < n; v++)
                if ((mask >> u & 1u) && !(mask >> v & 1u)) c += cap[u][v];
            best = std::min(best, c);
        }
        CHECK(r.run(0, n - 1) == best);
    }
    std::printf("random cross-check: 300 networks, max flow == brute-force min cut\nOK\n");
    return 0;
}
