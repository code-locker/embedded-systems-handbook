/*
 * 05_lru_cache.cpp — LRU cache preview: std::list + std::unordered_map
 *
 * Concept : A fixed-capacity cache that evicts the Least Recently Used entry.
 *           Hash map gives O(1) "where is key k?"; a doubly linked list keeps
 *           recency order with O(1) move-to-front and pop-back. The map stores
 *           LIST ITERATORS, which std::list never invalidates on splice.
 *           (Full treatment in 19-system-design-structures.)
 * Build   : make run P=dsa/08-hashing/cpp/05_lru_cache.cpp
 * Read    : ../README.md  (section "Where it is used" -> "Caches")
 */
#include <cassert>
#include <cstddef>
#include <iostream>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

/*
 *   index_ (hash)                order_ (list, front = most recent)
 *   +------+------+
 *   | "c"  |  ●───┼───────►  [c:3] <──> [a:1] <──> [b:2]
 *   | "a"  |  ●───┼──────────────────────┘           ▲
 *   | "b"  |  ●───┼──────────────────────────────────┘
 *   +------+------+                                  LRU (evict here)
 *
 *   get("b"):  splice b to the front, O(1), no allocation, iterators stay valid
 *              [b:2] <──> [c:3] <──> [a:1]
 *   put("d") when full: evict back ("a"): erase from map AND list.
 */
template <typename K, typename V>
class LruCache {
    using Entry = std::pair<K, V>;
    std::size_t cap_;
    std::list<Entry> order_;
    std::unordered_map<K, typename std::list<Entry>::iterator> index_;

public:
    explicit LruCache(std::size_t cap) : cap_(cap) { assert(cap > 0); }

    // O(1) average
    std::optional<V> get(const K &k) {
        auto it = index_.find(k);
        if (it == index_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second); // move to front
        return it->second->second;
    }

    // O(1) average
    void put(const K &k, V v) {
        if (auto it = index_.find(k); it != index_.end()) {
            it->second->second = std::move(v);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        if (order_.size() == cap_) {
            index_.erase(order_.back().first); // erase map entry FIRST: it
            order_.pop_back();                 // holds an iterator into the list
        }
        order_.emplace_front(k, std::move(v));
        index_[k] = order_.begin();
    }

    std::size_t size() const { return order_.size(); }
    const K &lru_key() const { return order_.back().first; }
};

int main() {
    LruCache<std::string, int> c(3);
    c.put("a", 1);
    c.put("b", 2);
    c.put("c", 3);              // order: c b a
    assert(c.get("a") == 1);    // order: a c b
    c.put("d", 4);              // full: evict LRU = b. order: d a c
    assert(!c.get("b"));
    assert(c.get("c") == 3);    // order: c d a
    assert(c.lru_key() == "a");
    c.put("a", 10);             // update + touch. order: a c d
    assert(c.lru_key() == "d");
    c.put("e", 5);              // evict d
    assert(!c.get("d") && c.get("a") == 10 && c.get("e") == 5 && c.size() == 3);

    // Simulated page cache: 4 frames, access pattern with locality
    LruCache<int, int> pages(4);
    int hits = 0, misses = 0;
    for (int rep = 0; rep < 100; ++rep)
        for (int p : {1, 2, 3, 1, 2, 4, 1, 2, 5}) {
            if (pages.get(p)) ++hits;
            else { ++misses; pages.put(p, p * 4096); }
        }
    std::cout << "page cache (4 frames): hits " << hits << ", misses " << misses << "\n";
    assert(hits > misses);

    std::cout << "all LRU checks passed\n";
    return 0;
}
