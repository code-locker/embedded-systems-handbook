/*
 * 01_lru_cache.cpp — generic LRU cache: std::list + std::unordered_map<K, list::iterator>
 *
 * Concept : Same design as the C version, but the STL does the plumbing.
 *           std::list::splice() moves a node to the front in O(1) WITHOUT
 *           invalidating iterators, so the map can keep iterators into the
 *           list safely. This is the idiomatic C++ LRU (and the interview one).
 * Build   : make run P=dsa/19-system-design-structures/cpp/01_lru_cache.cpp
 * Read    : ../README.md  (section "LRU cache")
 */
#include <cassert>
#include <cstdio>
#include <iostream>
#include <list>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

/*
 *   map: key -> iterator ----------------+
 *                                        v
 *   list: front [MRU] <-> ... <-> [k,v] <-> ... <-> [LRU] back
 *   get(k): splice(begin, list, it)   -> node moves, iterator stays valid
 *   put new when full: erase map[back.key], pop_back
 */
template <typename K, typename V>
class LruCache {
public:
    explicit LruCache(std::size_t capacity) : cap_(capacity) { map_.reserve(capacity * 2); }

    // O(1) average. Returns a copy in std::optional: a pointer/reference into
    // the cache would dangle after the next put() evicts it.
    std::optional<V> get(const K& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        items_.splice(items_.begin(), items_, it->second);   // touch
        return it->second->second;
    }

    void put(const K& key, V value) {
        if (cap_ == 0) return;
        if (auto it = map_.find(key); it != map_.end()) {
            it->second->second = std::move(value);
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        if (items_.size() == cap_) {
            map_.erase(items_.back().first);   // erase from map BEFORE the node dies
            items_.pop_back();
            ++evictions_;
        }
        items_.emplace_front(key, std::move(value));
        map_[key] = items_.begin();
    }

    std::size_t size() const { return items_.size(); }
    std::size_t evictions() const { return evictions_; }

    template <typename F>
    void for_each_mru(F f) const { for (const auto& kv : items_) f(kv.first, kv.second); }

private:
    using Item = std::pair<K, V>;
    std::size_t cap_;
    std::size_t evictions_ = 0;
    std::list<Item> items_;                                   // owns the entries
    std::unordered_map<K, typename std::list<Item>::iterator> map_;
};

// Reference model: vector ordered by recency, O(n) per op.
template <typename K, typename V>
struct RefLru {
    std::size_t cap;
    std::vector<std::pair<K, V>> v;   // front = MRU
    std::optional<V> get(const K& k) {
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i].first == k) {
                auto kv = v[i];
                v.erase(v.begin() + static_cast<long>(i));
                v.insert(v.begin(), kv);
                return kv.second;
            }
        return std::nullopt;
    }
    void put(const K& k, V val) {
        if (cap == 0) return;
        for (std::size_t i = 0; i < v.size(); ++i)
            if (v[i].first == k) { v.erase(v.begin() + static_cast<long>(i)); break; }
        if (v.size() == cap) v.pop_back();
        v.insert(v.begin(), {k, val});
    }
};

int main() {
    std::cout << "== string -> string cache, capacity 2 (like a DNS/resolver cache) ==\n";
    LruCache<std::string, std::string> dns(2);
    dns.put("beagle.local", "192.168.7.2");
    dns.put("nas.local", "192.168.1.10");
    assert(dns.get("beagle.local") == "192.168.7.2");   // beagle now MRU
    dns.put("router.local", "192.168.1.1");             // evicts nas.local
    assert(!dns.get("nas.local"));
    dns.for_each_mru([](const std::string& k, const std::string& v) {
        std::cout << "  " << k << " -> " << v << "\n";
    });
    assert(dns.size() == 2 && dns.evictions() == 1);

    std::cout << "\n== randomised check vs reference (int keys) ==\n";
    std::mt19937 rng(42);
    for (int trial = 0; trial < 60; ++trial) {
        std::size_t cap = rng() % 12;
        LruCache<int, int> c(cap);
        RefLru<int, int> r{cap, {}};
        for (int op = 0; op < 3000; ++op) {
            int key = static_cast<int>(rng() % 30);
            if (rng() & 1) {
                int val = static_cast<int>(rng());
                c.put(key, val);
                r.put(key, val);
            } else {
                assert(c.get(key) == r.get(key));
            }
            assert(c.size() == r.v.size());
        }
    }
    std::cout << "60 trials x 3000 ops agree (capacity 0..11)\n";
    std::cout << "OK\n";
    return 0;
}
