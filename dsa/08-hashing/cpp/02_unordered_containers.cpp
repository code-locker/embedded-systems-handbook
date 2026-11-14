/*
 * 02_unordered_containers.cpp — std::unordered_map / unordered_set in depth,
 *                               and custom hashers for your own types
 *
 * Concept : The standard hash containers are separate-chaining tables
 *           (buckets of nodes). Learn the API that matters (find vs [],
 *           try_emplace, reserve, bucket inspection) and the two ways to
 *           hash a struct: a hasher functor, or specialising std::hash.
 * Build   : make run P=dsa/08-hashing/cpp/02_unordered_containers.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

// ---- 1. a key type: a (bus, address) pair like an I2C device -----------
struct I2cDev {
    int bus;
    int addr;
    bool operator==(const I2cDev &o) const { return bus == o.bus && addr == o.addr; }
};

// Combining field hashes. NEVER just XOR them: h(1,2) == h(2,1) and
// h(x,x) == 0. boost::hash_combine mixes with a shift and the golden ratio
// constant, so order matters and bits spread.
inline void hash_combine(std::size_t &seed, std::size_t v) {
    seed ^= v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
}

// Option A: a hasher functor passed as a template argument.
struct I2cDevHash {
    std::size_t operator()(const I2cDev &d) const noexcept {
        std::size_t seed = 0;
        hash_combine(seed, std::hash<int>{}(d.bus));
        hash_combine(seed, std::hash<int>{}(d.addr));
        return seed;
    }
};

// ---- 2. Option B: specialise std::hash so the type "just works" ---------
struct Pixel {
    std::uint16_t x, y;
    bool operator==(const Pixel &o) const { return x == o.x && y == o.y; }
};
namespace std {
template <> struct hash<Pixel> {
    // Two 16-bit fields fit in one integer exactly: a PERFECT hash (no two
    // distinct pixels share a value). Always prefer packing when it fits.
    size_t operator()(const Pixel &p) const noexcept {
        return std::hash<std::uint32_t>{}((std::uint32_t(p.x) << 16) | p.y);
    }
};
} // namespace std

// ---- 3. heterogeneous-ish lookup with string_view ---------------------------
// C++17 unordered_map<std::string,...>::find needs a std::string, so a
// lookup with a string_view or const char* builds a temporary string
// (allocation if long). C++20 adds transparent hashing (is_transparent) to
// avoid it. In C++17 the common workaround: key the map BY string_view when
// the strings outlive the map (string literals, an interned table).

int main() {
    // ---------------- unordered_map basics ----------------
    std::unordered_map<std::string, int> mtu;
    mtu.reserve(16); // pre-size buckets: no rehash for the first 16 inserts
    std::size_t buckets_before = mtu.bucket_count();

    mtu["eth0"] = 1500;                       // [] inserts if missing
    auto [it, inserted] = mtu.try_emplace("eth0", 9000); // no overwrite
    assert(!inserted && it->second == 1500);
    mtu.insert_or_assign("eth0", 9000);       // explicit overwrite (C++17)
    assert(mtu.at("eth0") == 9000);           // at(): throws if missing

    // THE classic pitfall: operator[] on a read silently inserts.
    assert(mtu.count("wlan0") == 0);
    int v = mtu["wlan0"];                     // inserts {"wlan0", 0}!
    assert(v == 0 && mtu.size() == 2);
    // Read without inserting: find (or contains in C++20)
    assert(mtu.find("can0") == mtu.end() && mtu.size() == 2);
    mtu.erase("wlan0");
    assert(mtu.bucket_count() == buckets_before); // reserve() did its job

    // ---------------- bucket inspection ----------------
    std::unordered_map<int, int> h;
    for (int i = 0; i < 1000; ++i) h[i] = i;
    std::cout << "unordered_map<int,int> with 1000 keys: " << h.bucket_count()
              << " buckets, load_factor " << h.load_factor()
              << ", max_load_factor " << h.max_load_factor() << "\n";
    assert(h.load_factor() <= h.max_load_factor());
    std::size_t b = h.bucket(42);             // which bucket holds key 42
    bool found = false;
    for (auto lit = h.begin(b); lit != h.end(b); ++lit) found |= (lit->first == 42);
    assert(found);
    // Note: libstdc++ std::hash<int> is the identity function! Fine with its
    // prime bucket counts, a disaster with a power-of-two mask (see
    // c/01_hash_functions.c).
    assert(std::hash<int>{}(12345) == 12345u);

    // Iterator invalidation rule worth memorising:
    //   rehash invalidates ITERATORS, but never pointers/references to
    //   elements (nodes don't move). erase invalidates only the erased one.
    int *p = &h[7];
    h.reserve(100000); // forces a rehash
    assert(*p == 7 && p == &h[7]);

    // erase while iterating: use the returned iterator
    for (auto eit = h.begin(); eit != h.end();)
        eit = (eit->first % 2) ? h.erase(eit) : std::next(eit);
    assert(h.size() == 500);

    // ---------------- custom hashers ----------------
    std::unordered_map<I2cDev, std::string, I2cDevHash> devs{
        {{1, 0x50}, "eeprom"}, {{1, 0x68}, "rtc"}, {{2, 0x50}, "eeprom2"}};
    assert(devs.at({1, 0x68}) == "rtc");
    assert(devs.count({0x50, 1}) == 0); // order matters with hash_combine
    assert(I2cDevHash{}({1, 2}) != I2cDevHash{}({2, 1}));

    std::unordered_set<Pixel> lit_pixels; // uses std::hash<Pixel>
    for (std::uint16_t i = 0; i < 100; ++i) lit_pixels.insert({i, i});
    assert(lit_pixels.count({5, 5}) == 1 && lit_pixels.count({5, 6}) == 0);
    auto [pit, ok] = lit_pixels.insert({5, 5}); // set: duplicates rejected
    (void)pit;
    assert(!ok && lit_pixels.size() == 100);

    // ---------------- unordered_set: membership + set algebra -------------
    std::unordered_set<std::string_view> builtin{"cd", "echo", "export", "exit"};
    assert(builtin.count("echo") && !builtin.count("ls"));

    std::unordered_set<int> a{1, 2, 3, 4}, bset{3, 4, 5};
    std::unordered_set<int> inter;
    for (int x : a) if (bset.count(x)) inter.insert(x); // O(|a|) average
    assert(inter == (std::unordered_set<int>{3, 4}));

    // multimap: several values per key (e.g. IRQ -> handlers)
    std::unordered_multimap<int, std::string> irq{{33, "uart"}, {33, "dma"}, {7, "gpio"}};
    auto range = irq.equal_range(33);
    int n = 0;
    for (auto r = range.first; r != range.second; ++r) ++n;
    assert(n == 2 && irq.count(33) == 2);

    std::cout << "all unordered-container checks passed\n";
    return 0;
}
