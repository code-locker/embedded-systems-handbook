/*
 * 03_vector_vs_list.cpp — "list insertion is O(1)!" ... but you have to FIND the spot first
 *
 * Concept : Insert n random values keeping the sequence sorted.
 *             std::vector: linear search O(n) + shift O(n) per insert
 *             std::list  : linear search O(n) + link O(1) per insert
 *           Both are O(n^2) overall. The list "wins" on paper for the insert
 *           step, but the search walks pointer-chased nodes (cache misses)
 *           while the vector's search and memmove stream through contiguous
 *           memory. Result: vector is usually several times FASTER, and uses
 *           far less memory (list node = value + 2 pointers + malloc header).
 *           (Bjarne Stroustrup's classic demonstration.)
 * Build   : make run P=dsa/04-complexity-analysis/cpp/03_vector_vs_list.cpp
 *           Real numbers: make OPT=1 SAN=0 run P=dsa/04-complexity-analysis/cpp/03_vector_vs_list.cpp
 * Read    : ../README.md  (section "3.8 Cache effects")
 */
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <iterator>
#include <list>
#include <random>
#include <vector>

template <typename Seq>
static double sorted_inserts(const std::vector<int> &values, Seq &seq)
{
    auto t0 = std::chrono::steady_clock::now();
    for (int x : values) {
        auto it = seq.begin();
        while (it != seq.end() && *it < x) ++it;   // the SAME linear search for both
        seq.insert(it, x);
    }
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

template <typename Seq>
static double traverse(const Seq &seq, long long &sum)
{
    auto t0 = std::chrono::steady_clock::now();
    sum = 0;
    for (int x : seq) sum += x;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main()
{
    std::mt19937 rng(2024);
    std::printf("%7s %14s %14s %8s\n", "n", "vector (ms)", "list (ms)", "list/vec");
    for (int n : {1000, 4000, 12000}) {
        std::vector<int> values(n);
        for (auto &v : values) v = static_cast<int>(rng() % 1000000);
        std::vector<int> vec;
        std::list<int> lst;
        double tv = sorted_inserts(values, vec);
        double tl = sorted_inserts(values, lst);
        assert(std::is_sorted(vec.begin(), vec.end()));
        assert(std::equal(vec.begin(), vec.end(), lst.begin()));
        std::printf("%7d %14.2f %14.2f %8.1fx\n", n, tv * 1e3, tl * 1e3, tl / tv);
    }

    // Pure traversal of 1M elements after the list has been "aged" (nodes scattered).
    const int N = 1 << 20;
    std::vector<int> vec(N, 1);
    std::list<int> lst;
    std::vector<std::list<int>::iterator> its;
    for (int i = 0; i < N; i++) lst.push_back(1);
    // Scatter: splice nodes into random order (same nodes, shuffled links).
    for (auto it = lst.begin(); it != lst.end(); ++it) its.push_back(it);
    std::shuffle(its.begin(), its.end(), rng);
    std::list<int> scattered;
    for (auto &it : its) scattered.splice(scattered.end(), lst, it);
    long long s1 = 0, s2 = 0;
    double t_vec = traverse(vec, s1);
    double t_lst = traverse(scattered, s2);
    assert(s1 == N && s2 == N);
    std::printf("\ntraverse %d ints: vector %.2f ms, scattered list %.2f ms (x%.1f)\n",
                N, t_vec * 1e3, t_lst * 1e3, t_lst / t_vec);
    std::printf("memory per element: vector %zu B, list ~%zu B (value + 2 pointers + malloc overhead)\n",
                sizeof(int), sizeof(int) + 2 * sizeof(void *) + 8);
#if defined(__SANITIZE_ADDRESS__) || !defined(__OPTIMIZE__)
    std::printf("\nNOTE: this is an -O0/ASan build. Unoptimised vector iterators are function\n"
                "calls, which hides the cache effect in the insert test. With\n"
                "  make OPT=1 SAN=0 run P=dsa/04-complexity-analysis/cpp/03_vector_vs_list.cpp\n"
                "a typical result is vector 2-6x faster for inserts and ~100x for traversal.\n");
#endif
    std::printf("All checks passed.\n");
    return 0;
}
