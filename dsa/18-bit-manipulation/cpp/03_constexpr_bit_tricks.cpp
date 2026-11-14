/*
 * 03_constexpr_bit_tricks.cpp — generic, constexpr bit helpers for any
 *                               unsigned type, proven at compile time
 *
 * Concept : The classic tricks (popcount, x & (x-1), x & -x, Gray code,
 *           next power of two, reverse bits) written once as templates that
 *           work for uint8_t..uint64_t, and checked with static_assert so the
 *           compiler itself proves them. In C++20 most of these exist in
 *           <bit> (std::popcount, std::has_single_bit, std::countr_zero,
 *           std::bit_ceil, std::rotl); this repo uses C++17, so we write them
 *           — and compare with std::bitset::count and the gcc builtins.
 * Build   : make run P=dsa/18-bit-manipulation/cpp/03_constexpr_bit_tricks.cpp
 * Read    : ../README.md  (sections "3.5 Popcount..." and "3.6 XOR tricks and Gray code")
 */
#include <bitset>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <type_traits>

// Only unsigned types: shifting and wrapping are fully defined for them.
template <typename T>
using EnableUnsigned = std::enable_if_t<std::is_unsigned<T>::value, int>;

template <typename T, EnableUnsigned<T> = 0>
constexpr int popcount(T x) {
    int c = 0;
    while (x) { x = static_cast<T>(x & (x - 1)); ++c; }   // Kernighan
    return c;
}

template <typename T, EnableUnsigned<T> = 0>
constexpr bool has_single_bit(T x) { return x && !(x & (x - 1)); }

template <typename T, EnableUnsigned<T> = 0>
constexpr T lowest_bit(T x) { return static_cast<T>(x & static_cast<T>(T{0} - x)); }

// Count trailing zeros; defined as the bit width for x == 0 (like C++20).
template <typename T, EnableUnsigned<T> = 0>
constexpr int countr_zero(T x) {
    if (!x) return std::numeric_limits<T>::digits;
    int n = 0;
    while (!(x & 1u)) { x = static_cast<T>(x >> 1); ++n; }
    return n;
}

// Smallest power of two >= x. Smear the top bit down by doubling shifts,
// for however many bits T has (loop unrolled by the compiler).
template <typename T, EnableUnsigned<T> = 0>
constexpr T bit_ceil(T x) {
    if (x <= 1) return 1;
    --x;
    for (int s = 1; s < std::numeric_limits<T>::digits; s <<= 1)
        x = static_cast<T>(x | (x >> s));
    return static_cast<T>(x + 1);
}

template <typename T, EnableUnsigned<T> = 0>
constexpr T rotl(T x, int s) {
    constexpr int N = std::numeric_limits<T>::digits;
    s &= N - 1;
    if (s == 0) return x;
    return static_cast<T>((x << s) | (x >> (N - s)));
}

// Reverse bit order by swapping ever larger groups (1, 2, 4, 8 bits...).
//   abcdefgh -> badcfehg (swap neighbours) -> dcbahgfe (swap pairs)
//            -> hgfedcba (swap nibbles)
// Used in FFT (bit-reversal permutation) and CRC with reflected input.
constexpr std::uint32_t reverse_bits(std::uint32_t x) {
    x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1);
    x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
    x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4);
    x = ((x >> 8) & 0x00FF00FFu) | ((x & 0x00FF00FFu) << 8);
    return (x >> 16) | (x << 16);
}

template <typename T, EnableUnsigned<T> = 0>
constexpr T to_gray(T n) { return static_cast<T>(n ^ (n >> 1)); }

template <typename T, EnableUnsigned<T> = 0>
constexpr T from_gray(T g) {
    T n = g;
    while (g >>= 1) n = static_cast<T>(n ^ g);    // prefix XOR from the top
    return n;
}

// Branch-free helpers that show up in DSP / video code.
constexpr std::int32_t sign_of(std::int32_t x) { return (x > 0) - (x < 0); }
constexpr std::uint32_t abs_diff(std::uint32_t a, std::uint32_t b) { return a > b ? a - b : b - a; }
// Align up to a power-of-two boundary (DMA buffers, page sizes, cache lines).
constexpr std::uint64_t align_up(std::uint64_t x, std::uint64_t a) { return (x + a - 1) & ~(a - 1); }

// ---- the compiler proves these before the program even runs ---------------
static_assert(popcount<std::uint8_t>(0xFF) == 8, "");
static_assert(popcount<std::uint64_t>(0x8000000000000001ull) == 2, "");
static_assert(has_single_bit(64u) && !has_single_bit(0u) && !has_single_bit(96u), "");
static_assert(lowest_bit<std::uint16_t>(0x58) == 0x08, "");
static_assert(countr_zero<std::uint8_t>(0) == 8 && countr_zero(0x80u) == 7, "");
static_assert(bit_ceil<std::uint32_t>(1000) == 1024 && bit_ceil<std::uint8_t>(3) == 4, "");
static_assert(rotl<std::uint8_t>(0x81, 1) == 0x03, "");
static_assert(reverse_bits(1u) == 0x80000000u, "");
static_assert(from_gray(to_gray(12345u)) == 12345u, "");
static_assert(align_up(4097, 4096) == 8192 && align_up(4096, 4096) == 4096, "");
static_assert(sign_of(-7) == -1 && sign_of(0) == 0, "");

template <typename T>
void random_check(std::mt19937_64& rng, int iters) {
    for (int i = 0; i < iters; ++i) {
        T x = static_cast<T>(rng());
        std::bitset<std::numeric_limits<T>::digits> bs(x);
        assert(popcount(x) == static_cast<int>(bs.count()));
        assert(popcount(x) == __builtin_popcountll(x));
        assert(has_single_bit(x) == (bs.count() == 1));
        if (x) {
            assert(countr_zero(x) == __builtin_ctzll(x));
            assert(lowest_bit(x) == static_cast<T>(T{1} << countr_zero(x)));
        }
        assert(from_gray(to_gray(x)) == x);
        assert(popcount(static_cast<T>(to_gray(x) ^ to_gray(static_cast<T>(x + 1)))) == 1);
        int s = static_cast<int>(rng() % std::numeric_limits<T>::digits);
        T r = rotl(x, s);
        assert(popcount(r) == popcount(x));
        assert(rotl(r, std::numeric_limits<T>::digits - s) == x);   // rotate back
    }
}

int main() {
    std::cout << "== 1. static_asserts above already passed at compile time ==\n";

    std::cout << "\n== 2. Random checks against std::bitset and gcc builtins ==\n";
    std::mt19937_64 rng(2024);
    random_check<std::uint8_t>(rng, 20000);
    random_check<std::uint16_t>(rng, 20000);
    random_check<std::uint32_t>(rng, 20000);
    random_check<std::uint64_t>(rng, 20000);
    std::cout << "popcount, has_single_bit, countr_zero, lowest_bit, gray, rotl: OK for u8..u64\n";

    std::cout << "\n== 3. bit_ceil and reverse_bits vs slow references ==\n";
    for (std::uint32_t x = 1; x < 100000; ++x) {
        std::uint32_t p = 1;
        while (p < x) p <<= 1;
        assert(bit_ceil(x) == p);
    }
    for (int i = 0; i < 100000; ++i) {
        auto x = static_cast<std::uint32_t>(rng());
        std::uint32_t ref = 0;
        for (int b = 0; b < 32; ++b) if ((x >> b) & 1u) ref |= 1u << (31 - b);
        assert(reverse_bits(x) == ref);
    }
    std::cout << "bit_ceil(1000)=" << bit_ceil(1000u) << ", reverse_bits(0x1)=0x"
              << std::hex << reverse_bits(1u) << std::dec << ": OK\n";

    std::cout << "\n== 4. align_up: the kernel's ALIGN() macro ==\n";
    for (std::uint64_t x = 0; x < 20000; ++x) {
        std::uint64_t ref = (x + 63) / 64 * 64;
        assert(align_up(x, 64) == ref);
    }
    std::cout << "align_up(100, 64) = " << align_up(100, 64) << " (next cache line)\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
