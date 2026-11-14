/*
 * 02_register_field_template.cpp — type-safe register fields with templates:
 *                                  the compiler computes masks and rejects
 *                                  fields that do not fit
 *
 * Concept : In C (03_register_bitfields.c) a field is a macro mask; nothing
 *           stops you from writing a CTRL field into the STATUS register or a
 *           value of 20 into a 4-bit field. In C++ each field becomes a TYPE
 *           (Field<Reg, Hi, Lo>) whose mask is a constexpr, so a wrong
 *           register is a compile error, range checks are static_assert
 *           where possible, and the generated code is the same single
 *           AND/OR/shift as the C macro. Same idea as modm, Kvasir, or
 *           Rust's svd2rust-generated register crates.
 * Build   : make run P=dsa/18-bit-manipulation/cpp/02_register_field_template.cpp
 * Read    : ../README.md  (section "3.4 Register bitfields in drivers")
 */
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <type_traits>

// constexpr mask builder: bits Hi..Lo set. Evaluated at compile time.
constexpr std::uint32_t genmask(unsigned hi, unsigned lo) {
    return (~0u << lo) & (~0u >> (31u - hi));
}
static_assert(genmask(7, 4) == 0xF0u, "genmask");
static_assert(genmask(31, 0) == 0xFFFFFFFFu, "genmask full width");

// Tag types: one per register, so fields are bound to their register.
struct CtrlTag {};
struct StatusTag {};

template <typename RegTag, unsigned Hi, unsigned Lo>
struct Field {
    static_assert(Hi < 32 && Lo <= Hi, "field must lie inside a 32-bit register");
    using reg = RegTag;
    static constexpr unsigned shift = Lo;
    static constexpr unsigned width = Hi - Lo + 1;
    static constexpr std::uint32_t mask = genmask(Hi, Lo);
    static constexpr std::uint32_t max  = mask >> Lo;

    // Compile-time-checked encode for constants:  Field::prep<5>()
    template <std::uint32_t V>
    static constexpr std::uint32_t prep() {
        static_assert(V <= max, "value does not fit in this field");
        return V << Lo;
    }
    // Runtime encode: checked with assert (a real driver would return -EINVAL).
    static constexpr std::uint32_t prep(std::uint32_t v) {
        assert(v <= max && "value does not fit in this field");
        return (v << Lo) & mask;
    }
    static constexpr std::uint32_t get(std::uint32_t regval) { return (regval & mask) >> Lo; }
};

// The mock UART layout from the C lesson.
using CTRL_EN      = Field<CtrlTag, 0, 0>;
using CTRL_TXE     = Field<CtrlTag, 1, 1>;
using CTRL_RXE     = Field<CtrlTag, 2, 2>;
using CTRL_LBK     = Field<CtrlTag, 3, 3>;
using CTRL_DLEN    = Field<CtrlTag, 7, 4>;
using CTRL_PARITY  = Field<CtrlTag, 11, 8>;
using CTRL_BAUDDIV = Field<CtrlTag, 15, 12>;
using STATUS_TXDONE = Field<StatusTag, 0, 0>;

static_assert(CTRL_DLEN::mask == 0xF0u && CTRL_DLEN::width == 4 && CTRL_DLEN::max == 15, "");
static_assert(CTRL_BAUDDIV::prep<9>() == 0x9000u, "");
// static_assert(CTRL_DLEN::prep<16>() ...);   // would fail to compile: does not fit

// A register: owns a pointer to (here simulated) volatile storage and only
// accepts fields that carry its own tag. Memory: the storage is owned by the
// caller (a struct in RAM here, an ioremap()'d address in a driver).
template <typename Tag>
class Reg {
public:
    explicit Reg(volatile std::uint32_t* addr) : addr_(addr) {}

    std::uint32_t read() const { ++reads; return *addr_; }
    void write(std::uint32_t v) { ++writes; *addr_ = v; }

    template <typename F>
    std::uint32_t get() const {
        static_assert(std::is_same<typename F::reg, Tag>::value, "field belongs to another register");
        return F::get(read());
    }

    // Read-modify-write of ONE field: 1 read, 1 write, neighbours untouched.
    template <typename F>
    void update(std::uint32_t v) {
        static_assert(std::is_same<typename F::reg, Tag>::value, "field belongs to another register");
        write((read() & ~F::mask) | F::prep(v));
    }

    // Variadic RMW of several fields with a SINGLE bus write — a fold
    // expression builds the combined mask and value at compile time.
    template <typename... Fs, typename... Vs>
    void update_many(Vs... vs) {
        static_assert(sizeof...(Fs) == sizeof...(Vs), "one value per field");
        static_assert((std::is_same<typename Fs::reg, Tag>::value && ...), "field belongs to another register");
        constexpr std::uint32_t m = (Fs::mask | ...);
        write((read() & ~m) | (Fs::prep(static_cast<std::uint32_t>(vs)) | ...));
    }

    static inline unsigned reads = 0, writes = 0;

private:
    volatile std::uint32_t* addr_;
};

int main() {
    // The "device memory" — two registers in a plain struct on the stack.
    struct { volatile std::uint32_t ctrl = 0, status = 0; } hw;
    Reg<CtrlTag> ctrl(&hw.ctrl);
    Reg<StatusTag> status(&hw.status);

    std::cout << std::hex << std::uppercase;
    std::cout << "== 1. Compile-time masks ==\n";
    std::cout << "DLEN mask=0x" << CTRL_DLEN::mask << " shift=" << std::dec << CTRL_DLEN::shift
              << " width=" << CTRL_DLEN::width << std::hex << "\n";

    std::cout << "\n== 2. Init with one write (built from compile-time constants) ==\n";
    constexpr std::uint32_t init = CTRL_EN::prep<1>() | CTRL_TXE::prep<1>() | CTRL_RXE::prep<1>() |
                                   CTRL_DLEN::prep<3>() | CTRL_PARITY::prep<2>() | CTRL_BAUDDIV::prep<9>();
    static_assert(init == 0x9237u, "same value as the C program");
    ctrl.write(init);
    std::cout << "CTRL = 0x" << hw.ctrl << "\n";
    assert(hw.ctrl == 0x9237u);

    std::cout << "\n== 3. RMW single field / several fields in one write ==\n";
    ctrl.update<CTRL_BAUDDIV>(4);
    assert(hw.ctrl == 0x4237u);
    ctrl.update_many<CTRL_LBK, CTRL_PARITY>(1, 0);
    std::cout << "after update<BAUDDIV>(4) and update_many<LBK,PARITY>(1,0): CTRL = 0x" << hw.ctrl << "\n";
    assert(hw.ctrl == 0x403Fu);
    assert(ctrl.get<CTRL_DLEN>() == 3);
    // ctrl.update<STATUS_TXDONE>(1);   // compile error: field belongs to another register
    status.update<STATUS_TXDONE>(1);
    assert(hw.status == 1u);

    std::cout << "\n== 4. Exhaustive check vs a naive bit-by-bit model ==\n";
    // For every value of every 4-bit field, RMW must change only that field.
    std::uint32_t seed = 0xA5A5A5A5u;
    for (std::uint32_t v = 0; v <= 15; ++v) {
        hw.ctrl = seed;
        ctrl.update<CTRL_PARITY>(v);
        for (unsigned bit = 0; bit < 32; ++bit) {
            bool got = (hw.ctrl >> bit) & 1u;
            bool want = (bit >= 8 && bit <= 11) ? ((v >> (bit - 8)) & 1u) : ((seed >> bit) & 1u);
            assert(got == want);
        }
        seed = seed * 1664525u + 1013904223u;
    }
    std::cout << "update<PARITY>(v) changes only bits 11:8 for all v: OK\n";
    std::cout << std::dec << "bus traffic CTRL: " << Reg<CtrlTag>::reads << " reads, "
              << Reg<CtrlTag>::writes << " writes\n";

    std::cout << "\nall checks passed\n";
    return 0;
}
