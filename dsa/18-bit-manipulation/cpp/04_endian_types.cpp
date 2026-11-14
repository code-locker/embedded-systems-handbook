/*
 * 04_endian_types.cpp — generic byte swap, portable load/store templates, and
 *                       a Big<T> wrapper type that makes "forgot to swap"
 *                       a compile error
 *
 * Concept : The Linux kernel declares wire fields as __be32 / __le16 and the
 *           static checker `sparse` complains if you use one as a plain int
 *           without be32_to_cpu(). In C++ we get the same safety for free:
 *           Big<uint32_t> stores bytes in network order and only converts
 *           through .get()/.set(), so arithmetic on raw wire values cannot
 *           compile. sizeof(Big<T>) == sizeof(T), so it can sit inside a
 *           packed header struct that you memcpy from a packet.
 * Build   : make run P=dsa/18-bit-manipulation/cpp/04_endian_types.cpp
 * Read    : ../README.md  (section "3.8 Endianness and byte swapping")
 */
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <type_traits>

// Generic byte swap for any unsigned integer type: peel off bytes from the
// bottom of x and push them onto the bottom of r. O(sizeof(T)).
template <typename T>
constexpr T byteswap(T x) {
    static_assert(std::is_unsigned<T>::value, "unsigned only");
    T r = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        r = static_cast<T>((r << 8) | (x & 0xFFu));
        x = static_cast<T>(x >> 8);
    }
    return r;
}
static_assert(byteswap<std::uint32_t>(0x11223344u) == 0x44332211u, "");
static_assert(byteswap<std::uint8_t>(0xAB) == 0xAB, "");

// Host order detected portably (C++20 has std::endian; gcc predefines this).
constexpr bool kLittle = __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;

// Load/store with explicit byte order; any alignment, any host.
template <typename T>
T load_be(const std::uint8_t* p) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) v = static_cast<T>((v << 8) | p[i]);
    return v;
}
template <typename T>
T load_le(const std::uint8_t* p) {
    T v = 0;
    for (std::size_t i = sizeof(T); i-- > 0;) v = static_cast<T>((v << 8) | p[i]);
    return v;
}
template <typename T>
void store_be(std::uint8_t* p, T v) {
    for (std::size_t i = sizeof(T); i-- > 0;) { p[i] = static_cast<std::uint8_t>(v & 0xFFu); v = static_cast<T>(v >> 8); }
}

// Big<T>: value held in big-endian byte order. No implicit conversion to T.
template <typename T>
class Big {
public:
    Big() = default;
    explicit Big(T host) { set(host); }
    T get() const { return load_be<T>(bytes_.data()); }
    void set(T host) { store_be<T>(bytes_.data(), host); }
private:
    std::array<std::uint8_t, sizeof(T)> bytes_{};
};
static_assert(sizeof(Big<std::uint32_t>) == 4, "no overhead");
static_assert(std::is_trivially_copyable<Big<std::uint16_t>>::value, "memcpy-able");

// A UDP header exactly as it lies on the wire. Every field is big endian.
struct UdpHeader {
    Big<std::uint16_t> src_port, dst_port, length, checksum;
};
static_assert(sizeof(UdpHeader) == 8, "matches RFC 768");

int main() {
    std::cout << "== 1. Host is " << (kLittle ? "little" : "big") << " endian ==\n";
    std::uint32_t probe = 0x01020304u;
    std::uint8_t first;
    std::memcpy(&first, &probe, 1);
    assert((first == 0x04) == kLittle);

    std::cout << "\n== 2. Generic byteswap vs builtins ==\n";
    std::mt19937_64 rng(7);
    for (int i = 0; i < 100000; ++i) {
        std::uint64_t x = rng();
        assert(byteswap<std::uint64_t>(x) == __builtin_bswap64(x));
        assert(byteswap<std::uint32_t>(static_cast<std::uint32_t>(x)) == __builtin_bswap32(static_cast<std::uint32_t>(x)));
        assert(byteswap<std::uint16_t>(static_cast<std::uint16_t>(x)) == __builtin_bswap16(static_cast<std::uint16_t>(x)));
    }
    std::cout << "byteswap<u16/u32/u64> == __builtin_bswap*: OK\n";

    std::cout << "\n== 3. Parse a UDP header from raw packet bytes ==\n";
    // Source port 5000 (0x1388), dest port 554 (RTSP, 0x022A), length 36, checksum 0xBEEF
    const std::uint8_t wire[] = { 0x13, 0x88, 0x02, 0x2A, 0x00, 0x24, 0xBE, 0xEF };
    UdpHeader h;
    std::memcpy(&h, wire, sizeof h);       // the way packets are really parsed
    std::cout << "src=" << h.src_port.get() << " dst=" << h.dst_port.get()
              << " len=" << h.length.get() << " csum=0x" << std::hex << h.checksum.get() << std::dec << "\n";
    assert(h.src_port.get() == 5000 && h.dst_port.get() == 554);
    assert(h.length.get() == 36 && h.checksum.get() == 0xBEEF);
    // int bad = h.src_port + 1;          // compile error: no implicit conversion. That's the point.

    std::cout << "\n== 4. Build a header and check the bytes on the wire ==\n";
    UdpHeader out;
    out.src_port.set(0xABCD);
    out.dst_port.set(80);
    out.length.set(8);
    out.checksum.set(0);
    std::uint8_t buf[8];
    std::memcpy(buf, &out, sizeof buf);
    const std::uint8_t expect[] = { 0xAB, 0xCD, 0x00, 0x50, 0x00, 0x08, 0x00, 0x00 };
    assert(std::memcmp(buf, expect, 8) == 0);
    std::cout << "bytes match network order: OK\n";

    std::cout << "\n== 5. load_le/load_be round trips at every unaligned offset ==\n";
    std::uint8_t raw[16];
    for (int i = 0; i < 16; ++i) raw[i] = static_cast<std::uint8_t>(rng());
    for (int off = 0; off <= 8; ++off) {
        auto be = load_be<std::uint64_t>(raw + off);
        auto le = load_le<std::uint64_t>(raw + off);
        assert(be == byteswap(le));
        std::uint64_t host;
        std::memcpy(&host, raw + off, 8);
        assert(host == (kLittle ? le : be));
    }
    std::cout << "load_be == byteswap(load_le) and memcpy matches host order: OK\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
