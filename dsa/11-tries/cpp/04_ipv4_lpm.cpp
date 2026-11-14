/*
 * 04_ipv4_lpm.cpp — generic binary-trie routing table (longest-prefix match)
 *
 * Concept : C++ version of c/05_ipv4_lpm.c. `LpmTable<T>` maps CIDR prefixes
 *           ("10.1.0.0/16") to any value T and answers lookup(addr) with
 *           std::optional<T>: the value of the longest matching prefix, or
 *           nullopt. Nodes own their two children via unique_ptr. Also shows
 *           parsing CIDR text and enumerating routes in trie (prefix) order.
 *           Verified against a brute-force std::vector scan.
 * Build   : make run P=dsa/11-tries/cpp/04_ipv4_lpm.cpp
 * Read    : ../README.md  (section "IPv4 longest-prefix match")
 */
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct Cidr {
    std::uint32_t prefix;
    int len; // 0..32
};

static std::uint32_t mask_of(int len) // x << 32 is UB, so /0 is special
{
    return len == 0 ? 0u : ~0u << (32 - len);
}

// "a.b.c.d/len" -> Cidr. Host bits are cleared (10.1.2.3/16 -> 10.1.0.0/16),
// exactly what `ip route add` complains about / normalises.
static Cidr parse_cidr(const std::string& s)
{
    unsigned a, b, c, d;
    int len;
    char dot1, dot2, dot3, slash;
    std::istringstream in(s);
    in >> a >> dot1 >> b >> dot2 >> c >> dot3 >> d >> slash >> len;
    assert(in && dot1 == '.' && dot2 == '.' && dot3 == '.' && slash == '/' && a < 256 && b < 256 && c < 256 && d < 256 &&
           len >= 0 && len <= 32);
    std::uint32_t ip = a << 24 | b << 16 | c << 8 | d;
    return {ip & mask_of(len), len};
}

static std::string to_string(Cidr c)
{
    std::ostringstream o;
    o << (c.prefix >> 24) << '.' << (c.prefix >> 16 & 255) << '.' << (c.prefix >> 8 & 255)
      << '.' << (c.prefix & 255) << '/' << c.len;
    return o.str();
}

template <class T>
class LpmTable {
    struct Node {
        std::array<std::unique_ptr<Node>, 2> kid; // [0] = next bit 0, [1] = 1
        std::optional<T> value;                   // set => a route ends here
    };
    Node root_; // represents 0.0.0.0/0

    static int bit(std::uint32_t a, int i) { return static_cast<int>(a >> (31 - i) & 1u); }

    // erase helper: true if `n` is now empty and may be pruned
    static bool erase_rec(Node& n, Cidr c, int depth, bool& found)
    {
        if (depth == c.len) {
            found = n.value.has_value();
            n.value.reset();
        } else {
            auto& k = n.kid[static_cast<std::size_t>(bit(c.prefix, depth))];
            if (k && erase_rec(*k, c, depth + 1, found))
                k.reset();
        }
        return !n.value && !n.kid[0] && !n.kid[1];
    }

    template <class F>
    static void walk(const Node& n, std::uint32_t pfx, int depth, F& f)
    {
        if (n.value)
            f(Cidr{pfx, depth}, *n.value);
        for (std::uint32_t b = 0; b < 2; ++b)
            if (n.kid[b])
                walk(*n.kid[b], pfx | b << (31 - depth), depth + 1, f);
    }

public:
    void insert(Cidr c, T v) // O(len) <= 32 steps
    {
        Node* cur = &root_;
        for (int i = 0; i < c.len; ++i) {
            auto& k = cur->kid[static_cast<std::size_t>(bit(c.prefix, i))];
            if (!k)
                k = std::make_unique<Node>();
            cur = k.get();
        }
        cur->value = std::move(v);
    }

    // O(32): the answer is the DEEPEST node with a value on the address path.
    std::optional<T> lookup(std::uint32_t addr) const
    {
        std::optional<T> best;
        const Node* cur = &root_;
        for (int i = 0; cur; ++i) {
            if (cur->value)
                best = cur->value;
            if (i == 32)
                break;
            cur = cur->kid[static_cast<std::size_t>(bit(addr, i))].get();
        }
        return best;
    }

    bool erase(Cidr c) // O(32), prunes empty branches
    {
        bool found = false;
        erase_rec(root_, c, 0, found); // root itself is never freed
        return found;
    }

    // Pre-order walk: a prefix comes before the more specific ones inside it.
    template <class F>
    void for_each(F f) const
    {
        walk(root_, 0, 0, f);
    }
};

int main()
{
    LpmTable<std::string> rt;
    for (auto [cidr, dev] : std::vector<std::pair<std::string, std::string>>{
             {"0.0.0.0/0", "eth0 via 192.168.1.1"},
             {"192.168.1.0/24", "eth0"},
             {"192.168.7.0/24", "usb0"},
             {"10.0.0.0/8", "tun0"},
             {"10.20.0.0/16", "wg0"},
             {"10.20.30.40/32", "lo (host route)"}})
        rt.insert(parse_cidr(cidr), dev);

    std::cout << "routing table in trie order:\n";
    rt.for_each([](Cidr c, const std::string& d) {
        std::cout << "  " << to_string(c) << " -> " << d << '\n';
    });

    auto q = [&](const char* a) {
        auto r = rt.lookup(parse_cidr(std::string(a) + "/32").prefix);
        std::cout << "  lookup " << a << " -> " << (r ? *r : "unreachable") << '\n';
        return r.value_or("");
    };
    assert(q("10.20.30.40") == "lo (host route)");
    assert(q("10.20.30.41") == "wg0");
    assert(q("10.99.0.1") == "tun0");
    assert(q("192.168.7.2") == "usb0");
    assert(q("1.1.1.1") == "eth0 via 192.168.1.1");
    assert(rt.erase(parse_cidr("0.0.0.0/0")) && !rt.erase(parse_cidr("0.0.0.0/0")));
    assert(q("1.1.1.1").empty()); // no default route any more
    assert(parse_cidr("10.1.2.3/16").prefix == parse_cidr("10.1.0.0/16").prefix);

    // ---- randomised vs brute force ----
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> lenD(0, 32);
    struct R {
        Cidr c;
        int v;
        bool live;
    };
    std::vector<R> table;
    LpmTable<int> t;
    auto brute = [&](std::uint32_t a) -> std::optional<int> {
        int bl = -1;
        std::optional<int> v;
        for (const auto& r : table)
            if (r.live && (a & mask_of(r.c.len)) == r.c.prefix && r.c.len > bl) {
                bl = r.c.len;
                v = r.v;
            }
        return v;
    };
    for (int i = 0; i < 500; ++i) {
        int len = lenD(rng);
        std::uint32_t p = rng() & mask_of(len);
        if (len >= 8 && i % 2) // nest half of the routes under 172.16.0.0/12-ish
            p = (p & 0x000fffffu) | 0xac100000u;
        p &= mask_of(len);
        for (auto& r : table) // same prefix again: overwrite, old one dies
            if (r.live && r.c.len == len && r.c.prefix == p)
                r.live = false;
        table.push_back({{p, len}, i, true});
        t.insert({p, len}, i);
    }
    auto addr = [&](int i) {
        std::uint32_t a = rng();
        return i % 2 ? (a & 0x000fffffu) | 0xac100000u : a;
    };
    for (int i = 0; i < 60000; ++i) {
        std::uint32_t a = addr(i);
        assert(t.lookup(a) == brute(a));
    }
    for (std::size_t i = 0; i < table.size(); i += 3)
        if (table[i].live) {
            assert(t.erase(table[i].c));
            table[i].live = false;
        }
    for (int i = 0; i < 30000; ++i) {
        std::uint32_t a = addr(i);
        assert(t.lookup(a) == brute(a));
    }
    std::cout << "randomised: 500 routes, 90000 lookups match brute force (incl. after erase)\n";
    return 0;
}
