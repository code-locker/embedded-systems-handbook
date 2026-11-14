/*
 * 01_bitset.cpp — std::bitset<N> (fixed size), a hand-written dynamic bitset
 *                 on std::vector<uint64_t>, and a bitset sieve of Eratosthenes
 *
 * Concept : A bitset packs one bool per BIT instead of per byte: 8x less
 *           memory, and set operations (|, &, ^) process 64 elements per CPU
 *           instruction. std::bitset<N> has its size fixed at compile time;
 *           for runtime sizes write your own (or boost::dynamic_bitset).
 *           Bit i lives in word i / 64 at position i % 64 — that is the whole
 *           trick (i >> 6 and i & 63).
 * Build   : make run P=dsa/18-bit-manipulation/cpp/01_bitset.cpp
 * Read    : ../README.md  (section "3.7 Bitsets")
 */
#include <bitset>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// DynBitset: runtime-sized bitset. Memory owned by the vector (RAII) — no
// manual free, copy/move come for free.
//
//   bit index:   0 ......... 63 | 64 ........ 127 | 128 ...
//   words_[ ]:   [     0       ] [       1       ] [  2  ...
//   bit i  ->  words_[i >> 6], mask 1 << (i & 63)
// ---------------------------------------------------------------------------
class DynBitset {
public:
    explicit DynBitset(std::size_t nbits)
        : n_(nbits), words_((nbits + 63) / 64, 0) {}

    std::size_t size() const { return n_; }

    void set(std::size_t i)        { words_[i >> 6] |=  (std::uint64_t{1} << (i & 63)); }
    void reset(std::size_t i)      { words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }
    void flip(std::size_t i)       { words_[i >> 6] ^=  (std::uint64_t{1} << (i & 63)); }
    bool test(std::size_t i) const { return (words_[i >> 6] >> (i & 63)) & 1u; }

    // O(n/64): one popcount instruction per word instead of n tests.
    std::size_t count() const {
        std::size_t c = 0;
        for (auto w : words_) c += static_cast<std::size_t>(__builtin_popcountll(w));
        return c;
    }

    // Set operations: 64 elements per loop iteration.
    DynBitset& operator&=(const DynBitset& o) { for (std::size_t k = 0; k < words_.size(); ++k) words_[k] &= o.words_[k]; return *this; }
    DynBitset& operator|=(const DynBitset& o) { for (std::size_t k = 0; k < words_.size(); ++k) words_[k] |= o.words_[k]; return *this; }
    DynBitset& operator^=(const DynBitset& o) { for (std::size_t k = 0; k < words_.size(); ++k) words_[k] ^= o.words_[k]; return *this; }

    // Find next set bit at index >= from, or size() if none. Skips whole zero
    // words, and uses ctz inside a word: O(n/64) worst case.
    std::size_t find_next(std::size_t from) const {
        if (from >= n_) return n_;
        std::size_t k = from >> 6;
        std::uint64_t w = words_[k] & (~std::uint64_t{0} << (from & 63)); // drop bits below `from`
        while (true) {
            if (w) {
                std::size_t i = (k << 6) + static_cast<std::size_t>(__builtin_ctzll(w));
                return i < n_ ? i : n_;
            }
            if (++k == words_.size()) return n_;
            w = words_[k];
        }
    }

private:
    std::size_t n_;
    std::vector<std::uint64_t> words_;
};

// Sieve of Eratosthenes on a bitset: n bits instead of n bytes, so primes up
// to 10^7 fit in 1.25 MB (inside an A72's L2), vs 10 MB with bool[].
static DynBitset sieve(std::size_t n) {
    DynBitset composite(n + 1);
    composite.set(0);
    if (n >= 1) composite.set(1);
    for (std::size_t p = 2; p * p <= n; ++p)
        if (!composite.test(p))
            for (std::size_t m = p * p; m <= n; m += p) composite.set(m);
    return composite;
}

static bool is_prime_slow(std::size_t x) {
    if (x < 2) return false;
    for (std::size_t d = 2; d * d <= x; ++d) if (x % d == 0) return false;
    return true;
}

int main() {
    std::cout << "== 1. std::bitset<8> basics ==\n";
    std::bitset<8> b("10110010");          // string is MSB first: b[7] is the leftmost char
    std::cout << "b = " << b << ", count=" << b.count() << ", b[1]=" << b[1]
              << ", to_ulong=" << b.to_ulong() << "\n";
    assert(b.count() == 4 && b[1] && !b[0] && b.to_ulong() == 0xB2);
    b.set(0).reset(7).flip(4);             // chainable
    std::cout << "after set(0).reset(7).flip(4): " << b << "\n";
    assert(b.to_string() == "00100011");
    assert((b << 2).to_string() == "10001100");   // shifts drop bits off the end
    assert(b.any() && !b.all() && !b.none());
    // b.test(8) would throw std::out_of_range; b[8] is UB. test() = checked.
    bool threw = false;
    try { (void)b.test(8); } catch (const std::out_of_range&) { threw = true; }
    assert(threw);
    std::cout << "test(8) threw std::out_of_range as expected\n";

    std::cout << "\n== 2. Set algebra: bitset as a set of small integers ==\n";
    std::bitset<64> evens, threes;
    for (int i = 0; i < 64; i += 2) evens.set(static_cast<std::size_t>(i));
    for (int i = 0; i < 64; i += 3) threes.set(static_cast<std::size_t>(i));
    auto sixes = evens & threes;           // intersection
    std::cout << "multiples of 6 below 64: " << sixes.count() << "\n";
    assert(sixes.count() == 11);          // 0,6,...,60
    assert((evens | threes).count() == 32 + 22 - 11);

    std::cout << "\n== 3. DynBitset vs std::vector<bool> on random operations ==\n";
    std::mt19937 rng(42);
    const std::size_t N = 1000;
    DynBitset d(N), d2(N);
    std::vector<bool> ref(N), ref2(N);
    for (int it = 0; it < 50000; ++it) {
        std::size_t i = rng() % N;
        switch (rng() % 4) {
        case 0: d.set(i);   ref[i] = true;     break;
        case 1: d.reset(i); ref[i] = false;    break;
        case 2: d.flip(i);  ref[i] = !ref[i];  break;
        default: d2.set(i); ref2[i] = true;    break;
        }
        assert(d.test(i) == ref[i]);
    }
    std::size_t refcount = 0;
    for (bool x : ref) refcount += x;
    assert(d.count() == refcount);
    DynBitset u = d;  u |= d2;
    DynBitset x = d;  x ^= d2;
    DynBitset a = d;  a &= d2;
    for (std::size_t i = 0; i < N; ++i) {
        assert(u.test(i) == (ref[i] || ref2[i]));
        assert(x.test(i) == (ref[i] != ref2[i]));
        assert(a.test(i) == (ref[i] && ref2[i]));
    }
    // find_next walks exactly the set bits, in order
    std::size_t walked = 0, prev = 0;
    for (std::size_t i = d.find_next(0); i < N; i = d.find_next(i + 1)) {
        assert(ref[i]);
        assert(walked == 0 || i > prev);
        prev = i;
        ++walked;
    }
    assert(walked == refcount);
    std::cout << "set/reset/flip/count/|/&/^/find_next all match vector<bool>: OK ("
              << refcount << " bits set)\n";

    std::cout << "\n== 4. Bitset sieve of Eratosthenes ==\n";
    const std::size_t LIMIT = 1000000;
    DynBitset comp = sieve(LIMIT);
    std::size_t primes = LIMIT + 1 - comp.count();
    std::cout << "primes <= 10^6: " << primes << " (sieve used "
              << (LIMIT + 64) / 64 * 8 / 1024 << " KiB)\n";
    assert(primes == 78498);
    for (std::size_t i = 0; i < 20000; ++i) assert(!comp.test(i) == is_prime_slow(i));
    std::cout << "sieve agrees with trial division for 0..19999: OK\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
