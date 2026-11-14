/*
 * 15_euler_hierholzer.cpp — O(E) Hierholzer for undirected and DIRECTED
 *                           graphs, and a de Bruijn sequence built from an
 *                           Euler circuit
 *
 * Concept : Directed Euler circuit exists iff every vertex has
 *           in-degree == out-degree (and the edges are connected); a path
 *           iff exactly one vertex has out = in + 1 (start) and one has
 *           in = out + 1 (end). Hierholzer with a per-vertex "next edge"
 *           cursor touches each edge once: O(E).
 *           De Bruijn graph: vertices = all (n-1)-bit strings, edge
 *           x -> x' for appending a bit. An Euler circuit visits every n-bit
 *           string exactly once => the shortest cyclic bit string containing
 *           every n-bit pattern. The same idea assembles genomes from short
 *           DNA reads, and drives absolute rotary encoders.
 * Build   : make run P=dsa/13-graphs/cpp/15_euler_hierholzer.cpp
 * Read    : ../README.md and ../notes/08-advanced.md  (section "Euler")
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

// Directed Euler path/circuit. Returns the vertex sequence or nullopt.
std::optional<std::vector<int>> euler_directed(int n, const std::vector<std::pair<int, int>> &arcs)
{
    std::vector<std::vector<int>> out(n);
    std::vector<int> in(n, 0);
    for (auto [u, v] : arcs) { out[u].push_back(v); ++in[v]; }
    int start = -1, plus = 0, minus = 0;
    for (int v = 0; v < n; v++) {
        int diff = static_cast<int>(out[v].size()) - in[v];
        if (diff == 1) { ++plus; start = v; }
        else if (diff == -1) ++minus;
        else if (diff != 0) return std::nullopt;
    }
    if (!((plus == 0 && minus == 0) || (plus == 1 && minus == 1))) return std::nullopt;
    if (start == -1)
        for (int v = 0; v < n; v++) if (!out[v].empty()) { start = v; break; }
    if (start == -1) return std::vector<int>{ 0 };            // no edges at all

    std::vector<size_t> next(n, 0);                           // per-vertex cursor: O(E) total
    std::vector<int> stack{ start }, path;
    while (!stack.empty()) {
        int u = stack.back();
        if (next[u] < out[u].size()) stack.push_back(out[u][next[u]++]);
        else { path.push_back(u); stack.pop_back(); }
    }
    if (path.size() != arcs.size() + 1) return std::nullopt;  // edges not all connected
    std::reverse(path.begin(), path.end());
    return path;
}

// de Bruijn sequence B(2, n): every n-bit string appears exactly once (cyclically).
std::string de_bruijn(int n)
{
    const int nodes = 1 << (n - 1);
    std::vector<std::pair<int, int>> arcs;
    for (int x = 0; x < nodes; x++)
        for (int b = 0; b < 2; b++) arcs.push_back({ x, ((x << 1) | b) & (nodes - 1) });
    auto circ = euler_directed(nodes, arcs);
    CHECK(circ.has_value());
    std::string s;
    for (size_t i = 1; i < circ->size(); i++) s += static_cast<char>('0' + ((*circ)[i] & 1));   // appended bit
    return s;
}

// Brute force existence of a directed Euler trail (tiny graphs only).
bool brute(int u, std::vector<std::pair<int, int>> &arcs, std::vector<char> &used, size_t left)
{
    if (left == 0) return true;
    for (size_t i = 0; i < arcs.size(); i++)
        if (!used[i] && arcs[i].first == u) {
            used[i] = 1;
            bool ok = brute(arcs[i].second, arcs, used, left - 1);
            used[i] = 0;
            if (ok) return true;
        }
    return false;
}

int main()
{
    // Directed example: a little "tour of pipeline states" graph.
    //   0 -> 1 -> 2 -> 0 ,  1 -> 3 -> 1      (two loops sharing vertex 1)
    const std::vector<std::pair<int, int>> g = { {0,1},{1,2},{2,0},{1,3},{3,1} };
    auto c = euler_directed(4, g);
    CHECK(c.has_value());
    std::printf("directed Euler circuit:");
    for (int v : *c) std::printf(" %d", v);
    std::printf("\n");
    CHECK((*c == std::vector<int>{0, 1, 3, 1, 2, 0}));   // loop 1-3-1 spliced into 0-1-2-0

    const std::vector<std::pair<int, int>> bad = { {0,1},{0,2} };   // out(0)=2, two sinks
    CHECK(!euler_directed(3, bad));
    std::printf("0->1, 0->2: no Euler path (vertex 0 has out-in = 2)\n");

    for (int n = 2; n <= 5; n++) {
        std::string s = de_bruijn(n);
        std::set<std::string> seen;
        for (size_t i = 0; i < s.size(); i++) {
            std::string w;
            for (int k = 0; k < n; k++) w += s[(i + static_cast<size_t>(k)) % s.size()];
            seen.insert(w);
        }
        std::printf("de Bruijn B(2,%d) = %s  (length %zu, contains all %zu %d-bit patterns)\n",
                    n, s.c_str(), s.size(), seen.size(), n);
        CHECK(s.size() == (1u << n) && seen.size() == (1u << n));
    }

    std::mt19937 rng(1736);
    for (int it = 0; it < 400; it++) {
        int n = 1 + static_cast<int>(rng() % 5), m = static_cast<int>(rng() % 8);
        std::vector<std::pair<int, int>> arcs;
        for (int i = 0; i < m; i++) arcs.push_back({ static_cast<int>(rng() % n), static_cast<int>(rng() % n) });
        auto r = euler_directed(n, arcs);
        std::vector<char> used(arcs.size(), 0);
        bool exists = arcs.empty();
        for (int s = 0; s < n && !exists; s++) exists = brute(s, arcs, used, arcs.size());
        CHECK(r.has_value() == exists);
        if (r) {                                          // every arc used exactly once
            std::multiset<std::pair<int, int>> want(arcs.begin(), arcs.end()), got;
            for (size_t i = 0; i + 1 < r->size(); i++) got.insert({ (*r)[i], (*r)[i + 1] });
            CHECK(want == got);
        }
    }
    std::printf("random cross-check: 400 directed multigraphs, Hierholzer == brute force\nOK\n");
    return 0;
}
