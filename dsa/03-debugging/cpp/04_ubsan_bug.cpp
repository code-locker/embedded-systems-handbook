/*
 * 04_ubsan_bug.cpp — INTENTIONAL BUGS: undefined behaviour that UBSan reports
 *
 * Bug     : Four classic UBs in one run: signed integer overflow, shift
 *           exponent too large, out-of-bounds index on a fixed array, and a
 *           misaligned load. Each one "works" on x86 most of the time —
 *           which is exactly why they survive until an optimiser or an ARM
 *           core disagrees.
 * Finds it: UndefinedBehaviorSanitizer (-fsanitize=undefined, on in this repo):
 *           "runtime error: signed integer overflow: 2147483647 + 1 cannot be
 *           represented in type 'int'", etc. This repo builds with
 *           -fno-sanitize-recover=all, so the program STOPS at the first
 *           report. Plain -fsanitize=undefined reports and CONTINUES, which
 *           shows all four in one run (command below). table[4] is also an
 *           ASan global-buffer-overflow.
 * Build   : make run P=dsa/03-debugging/cpp/04_ubsan_bug.cpp     -> first report, exit 1
 *           All four: g++ -std=c++17 -g -fsanitize=undefined dsa/03-debugging/cpp/04_ubsan_bug.cpp -o /tmp/ub && /tmp/ub
 * Read    : ../README.md  (section "Bug 10: undefined behaviour (UBSan)")
 */
#include <climits>
#include <cstdint>
#include <iostream>

static int add(int a, int b) { return a + b; }                       // overflows for INT_MAX + 1
static std::uint32_t mask(unsigned bits) { return 1u << bits; }      // UB for bits >= 32
static int table_lookup(int i)
{
    static int table[4] = {10, 20, 30, 40};
    return table[i];                                                  // UB for i == 4
}
static std::uint32_t read_u32(const unsigned char *p)
{
    return *reinterpret_cast<const std::uint32_t *>(p);              // misaligned if p % 4 != 0
}

int main(int argc, char **)
{
    volatile int big = INT_MAX;            // volatile: keep the compiler from folding it away
    unsigned shift = 31 + static_cast<unsigned>(argc);   // 32 when run without arguments
    int idx = 3 + argc;                                  // 4

    std::cout << "INT_MAX + 1  = " << add(big, 1) << '\n';
    std::cout << "1u << 32     = " << mask(shift) << '\n';

    alignas(4) unsigned char buf[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    std::cout << "u32 at buf+1 = 0x" << std::hex << read_u32(buf + 1) << std::dec << '\n';
    // Correct: std::uint32_t v; std::memcpy(&v, buf + 1, 4);

    std::cout << "table[4]     = " << table_lookup(idx) << '\n';
    return 0;
}
