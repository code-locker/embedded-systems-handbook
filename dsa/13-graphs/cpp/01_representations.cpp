/*
 * 01_representations.cpp — one graph, four ways: adjacency matrix,
 *                          vector<vector<int>>, edge list, CSR
 *
 * Concept : The same 8-vertex undirected graph G1 stored four ways with STL
 *           containers. vector<vector<int>> is the everyday C++ adjacency
 *           list; CSR (two flat vectors) is what HPC / GPU / database graph
 *           engines use because a vertex's neighbours are one contiguous
 *           span. All four are cross-checked on every vertex pair.
 * Build   : make run P=dsa/13-graphs/cpp/01_representations.cpp
 * Read    : ../README.md and ../notes/02-representations.md
 *
 *       0 ----- 1 ----- 2
 *       |       |       |
 *       3 ----- 4       5
 *               |       |
 *               6 ----- 7
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

using EdgeList = std::vector<std::pair<int, int>>;

// --- adjacency matrix: n*n cells in ONE vector (row-major), O(1) edge test.
class AdjMatrix {
public:
    AdjMatrix(int n, const EdgeList &edges) : n_(n), cell_(static_cast<size_t>(n) * n, 0)
    {
        for (auto [u, v] : edges) { at(u, v) = 1; at(v, u) = 1; }
    }
    bool has(int u, int v) const { return cell_[static_cast<size_t>(u) * n_ + v] != 0; }
private:
    unsigned char &at(int u, int v) { return cell_[static_cast<size_t>(u) * n_ + v]; }
    int n_;
    std::vector<unsigned char> cell_;
};

// --- CSR: offsets (n+1) + targets (2m). Immutable after construction.
struct Csr {
    std::vector<int> row_ptr, col;

    Csr(int n, const EdgeList &edges) : row_ptr(static_cast<size_t>(n) + 1, 0), col(2 * edges.size())
    {
        for (auto [u, v] : edges) { ++row_ptr[u + 1]; ++row_ptr[v + 1]; }    // degrees
        std::partial_sum(row_ptr.begin(), row_ptr.end(), row_ptr.begin());  // prefix sum
        std::vector<int> cursor(row_ptr.begin(), row_ptr.end() - 1);
        for (auto [u, v] : edges) { col[cursor[u]++] = v; col[cursor[v]++] = u; }  // scatter
    }
    // C++20 would return std::span<const int>; a pointer pair works in C++17.
    std::pair<const int *, const int *> neighbours(int u) const
    {
        return { col.data() + row_ptr[u], col.data() + row_ptr[u + 1] };
    }
    bool has(int u, int v) const
    {
        auto [b, e] = neighbours(u);
        return std::find(b, e, v) != e;
    }
};

int main()
{
    const int n = 8;
    const EdgeList edges = { {0,1},{0,3},{1,2},{1,4},{2,5},{3,4},{4,6},{5,7},{6,7} };

    // vector<vector<int>>: the default choice in C++. push_back is amortised O(1).
    std::vector<std::vector<int>> adj(n);
    for (auto [u, v] : edges) { adj[u].push_back(v); adj[v].push_back(u); }

    AdjMatrix mat(n, edges);
    Csr csr(n, edges);

    std::printf("edge list:");
    for (auto [u, v] : edges) std::printf(" %d-%d", u, v);
    std::printf("\n\nvector<vector<int>> adjacency list:\n");
    for (int u = 0; u < n; u++) {
        std::printf("  adj[%d] = {", u);
        for (size_t i = 0; i < adj[u].size(); i++) std::printf("%s%d", i ? ", " : "", adj[u][i]);
        std::printf("}\n");
    }
    std::printf("\nadjacency matrix:\n");
    for (int u = 0; u < n; u++) {
        std::printf("  ");
        for (int v = 0; v < n; v++) std::printf("%d ", mat.has(u, v) ? 1 : 0);
        std::printf("\n");
    }
    std::printf("\nCSR row_ptr:");
    for (int x : csr.row_ptr) std::printf(" %d", x);
    std::printf("\nCSR col    :");
    for (int x : csr.col) std::printf(" %d", x);
    std::printf("\n");

    CHECK((csr.row_ptr == std::vector<int>{0, 2, 5, 7, 9, 12, 14, 16, 18}));

    auto in_edge_list = [&edges](int a, int b) {
        return std::any_of(edges.begin(), edges.end(), [a, b](const std::pair<int, int> &e) {
            return (e.first == a && e.second == b) || (e.first == b && e.second == a);
        });
    };
    size_t degree_sum = 0;
    for (int u = 0; u < n; u++) {
        for (int v = 0; v < n; v++) {
            bool a = mat.has(u, v);
            bool b = std::find(adj[u].begin(), adj[u].end(), v) != adj[u].end();
            CHECK(a == b && b == csr.has(u, v) && a == in_edge_list(u, v));
        }
        auto [bg, en] = csr.neighbours(u);
        CHECK(static_cast<size_t>(en - bg) == adj[u].size());
        // CSR keeps the same per-row order as the push_back adjacency list
        CHECK(std::equal(bg, en, adj[u].begin()));
        degree_sum += adj[u].size();
    }
    CHECK(degree_sum == 2 * edges.size());   // handshake lemma
    std::printf("\nall four representations agree; sum of degrees = %zu = 2|E|\n", degree_sum);

    std::printf("\nsizes (bytes of payload): matrix %d, vector<vector> ~%zu (+24 per inner vector),"
                " CSR %zu\n", n * n, degree_sum * sizeof(int),
                (csr.row_ptr.size() + csr.col.size()) * sizeof(int));
    std::printf("OK\n");
    return 0;
}
