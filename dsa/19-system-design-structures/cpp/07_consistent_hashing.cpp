/*
 * 07_consistent_hashing.cpp — HashRing with std::map, weighted servers, and
 *                             replica placement (N distinct successors, like Dynamo/Cassandra)
 *
 * Concept : std::map<hash, server> IS the ring: lower_bound() finds the first
 *           point clockwise in O(log n) and insert/erase keep it sorted with
 *           no rebuild. Weight = number of virtual nodes. For replication,
 *           keep walking clockwise until N DISTINCT physical servers are seen
 *           (skipping extra vnodes of servers already chosen).
 * Build   : make run P=dsa/19-system-design-structures/cpp/07_consistent_hashing.cpp
 * Read    : ../README.md  (section "Consistent hashing")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

static std::uint32_t hash32(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;            // FNV-1a ...
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;            // ... + splitmix64 finaliser
    h ^= h >> 27; h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return static_cast<std::uint32_t>(h);
}

class HashRing {
public:
    // O(w log n): w virtual nodes for this server.
    void add(const std::string& server, int weight) {
        for (int v = 0; v < weight; ++v) {
            auto [it, inserted] = ring_.emplace(hash32(server + "#" + std::to_string(v)), server);
            (void)it;
            if (inserted) ++points_[server];   // (a 32-bit collision just loses one vnode)
        }
    }

    void remove(const std::string& server) {
        for (auto it = ring_.begin(); it != ring_.end();)
            it = (it->second == server) ? ring_.erase(it) : std::next(it);
        points_.erase(server);
    }

    // O(log n)
    const std::string& owner(const std::string& key) const {
        assert(!ring_.empty());
        auto it = ring_.lower_bound(hash32(key));
        if (it == ring_.end()) it = ring_.begin();           // wrap around the circle
        return it->second;
    }

    // First n distinct physical servers clockwise from the key.
    std::vector<std::string> replicas(const std::string& key, std::size_t n) const {
        std::vector<std::string> out;
        n = std::min(n, points_.size());
        auto it = ring_.lower_bound(hash32(key));
        for (std::size_t steps = 0; out.size() < n && steps < ring_.size(); ++steps, ++it) {
            if (it == ring_.end()) it = ring_.begin();
            if (std::find(out.begin(), out.end(), it->second) == out.end())
                out.push_back(it->second);
        }
        return out;
    }

    // Reference: walk the whole ring linearly.
    std::vector<std::string> replicas_slow(const std::string& key, std::size_t n) const {
        std::vector<std::pair<std::uint32_t, std::string>> pts(ring_.begin(), ring_.end());
        std::uint32_t h = hash32(key);
        std::size_t start = 0;
        while (start < pts.size() && pts[start].first < h) ++start;
        std::vector<std::string> out;
        n = std::min(n, points_.size());
        for (std::size_t i = 0; i < pts.size() && out.size() < n; ++i) {
            const auto& s = pts[(start + i) % pts.size()].second;
            if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
        }
        return out;
    }

private:
    std::map<std::uint32_t, std::string> ring_;
    std::unordered_map<std::string, int> points_;
};

int main() {
    HashRing ring;
    ring.add("cache-a", 100);
    ring.add("cache-b", 100);
    ring.add("cache-c", 200);    // twice the RAM => twice the weight

    std::cout << "== weighted ownership over 30000 keys ==\n";
    std::map<std::string, int> load;
    std::vector<std::string> keys;
    for (int i = 0; i < 30000; ++i) keys.push_back("session:" + std::to_string(i));
    for (const auto& k : keys) ++load[ring.owner(k)];
    for (const auto& [s, n] : load) std::cout << "  " << s << " owns " << n << "\n";
    double ratio = static_cast<double>(load["cache-c"]) / ((load["cache-a"] + load["cache-b"]) / 2.0);
    std::cout << "cache-c / avg(a,b) = " << ratio << " (target 2.0)\n";
    // With ~100 vnodes per server the load still wobbles by ~10% per server
    // (see the C version's stddev table), so allow a generous band.
    assert(ratio > 1.5 && ratio < 2.5);

    std::cout << "\n== replica placement (3 copies) vs linear reference ==\n";
    ring.add("cache-d", 100);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        auto r = ring.replicas(k, 3);
        if (i % 10 == 0) assert(r == ring.replicas_slow(k, 3));   // slow check: O(ring)
        assert(r.size() == 3 && r[0] == ring.owner(k));
        assert(std::set<std::string>(r.begin(), r.end()).size() == 3);   // distinct
    }
    auto r0 = ring.replicas(keys[0], 3);
    std::cout << keys[0] << " -> " << r0[0] << ", " << r0[1] << ", " << r0[2] << "\n";
    assert(ring.replicas(keys[0], 10).size() == 4);      // can't exceed #servers

    std::cout << "\n== node failure: the first replica takes over ==\n";
    std::unordered_map<std::string, std::vector<std::string>> before;
    for (const auto& k : keys) before[k] = ring.replicas(k, 3);
    ring.remove("cache-b");
    int moved = 0;
    for (const auto& k : keys) {
        const auto& old = before[k];
        if (old[0] == "cache-b") {
            assert(ring.owner(k) == old[1]);   // next replica already HAS the data
            ++moved;
        } else {
            assert(ring.owner(k) == old[0]);   // nobody else's primary changed
        }
    }
    std::cout << moved << " keys failed over to their 2nd replica; all others untouched\n";
    std::cout << "OK\n";
    return 0;
}
