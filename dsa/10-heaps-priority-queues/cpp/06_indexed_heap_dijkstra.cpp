/*
 * 06_indexed_heap_dijkstra.cpp — indexed min-priority-queue with decrease-key,
 *                                used for Dijkstra's shortest paths
 *
 * Concept : A plain heap cannot find "key 17" without an O(n) scan. An INDEXED
 *           heap keeps pos[key] = where that key sits in the heap array, so
 *           decrease_key / erase / contains are O(log n) / O(1). This is the
 *           textbook structure behind Dijkstra and Prim, and the same trick as
 *           the timer queue's heap_idx. Compared with the "lazy" approach
 *           (push duplicates, skip stale) on random graphs and checked against
 *           Bellman-Ford.
 * Build   : make run P=dsa/10-heaps-priority-queues/cpp/06_indexed_heap_dijkstra.cpp
 * Read    : ../README.md  (section "Indexed heap / decrease-key")
 */
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <utility>
#include <vector>

// Keys are 0..n-1 (e.g. graph vertices). Priority type P, smaller = first.
template <class P>
class IndexedMinHeap {
public:
    explicit IndexedMinHeap(std::size_t n) : pos_(n, npos), prio_(n) {}

    bool        empty() const { return heap_.empty(); }
    std::size_t size()  const { return heap_.size(); }
    bool contains(std::size_t k) const { return pos_[k] != npos; }       // O(1)
    const P& priority(std::size_t k) const { assert(contains(k)); return prio_[k]; }

    void push(std::size_t k, P p)                                        // O(log n)
    {
        assert(!contains(k));
        prio_[k] = std::move(p);
        heap_.push_back(k);
        pos_[k] = heap_.size() - 1;
        sift_up(pos_[k]);
    }

    /*
     * decrease_key — O(log n): a smaller priority can only move a node UP.
     *
     *   heap (key:prio)        decrease C to 1      C swaps with A
     *        A:3                    A:3                   C:1
     *       /   \                  /   \                 /   \
     *     B:5   C:7     ->       B:5   C:1     ->      B:5   A:3
     *   pos[C] tells us where C is without searching.
     */
    void decrease_key(std::size_t k, P p)
    {
        assert(contains(k) && !(prio_[k] < p));
        prio_[k] = std::move(p);
        sift_up(pos_[k]);
    }

    std::pair<std::size_t, P> pop()                                      // O(log n)
    {
        assert(!empty());
        std::size_t k = heap_.front();
        swap_slots(0, heap_.size() - 1);
        heap_.pop_back();
        pos_[k] = npos;
        if (!heap_.empty()) sift_down(0);
        return {k, prio_[k]};
    }

    void erase(std::size_t k)                                            // O(log n)
    {
        std::size_t i = pos_[k];
        assert(i != npos);
        swap_slots(i, heap_.size() - 1);
        heap_.pop_back();
        pos_[k] = npos;
        // The moved-in last element came from another subtree: it may need to
        // go up OR down (never both), exactly like cancelling a timer.
        if (i < heap_.size()) {
            if (i > 0 && less(i, (i - 1) / 2)) sift_up(i);
            else                              sift_down(i);
        }
    }

    bool valid() const
    {
        for (std::size_t i = 0; i < heap_.size(); ++i) {
            if (pos_[heap_[i]] != i) return false;
            if (i && prio_[heap_[i]] < prio_[heap_[(i - 1) / 2]]) return false;
        }
        return true;
    }

private:
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    bool less(std::size_t i, std::size_t j) const { return prio_[heap_[i]] < prio_[heap_[j]]; }

    void swap_slots(std::size_t i, std::size_t j)   // the ONLY place slots move
    {
        std::swap(heap_[i], heap_[j]);
        pos_[heap_[i]] = i;
        pos_[heap_[j]] = j;
    }

    void sift_up(std::size_t i)
    {
        while (i > 0 && less(i, (i - 1) / 2)) { swap_slots(i, (i - 1) / 2); i = (i - 1) / 2; }
    }

    void sift_down(std::size_t i)
    {
        for (;;) {
            std::size_t l = 2 * i + 1, r = l + 1, m = i;
            if (l < heap_.size() && less(l, m)) m = l;
            if (r < heap_.size() && less(r, m)) m = r;
            if (m == i) return;
            swap_slots(i, m);
            i = m;
        }
    }

    std::vector<std::size_t> heap_;   // heap of keys
    std::vector<std::size_t> pos_;    // key -> slot in heap_, or npos
    std::vector<P>           prio_;   // key -> priority
};

struct Edge { std::size_t to; std::int64_t w; };
using Graph = std::vector<std::vector<Edge>>;
constexpr std::int64_t INF = std::numeric_limits<std::int64_t>::max() / 4;

// Dijkstra with an indexed heap: each vertex in the heap at most once.
// O((V + E) log V); heap size <= V.
static std::vector<std::int64_t> dijkstra_indexed(const Graph& g, std::size_t src)
{
    std::vector<std::int64_t> dist(g.size(), INF);
    IndexedMinHeap<std::int64_t> pq(g.size());
    dist[src] = 0;
    pq.push(src, 0);
    while (!pq.empty()) {
        auto [u, d] = pq.pop();
        for (const Edge& e : g[u]) {
            std::int64_t nd = d + e.w;
            if (nd < dist[e.to]) {
                bool first_time = dist[e.to] == INF;
                dist[e.to] = nd;
                if (pq.contains(e.to))  pq.decrease_key(e.to, nd);   // update in place
                else if (first_time)    pq.push(e.to, nd);
            }
        }
    }
    return dist;
}

// Dijkstra with std::priority_queue and lazy deletion: push duplicates, skip
// stale pops. O((V + E) log E); heap may hold up to E entries. Simpler, and
// usually just as fast in practice.
static std::vector<std::int64_t> dijkstra_lazy(const Graph& g, std::size_t src)
{
    using QE = std::pair<std::int64_t, std::size_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    std::vector<std::int64_t> dist(g.size(), INF);
    dist[src] = 0;
    pq.emplace(0, src);
    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u]) continue;                 // stale entry: a shorter path won
        for (const Edge& e : g[u])
            if (d + e.w < dist[e.to]) { dist[e.to] = d + e.w; pq.emplace(dist[e.to], e.to); }
    }
    return dist;
}

// Reference: Bellman-Ford, O(V * E), obviously correct.
static std::vector<std::int64_t> bellman_ford(const Graph& g, std::size_t src)
{
    std::vector<std::int64_t> dist(g.size(), INF);
    dist[src] = 0;
    for (std::size_t it = 0; it + 1 < g.size(); ++it)
        for (std::size_t u = 0; u < g.size(); ++u)
            if (dist[u] != INF)
                for (const Edge& e : g[u])
                    if (dist[u] + e.w < dist[e.to]) dist[e.to] = dist[u] + e.w;
    return dist;
}

int main()
{
    // ---- indexed heap vs a brute-force array of priorities --------------------
    std::mt19937 rng(8);
    const std::size_t N = 64;
    IndexedMinHeap<int> h(N);
    std::vector<int> ref(N, -1);                   // -1 = absent
    for (int step = 0; step < 20000; ++step) {
        std::size_t k = rng() % N;
        int op = static_cast<int>(rng() % 4);
        if (op == 0 && ref[k] < 0) {
            int p = static_cast<int>(rng() % 1000);
            h.push(k, p); ref[k] = p;
        } else if (op == 1 && ref[k] > 0) {
            int p = static_cast<int>(rng() % static_cast<unsigned>(ref[k]));
            h.decrease_key(k, p); ref[k] = p;
        } else if (op == 2 && ref[k] >= 0) {
            h.erase(k); ref[k] = -1;
        } else if (op == 3 && !h.empty()) {
            int best = std::numeric_limits<int>::max();
            for (int p : ref) if (p >= 0 && p < best) best = p;
            auto [key, p] = h.pop();
            assert(p == best && ref[key] == best);
            ref[key] = -1;
        }
        assert(h.valid());
        for (std::size_t i = 0; i < N; ++i) assert(h.contains(i) == (ref[i] >= 0));
    }
    std::cout << "20000 random push/decrease/erase/pop steps match brute force\n";

    // ---- small worked graph -----------------------------------------------------
    //   0 --4--> 1 --1--> 3
    //   0 --1--> 2 --2--> 1 ,  2 --5--> 3
    Graph g(4);
    g[0] = {{1, 4}, {2, 1}};
    g[2] = {{1, 2}, {3, 5}};
    g[1] = {{3, 1}};
    auto d = dijkstra_indexed(g, 0);
    std::cout << "dist from 0: " << d[0] << ' ' << d[1] << ' ' << d[2] << ' ' << d[3] << '\n';
    assert((d == std::vector<std::int64_t>{0, 3, 1, 4}));

    // ---- random graphs: indexed == lazy == Bellman-Ford -------------------------
    for (int round = 0; round < 60; ++round) {
        std::size_t V = rng() % 60 + 1, E = rng() % (V * 4 + 1);
        Graph rg(V);
        for (std::size_t i = 0; i < E; ++i)
            rg[rng() % V].push_back({rng() % V, static_cast<std::int64_t>(rng() % 100)});
        std::size_t s = rng() % V;
        auto a = dijkstra_indexed(rg, s), b = dijkstra_lazy(rg, s), c = bellman_ford(rg, s);
        assert(a == c && b == c);
    }
    std::cout << "60 random graphs: indexed-heap Dijkstra == lazy Dijkstra == Bellman-Ford\n";
    std::cout << "all checks passed\n";
    return 0;
}
