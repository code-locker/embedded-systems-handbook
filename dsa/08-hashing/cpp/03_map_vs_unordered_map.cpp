/*
 * 03_map_vs_unordered_map.cpp — std::map (red-black tree) vs
 *                               std::unordered_map (hash table)
 *
 * Concept : map keeps keys SORTED: O(log n) ops, ordered iteration, range
 *           queries (lower_bound), needs only operator<. unordered_map is
 *           O(1) average but unordered, needs hash + ==, and has a worst
 *           case of O(n). We demonstrate each difference and time them.
 * Build   : make run P=dsa/08-hashing/cpp/03_map_vs_unordered_map.cpp
 * Read    : ../README.md  (section "C vs C++" -> "map vs unordered_map")
 */
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

template <typename F> static double ms(F &&f) {
    auto t0 = std::chrono::steady_clock::now();
    f();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    // 1. ORDER: map iterates sorted; unordered_map in bucket order.
    std::map<std::string, int> ordered;
    std::unordered_map<std::string, int> hashed;
    for (const char *k : {"spi0", "can0", "uart2", "i2c1", "eth0"}) {
        ordered[k] = 1;
        hashed[k] = 1;
    }
    std::string prev;
    std::cout << "map order:           ";
    for (auto &[k, v] : ordered) {
        (void)v;
        assert(prev < k); // strictly increasing, guaranteed
        prev = k;
        std::cout << k << ' ';
    }
    std::cout << "\nunordered_map order: ";
    for (auto &kv : hashed) std::cout << kv.first << ' ';
    std::cout << " (implementation-defined, may change on rehash)\n";

    // 2. RANGE QUERIES: only the tree can do these in O(log n + k).
    //    "all timer events between t=100 and t=200"
    std::map<std::uint64_t, std::string> timeline{
        {50, "boot"}, {120, "irq"}, {150, "dma"}, {199, "irq"}, {250, "idle"}};
    std::vector<std::string> window;
    for (auto it = timeline.lower_bound(100); it != timeline.upper_bound(200); ++it)
        window.push_back(it->second);
    assert((window == std::vector<std::string>{"irq", "dma", "irq"}));
    // nearest key <= 140 ("which mapping contains address 140?"):
    auto it = timeline.upper_bound(140);
    --it;
    assert(it->first == 120);
    // smallest / largest in O(1)/O(log n): begin(), rbegin()
    assert(timeline.begin()->first == 50 && timeline.rbegin()->first == 250);
    // With unordered_map each of these would need a full O(n) scan.

    // 3. SPEED for pure point lookups (unordered usually wins, ~2-5x).
    const int N = 200000;
    std::map<int, int> tm;
    std::unordered_map<int, int> um;
    um.reserve(N);
    double t_ins_map = ms([&] { for (int i = 0; i < N; ++i) tm[i * 7] = i; });
    double t_ins_um = ms([&] { for (int i = 0; i < N; ++i) um[i * 7] = i; });
    long long s1 = 0, s2 = 0;
    double t_find_map = ms([&] { for (int i = 0; i < N; ++i) s1 += tm.find(i * 7)->second; });
    double t_find_um = ms([&] { for (int i = 0; i < N; ++i) s2 += um.find(i * 7)->second; });
    assert(s1 == s2);
    std::cout << "\n" << N << " ints (debug + sanitizer build, ratios matter, not ms):\n"
              << "  insert: map " << t_ins_map << " ms, unordered_map " << t_ins_um << " ms\n"
              << "  find  : map " << t_find_map << " ms, unordered_map " << t_find_um << " ms\n";

    // 4. MEMORY / NODE COST: both allocate one node per element. A map node
    //    carries 3 pointers + colour; an unordered_map node 1 pointer (+
    //    cached hash for strings) plus the bucket array. Neither is cache
    //    friendly — for small sets, a sorted std::vector + binary_search
    //    often beats both.

    // 5. REQUIREMENTS on the key:
    //    map           : operator< (strict weak ordering)
    //    unordered_map : std::hash<K> + operator==
    //    e.g. std::pair<int,int> works in map out of the box, but has NO
    //    std::hash, so unordered_map<pair<int,int>,..> fails to compile
    //    without a custom hasher (see 02_unordered_containers.cpp).
    std::map<std::pair<int, int>, int> grid{{{1, 2}, 3}};
    assert(grid.at({1, 2}) == 3);

    // 6. WORST CASE: a tree is O(log n) always. A hash table under
    //    adversarial keys (all colliding) degrades to O(n) per op — this is
    //    the "hash flooding" DoS. Defence: randomised/keyed hash (SipHash,
    //    as Python, Rust and the Linux kernel's siphash for net tables use).
    //    The kernel and many real-time systems pick trees where a bounded
    //    worst case matters more than the average.

    std::cout << "\nRule of thumb:\n"
                 "  need order / ranges / predictable worst case -> std::map\n"
                 "  only point lookups, big n                    -> std::unordered_map\n"
                 "  small n, read-mostly                          -> sorted std::vector\n";
    std::cout << "all map-vs-unordered checks passed\n";
    return 0;
}
