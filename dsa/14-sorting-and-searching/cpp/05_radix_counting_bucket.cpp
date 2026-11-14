/*
 * 05_radix_counting_bucket.cpp — generic counting / radix / bucket sort in C++
 *
 * Concept : Non-comparison sorts need a KEY that is a small integer (counting)
 *           or a fixed-width bit pattern (radix). Templates let one radix sort
 *           handle uint8..uint64, signed ints and floats, as long as we map
 *           each value to an unsigned key whose ORDER matches:
 *             signed int : flip the sign bit       (-1 = 0xFF.. -> 0x7F..)
 *             float      : positive -> flip sign bit; negative -> flip ALL bits
 *           The record version sorts structs by a key function, stably.
 * Build   : make run P=dsa/14-sorting-and-searching/cpp/05_radix_counting_bucket.cpp
 * Read    : ../README.md  (section "3.5 Non-comparison sorts")
 */
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

// ---- order-preserving value -> unsigned key maps ---------------------------
template <class T>
auto radix_key(T v)
{
    if constexpr (std::is_floating_point_v<T>) {
        static_assert(sizeof(T) == 4 || sizeof(T) == 8);
        using U = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
        U u;
        std::memcpy(&u, &v, sizeof u);          // bit-cast without UB
        const U sign = U(1) << (sizeof(U) * 8 - 1);
        //   IEEE-754: [sign | exponent | mantissa]; for positives, bigger bits
        //   = bigger value. Negatives are stored sign-magnitude, so their
        //   order is REVERSED -> invert all bits. Positives: set the sign bit
        //   so they sort above every negative.
        return (u & sign) ? U(~u) : U(u | sign);
    } else if constexpr (std::is_signed_v<T>) {
        using U = std::make_unsigned_t<T>;
        return static_cast<U>(static_cast<U>(v) ^ (U(1) << (sizeof(U) * 8 - 1)));
    } else {
        return v;
    }
}

// ---------------------------------------------------------------------------
// LSD radix sort, 8 bits per pass, on any T that radix_key understands.
// sizeof(T) passes of a stable counting sort. Skips a pass when every key
// has the same byte there (common for small values in wide types).
// ---------------------------------------------------------------------------
template <class T>
void radix_sort(std::vector<T> &v)
{
    std::vector<T> buf(v.size());
    for (unsigned pass = 0; pass < sizeof(T); ++pass) {
        const unsigned shift = pass * 8;
        std::array<size_t, 257> cnt{};
        for (const T &x : v) cnt[((radix_key(x) >> shift) & 0xFF) + 1]++;
        if (std::any_of(cnt.begin() + 1, cnt.end(), [&](size_t c) { return c == v.size(); }))
            continue;                               // all in one bucket: nothing to do
        for (int b = 0; b < 256; ++b) cnt[b + 1] += cnt[b];
        for (const T &x : v) buf[cnt[(radix_key(x) >> shift) & 0xFF]++] = x;
        v.swap(buf);
    }
}

// ---------------------------------------------------------------------------
// Stable counting sort of RECORDS by an integer key in [0, k).
// key(rec) is any callable -> a lambda, inlined.
// ---------------------------------------------------------------------------
template <class T, class KeyFn>
std::vector<T> counting_sort_by(const std::vector<T> &in, size_t k, KeyFn key)
{
    std::vector<size_t> start(k + 1, 0);
    for (const T &r : in) start[static_cast<size_t>(key(r)) + 1]++;
    for (size_t i = 0; i < k; ++i) start[i + 1] += start[i];     // prefix sums
    std::vector<T> out(in.size());
    for (const T &r : in) out[start[static_cast<size_t>(key(r))]++] = r;   // stable scatter
    return out;
}

// Bucket sort for values in [0,1): vector of vectors, then sort each bucket.
static void bucket_sort(std::vector<double> &v)
{
    const size_t n = v.size();
    if (n < 2) return;
    std::vector<std::vector<double>> buckets(n);
    for (double x : v) buckets[static_cast<size_t>(x * static_cast<double>(n))].push_back(x);
    size_t i = 0;
    for (auto &b : buckets) {
        std::sort(b.begin(), b.end());          // each bucket has ~1 element on average
        for (double x : b) v[i++] = x;
    }
}

struct Packet {
    int         priority;   // 0..7 (like 802.1p / IP precedence)
    int         seq;
    std::string payload;
};

template <class T>
void check_radix(const char *name, std::vector<T> v)
{
    auto ref = v;
    std::sort(ref.begin(), ref.end());
    radix_sort(v);
    assert(v == ref);
    std::cout << "  radix_sort<" << name << "> == std::sort (" << v.size() << " values)\n";
}

int main()
{
    std::cout << "=== Counting, radix and bucket sort (generic) ===\n\n";
    std::mt19937_64 rng(1234);

    std::vector<uint8_t>  u8(5000);  for (auto &x : u8)  x = static_cast<uint8_t>(rng());
    std::vector<uint32_t> u32(5000); for (auto &x : u32) x = static_cast<uint32_t>(rng());
    std::vector<uint64_t> u64(5000); for (auto &x : u64) x = rng();
    std::vector<int32_t>  i32(5000); for (auto &x : i32) x = static_cast<int32_t>(rng());
    std::vector<int64_t>  i64(5000); for (auto &x : i64) x = static_cast<int64_t>(rng());
    std::vector<float>    f32(5000);
    std::vector<double>   f64(5000);
    std::uniform_real_distribution<double> d(-1e6, 1e6);
    for (auto &x : f32) x = static_cast<float>(d(rng));
    for (auto &x : f64) x = d(rng);
    // the awkward values: signed zeros, extremes, infinities, denormals
    f64.insert(f64.end(), {0.0, -0.0, std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::denorm_min(), -1e-310, 1e300, -1e300});
    i32.insert(i32.end(), {INT32_MIN, INT32_MAX, -1, 0, 1});

    check_radix("uint8_t", u8);
    check_radix("uint32_t", u32);
    check_radix("uint64_t", u64);
    check_radix("int32_t", i32);
    check_radix("int64_t", i64);
    check_radix("float", f32);
    {   // doubles: -0.0 == 0.0 compare equal, so compare by sign bit as well
        auto v = f64, ref = f64;
        radix_sort(v);
        std::sort(ref.begin(), ref.end());
        for (size_t i = 0; i < v.size(); ++i) assert(v[i] == ref[i]);
        auto neg0 = std::find_if(v.begin(), v.end(), [](double x) { return x == 0 && std::signbit(x); });
        auto pos0 = std::find_if(v.begin(), v.end(), [](double x) { return x == 0 && !std::signbit(x); });
        assert(neg0 < pos0);    // radix order is total: -0.0 before +0.0
        std::cout << "  radix_sort<double> == std::sort incl. +-inf, denormals; -0.0 sorts before +0.0\n";
    }
    std::cout << "  key map example: int32 -1 -> 0x" << std::hex << radix_key(int32_t{-1})
              << ", 0 -> 0x" << radix_key(int32_t{0}) << std::dec << '\n';
    assert(radix_key(int32_t{-1}) < radix_key(int32_t{0}));

    // ---- stable counting sort of packets by priority ------------------------
    std::vector<Packet> pk;
    for (int i = 0; i < 2000; ++i)
        pk.push_back({static_cast<int>(rng() % 8), i, "p" + std::to_string(i)});
    // highest priority first: key = 7 - priority
    auto sorted = counting_sort_by(pk, 8, [](const Packet &p) { return 7 - p.priority; });
    auto ref = pk;
    std::stable_sort(ref.begin(), ref.end(), [](const Packet &a, const Packet &b) { return a.priority > b.priority; });
    for (size_t i = 0; i < pk.size(); ++i) assert(sorted[i].seq == ref[i].seq);
    std::cout << "\ncounting_sort_by(priority) on 2000 packets == std::stable_sort\n"
              << "  (O(n + 8): this is how a strict-priority packet scheduler buckets traffic)\n";

    // ---- bucket sort --------------------------------------------------------
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (int t = 0; t < 100; ++t) {
        std::vector<double> v(rng() % 3000);
        for (auto &x : v) x = u01(rng);
        auto r = v;
        std::sort(r.begin(), r.end());
        bucket_sort(v);
        assert(v == r);
    }
    std::cout << "\nbucket_sort == std::sort on 100 uniform [0,1) vectors\n";

    std::cout << "\nAll counting/radix/bucket checks passed.\n";
    return 0;
}
