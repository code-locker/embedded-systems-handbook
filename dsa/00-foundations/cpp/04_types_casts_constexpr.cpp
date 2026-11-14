/*
 * 04_types_casts_constexpr.cpp — C++ type tools: auto, the 4 named casts, constexpr, numeric_limits
 *
 * Concept : A C cast (T)x can do five different things silently. C++ splits
 *           them into named casts so the dangerous ones are greppable:
 *             static_cast      - ordinary conversions (int<->double, up/down class)
 *             const_cast       - add/remove const (only safe if object is non-const)
 *             reinterpret_cast - reinterpret bits/addresses (MMIO, serialisation)
 *             dynamic_cast     - checked downcast using RTTI
 *           Brace initialisation {} refuses narrowing conversions.
 * Build   : make run P=dsa/00-foundations/cpp/04_types_casts_constexpr.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>

struct Base { virtual ~Base() = default; };
struct Derived : Base { int extra = 7; };

constexpr std::uint32_t bit(unsigned n) { return 1u << n; }   // compile-time register masks
constexpr std::uint32_t CTRL_ENABLE = bit(0) | bit(4);

int main()
{
    // numeric_limits: the C++ face of <limits.h>
    std::cout << "int range: " << std::numeric_limits<int>::min() << " .. "
              << std::numeric_limits<int>::max() << '\n';
    static_assert(std::numeric_limits<std::uint8_t>::max() == 255, "");

    // auto deduces from the initialiser (drops top-level const and references)
    auto a = 3u;                      // unsigned
    auto d = 2.5;                     // double
    static_assert(sizeof(a) == sizeof(unsigned), "");
    (void)d;

    // Brace init forbids narrowing: int n{2.5}; is a compile error. () allows it.
    int n(static_cast<int>(2.9));     // explicit: we mean truncation
    assert(n == 2);

    // const_cast: legal to remove const only when the object itself is not const.
    int mutable_obj = 1;
    const int &view = mutable_obj;
    const_cast<int &>(view) = 2;      // OK: mutable_obj was never const
    assert(mutable_obj == 2);

    // reinterpret_cast: treat an address as an integer and back (what MMIO code does).
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(&mutable_obj);
    int *back = reinterpret_cast<int *>(addr);
    assert(back == &mutable_obj);
    // Reading a float's bits: memcpy, not reinterpret_cast<uint32_t&>(f) (aliasing UB).
    float f = -0.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    assert(bits == 0x80000000u);

    // dynamic_cast: returns nullptr when the object is not really a Derived.
    Derived der;
    Base base;
    Base *pb1 = &der, *pb2 = &base;
    assert(dynamic_cast<Derived *>(pb1) != nullptr);
    assert(dynamic_cast<Derived *>(pb2) == nullptr);

    static_assert(CTRL_ENABLE == 0x11, "computed by the compiler, zero runtime cost");
    std::cout << "CTRL_ENABLE = 0x" << std::hex << CTRL_ENABLE << std::dec
              << " (constexpr)\nAll checks passed.\n";
    return 0;
}
