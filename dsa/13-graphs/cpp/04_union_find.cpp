/*
 * 04_union_find.cpp — a reusable Dsu class (path compression + union by
 *                     rank) and connected components
 *
 * Concept : Each set is a tree; the root names the set. Union by rank keeps
 *           trees O(log n) tall; path compression flattens them further as a
 *           side effect of find(). Together each operation costs alpha(n)
 *           amortised: the inverse Ackermann function, <= 4 for any n that
 *           fits in this universe. Used here for "online" connectivity: links
 *           come up one by one and we ask "are A and B connected yet?".
 * Build   : make run P=dsa/13-graphs/cpp/04_union_find.cpp
 * Read    : ../README.md and ../notes/04-connectivity.md  (section "Union-find")
 *
 *   G3:  0 --- 1 --- 2      3 --- 4      5 --- 6      8      9
 *                                         \   /
 *                                           7
 */
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <queue>
#include <random>
#include <utility>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", \
                         #c, __FILE__, __LINE__); std::abort(); } } while (0)

class Dsu {
public:
    explicit Dsu(int n) : parent_(n), rank_(n, 0), sets_(n) { std::iota(parent_.begin(), parent_.end(), 0); }

    int find(int x)                              // amortised ~O(1)
    {
        int root = x;
        while (parent_[root] != root) root = parent_[root];
        while (parent_[x] != root) {             // compress: everyone points at root
            int next = parent_[x];
            parent_[x] = root;
            x = next;
        }
        return root;
    }

    bool unite(int a, int b)                     // false: already same set (cycle edge)
    {
        a = find(a);
        b = find(b);
        if (a == b) return false;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
        --sets_;
        return true;
    }

    bool same(int a, int b) { return find(a) == find(b); }
    int sets() const { return sets_; }
    const std::vector<int> &parents() const { return parent_; }

private:
    std::vector<int> parent_, rank_;
    int sets_;
};

// BFS component labels, for the cross-check.
std::vector<int> components(const std::vector<std::vector<int>> &g, int &count)
{
    std::vector<int> comp(g.size(), -1);
    count = 0;
    for (size_t s = 0; s < g.size(); s++) {
        if (comp[s] != -1) continue;
        std::queue<int> q;
        q.push(static_cast<int>(s));
        comp[s] = count;
        while (!q.empty()) {
            int u = q.front();
            q.pop();
            for (int v : g[u]) if (comp[v] == -1) { comp[v] = count; q.push(v); }
        }
        ++count;
    }
    return comp;
}

int main()
{
    Dsu d(10);
    const std::vector<std::pair<int, int>> links = { {0,1},{1,2},{3,4},{5,6},{6,7},{7,5} };
    std::printf("links come up one at a time:\n");
    for (auto [a, b] : links) {
        bool merged = d.unite(a, b);
        std::printf("  link %d-%d : %-28s parent =", a, b, merged ? "merged two networks" : "already connected (redundant)");
        for (int p : d.parents()) std::printf(" %d", p);
        std::printf("   networks=%d\n", d.sets());
    }
    CHECK(d.sets() == 5);
    CHECK((d.parents() == std::vector<int>{0, 0, 0, 3, 3, 5, 5, 5, 8, 9}));
    CHECK(d.same(0, 2) && !d.same(2, 3) && d.same(5, 7));
    std::printf("same(0,2)=yes  same(2,3)=no  same(5,7)=yes\n");

    std::mt19937 rng(2024);
    for (int t = 0; t < 300; t++) {
        int n = 1 + static_cast<int>(rng() % 20);
        Dsu r(n);
        std::vector<std::vector<int>> g(n);
        int m = static_cast<int>(rng() % (n + 5));
        for (int i = 0; i < m; i++) {
            int a = static_cast<int>(rng() % n), b = static_cast<int>(rng() % n);
            g[a].push_back(b);
            g[b].push_back(a);
            r.unite(a, b);
        }
        int cnt = 0;
        auto comp = components(g, cnt);
        CHECK(cnt == r.sets());
        for (int a = 0; a < n; a++)
            for (int b = 0; b < n; b++) CHECK((comp[a] == comp[b]) == r.same(a, b));
    }
    std::printf("random cross-check: 300 graphs, Dsu == BFS components\n");

    // One million unions + finds finish instantly: the alpha(n) promise.
    const int big = 1000000;
    Dsu huge(big);
    std::mt19937 r2(7);
    for (int i = 0; i < big; i++) huge.unite(static_cast<int>(r2() % big), static_cast<int>(r2() % big));
    long long finds = 0;
    for (int i = 0; i < big; i++) finds += huge.find(i) >= 0;
    std::printf("1e6 random unions + 1e6 finds done; %d sets left\nOK\n", huge.sets());
    CHECK(finds == big);
    return 0;
}
