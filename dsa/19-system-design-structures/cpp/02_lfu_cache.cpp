/*
 * 02_lfu_cache.cpp — LFU cache in O(1): key map + (freq -> list of keys) + min_freq
 *
 * Concept : The popular "LeetCode 460" formulation. Instead of a linked list
 *           of frequency buckets (C version), keep unordered_map<freq, list>
 *           and remember min_freq. min_freq only changes in two easy ways:
 *           a new key sets it to 1; a touch that empties the min bucket
 *           bumps it by exactly 1. So eviction never searches.
 * Build   : make run P=dsa/19-system-design-structures/cpp/02_lfu_cache.cpp
 * Read    : ../README.md  (section "LFU cache")
 */
#include <cassert>
#include <iostream>
#include <list>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

template <typename K, typename V>
class LfuCache {
    struct Entry {
        V value;
        unsigned long freq;
        typename std::list<K>::iterator pos;   // where the key sits in buckets_[freq]
    };

public:
    explicit LfuCache(std::size_t cap) : cap_(cap) {}

    std::optional<V> get(const K& key) {
        auto it = entries_.find(key);
        if (it == entries_.end()) return std::nullopt;
        touch(key, it->second);
        return it->second.value;
    }

    void put(const K& key, V value) {
        if (cap_ == 0) return;
        if (auto it = entries_.find(key); it != entries_.end()) {
            it->second.value = std::move(value);
            touch(key, it->second);
            return;
        }
        if (entries_.size() == cap_) evict();
        auto& lst = buckets_[1];
        lst.push_front(key);                   // front = most recent within this freq
        entries_.emplace(key, Entry{std::move(value), 1, lst.begin()});
        min_freq_ = 1;                         // a brand-new key is always the least used
    }

    std::size_t size() const { return entries_.size(); }

private:
    // O(1): move key from bucket f to bucket f+1.
    void touch(const K& key, Entry& e) {
        auto& from = buckets_[e.freq];
        from.erase(e.pos);
        if (from.empty()) {
            buckets_.erase(e.freq);            // `from` is now dangling: don't use it
            if (min_freq_ == e.freq) ++min_freq_;
        }
        ++e.freq;
        auto& to = buckets_[e.freq];
        to.push_front(key);
        e.pos = to.begin();
    }

    // O(1): least recent key of the least frequent bucket.
    void evict() {
        auto& lst = buckets_[min_freq_];
        K victim = lst.back();
        lst.pop_back();
        if (lst.empty()) buckets_.erase(min_freq_);
        entries_.erase(victim);
    }

    std::size_t cap_;
    unsigned long min_freq_ = 0;
    std::unordered_map<K, Entry> entries_;
    std::unordered_map<unsigned long, std::list<K>> buckets_;
};

// Brute force: scan for (min freq, oldest last-use).
struct RefLfu {
    struct S { int key, value; unsigned long freq, stamp; };
    std::size_t cap;
    std::vector<S> s;
    unsigned long clk = 0;
    std::optional<int> get(int k) {
        for (auto& x : s)
            if (x.key == k) { ++x.freq; x.stamp = ++clk; return x.value; }
        return std::nullopt;
    }
    void put(int k, int v) {
        if (cap == 0) return;
        for (auto& x : s)
            if (x.key == k) { x.value = v; ++x.freq; x.stamp = ++clk; return; }
        if (s.size() == cap) {
            std::size_t vi = 0;
            for (std::size_t i = 1; i < s.size(); ++i)
                if (s[i].freq < s[vi].freq || (s[i].freq == s[vi].freq && s[i].stamp < s[vi].stamp))
                    vi = i;
            s.erase(s.begin() + static_cast<long>(vi));
        }
        s.push_back({k, v, 1, ++clk});
    }
};

int main() {
    std::cout << "== LFU vs LRU intuition: a scan does not flush hot keys ==\n";
    LfuCache<int, int> c(3);
    c.put(1, 1); c.put(2, 2);
    for (int i = 0; i < 5; ++i) { (void)c.get(1); (void)c.get(2); }   // 1 and 2 are hot
    for (int k = 100; k < 110; ++k) c.put(k, k);   // one-off scan of cold keys
    assert(c.get(1) && c.get(2));                  // hot keys survived the scan
    std::cout << "hot keys 1,2 survived a scan of 10 cold keys (an LRU would have lost them)\n";

    std::cout << "\n== randomised check vs brute force ==\n";
    std::mt19937 rng(460);
    for (int trial = 0; trial < 60; ++trial) {
        std::size_t cap = rng() % 10;
        LfuCache<int, int> lfu(cap);
        RefLfu ref{cap, {}};
        for (int op = 0; op < 3000; ++op) {
            int key = static_cast<int>(rng() % 25);
            if (rng() % 3) {
                assert(lfu.get(key) == ref.get(key));
            } else {
                int v = static_cast<int>(rng() % 1000);
                lfu.put(key, v);
                ref.put(key, v);
            }
            assert(lfu.size() == ref.s.size());
        }
    }
    std::cout << "60 trials x 3000 ops agree\n";
    std::cout << "OK\n";
    return 0;
}
