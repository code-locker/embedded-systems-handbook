/*
 * 04_std_map_set.cpp — std::map / std::set / multimap / multiset in practice
 *
 * Concept : std::map and std::set are red-black trees (libstdc++
 *           _Rb_tree, see bits/stl_tree.h). That gives you: sorted iteration,
 *           O(log n) insert/erase/find, lower_bound/upper_bound range queries,
 *           and iterators/references that stay valid when OTHER elements are
 *           inserted or erased. This program shows the API you actually use,
 *           the traps (operator[] inserts!), and an embedded-flavoured
 *           example: "which memory region contains this physical address?".
 * Build   : make run P=dsa/09-trees/cpp/04_std_map_set.cpp
 * Read    : ../README.md  (section "C vs C++" and "std::map / std::set")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// ---- a memory map like /proc/iomem: start address -> region -------------
struct Region {
    uint64_t start, size;
    std::string name;
};

// Find the region containing addr: the region with the greatest start <= addr.
// upper_bound gives the first start > addr; step back one. O(log n).
//
//   starts:  0x0000_0000   0x0010_0000   0x8000_0000
//                                ^            ^
//   addr = 0x0012_3456 ----------+  prev  <-- upper_bound
const Region* find_region(const std::map<uint64_t, Region>& m, uint64_t addr)
{
    auto it = m.upper_bound(addr);
    if (it == m.begin()) return nullptr;          // addr below every region
    --it;
    const Region& r = it->second;
    return addr < r.start + r.size ? &r : nullptr; // inside, or in a gap
}

int main()
{
    // ==== 1. map basics ======================================================
    std::map<std::string, int> irq;               // key-sorted, unique keys
    irq["eth0"] = 45;                              // operator[]: insert-or-access
    irq.insert({"mmc0", 32});                      // no overwrite if present
    irq.emplace("uart0", 20);                      // construct in place
    irq.try_emplace("gpu", 70);                    // C++17: no move-from if present
    irq.insert_or_assign("eth0", 46);              // C++17: explicit overwrite
    assert(irq.at("eth0") == 46);

    // TRAP: operator[] on a missing key INSERTS a default value (0).
    assert(irq.size() == 4);
    int dummy = irq["typo0"];                      // oops - creates "typo0" -> 0
    assert(dummy == 0 && irq.size() == 5);
    irq.erase("typo0");
    // Lookups that must not insert: find / count / contains (C++20) / at.
    assert(irq.find("nope") == irq.end() && irq.count("nope") == 0);

    std::printf("irq map in key order:");
    for (const auto& [name, num] : irq)            // structured bindings (C++17)
        std::printf(" %s=%d", name.c_str(), num);
    std::printf("\n");

    // ==== 2. ordered queries: what a hash map cannot do =====================
    std::set<int> s{10, 20, 30, 40, 50};
    assert(*s.lower_bound(25) == 30);              // first >= 25
    assert(*s.upper_bound(30) == 40);              // first >  30
    assert(s.lower_bound(60) == s.end());
    // all keys in [20, 40): O(log n + k)
    std::vector<int> range(s.lower_bound(20), s.lower_bound(40));
    assert((range == std::vector<int>{20, 30}));
    assert(*s.begin() == 10 && *s.rbegin() == 50);   // min / max: O(1) in libstdc++
    // floor(x): greatest <= x  =  prev(upper_bound(x))
    auto fl = s.upper_bound(35);
    assert(fl != s.begin() && *std::prev(fl) == 30);

    // ==== 3. custom comparators ==============================================
    // (a) function object
    struct ByLength {
        bool operator()(const std::string& a, const std::string& b) const
        {
            return a.size() != b.size() ? a.size() < b.size() : a < b;
        }
    };
    std::set<std::string, ByLength> words{"dma", "i2c", "pcie", "spi", "usb", "ethernet"};
    assert(*words.begin() == "dma" && *words.rbegin() == "ethernet");

    // (b) lambda: its type is passed with decltype, the object to the ctor
    auto by_abs = [](int a, int b) { return std::abs(a) < std::abs(b); };
    std::set<int, decltype(by_abs)> mags(by_abs);
    for (int v : {-3, 1, 2, -1}) mags.insert(v);
    // -1 and 1 are EQUIVALENT under this order (!(a<b) && !(b<a)) -> one kept.
    assert(mags.size() == 3);

    // (c) the comparator MUST be a strict weak ordering. Using <= instead of <
    // breaks equivalence (a<=a is true), so find() never matches and the tree
    // can corrupt. Never do: std::set<int, std::less_equal<int>>.

    // (d) transparent comparator std::less<>: find() with a string_view or
    // const char* without building a temporary std::string.
    std::map<std::string, int, std::less<>> fast{{"video0", 1}, {"video1", 2}};
    std::string_view key = "video1";
    assert(fast.find(key)->second == 2);          // heterogeneous lookup

    // ==== 4. multimap / multiset: duplicates allowed =========================
    std::multimap<int, std::string> by_prio;       // e.g. timers by deadline
    by_prio.emplace(5, "flush");
    by_prio.emplace(1, "irq");
    by_prio.emplace(5, "log");
    by_prio.emplace(3, "poll");
    auto [lo, hi] = by_prio.equal_range(5);        // all entries with key 5
    assert(std::distance(lo, hi) == 2);
    assert(lo->second == "flush");                 // equal keys keep insert order
    std::multiset<int> ms{3, 1, 3, 3, 2};
    assert(ms.count(3) == 3);
    ms.erase(ms.find(3));                          // erase ONE copy
    assert(ms.count(3) == 2);
    ms.erase(3);                                   // erase(key) removes ALL copies
    assert(ms.count(3) == 0 && ms.size() == 2);

    // ==== 5. erase while iterating: use the returned iterator ================
    std::map<int, int> m;
    for (int i = 0; i < 10; ++i) m[i] = i * i;
    for (auto it = m.begin(); it != m.end();) {
        if (it->first % 2) it = m.erase(it);       // returns next element
        else ++it;
    }
    assert(m.size() == 5 && m.count(3) == 0);
    // (C++20 has std::erase_if(m, pred) for this.)

    // ==== 6. iterator / reference stability =================================
    const int& ref4 = m.at(4);                     // reference into a node
    auto it6 = m.find(6);
    for (int i = 100; i < 1100; ++i) m[i] = i;     // many inserts -> rebalancing
    m.erase(2);
    assert(ref4 == 16 && it6->second == 36);       // still valid: nodes never move
    // (contrast: std::vector / unordered_map rehash invalidate on growth)

    // ==== 7. node handles (C++17): move a node between trees, no realloc =====
    std::map<int, std::string> pending{{1, "a"}, {2, "b"}}, done;
    auto nh = pending.extract(1);                  // unlink: O(log n), no free
    nh.key() = 10;                                 // keys can be changed while out!
    done.insert(std::move(nh));
    assert(pending.size() == 1 && done.at(10) == "a");
    done.merge(pending);                           // splice all nodes across
    assert(pending.empty() && done.size() == 2);

    // ==== 8. set algebra on sorted ranges ===================================
    std::set<std::string> a{"dma", "gpio", "i2c", "spi"}, b{"gpio", "pwm", "spi"};
    std::vector<std::string> both;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(both));
    assert((both == std::vector<std::string>{"gpio", "spi"}));

    // ==== 9. memory map lookup ===============================================
    std::map<uint64_t, Region> iomem;
    auto add = [&iomem](uint64_t start, uint64_t size, const char* name) {
        iomem.emplace(start, Region{start, size, name});
    };
    add(0x00000000, 0x00100000, "boot-rom");
    add(0x00100000, 0x00100000, "msmc-sram");
    add(0x02800000, 0x00000200, "uart0");
    add(0x80000000, 0x80000000, "ddr");
    const uint64_t probes[] = {0x00123456, 0x02800010, 0x02800400, 0x90000000, 0x02000000};
    for (uint64_t a64 : probes) {
        const Region* r = find_region(iomem, a64);
        std::printf("0x%08llx -> %s\n", static_cast<unsigned long long>(a64), r ? r->name.c_str() : "(unmapped)");
    }
    assert(find_region(iomem, 0x00123456)->name == "msmc-sram");
    assert(find_region(iomem, 0x02800010)->name == "uart0");
    assert(find_region(iomem, 0x02800400) == nullptr);     // gap after uart0
    assert(find_region(iomem, 0x90000000)->name == "ddr");

    // randomised: find_region vs linear scan
    std::mt19937_64 rng(3);
    for (int i = 0; i < 20000; ++i) {
        uint64_t addr = rng() % 0x100000000ULL;
        const Region* expect = nullptr;
        for (const auto& kv : iomem)
            if (addr >= kv.second.start && addr < kv.second.start + kv.second.size)
                expect = &kv.second;
        assert(find_region(iomem, addr) == expect);
    }

    std::printf("all std::map / std::set checks passed\n");
    return 0;
}
