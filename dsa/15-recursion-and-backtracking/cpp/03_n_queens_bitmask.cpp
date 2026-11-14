/*
 * 03_n_queens_bitmask.cpp — N-Queens two ways: readable (vectors of bool) and
 *                           fast (three bitmasks shifted per row)
 *
 * Concept : The bitmask version keeps "attacked columns" for the CURRENT row
 *           in three machine words. Moving down one row, diagonal attacks
 *           move one column left/right — which is just a shift:
 *
 *              row r  : cols=0b00100  ld=0b00100  rd=0b00100   (queen at col 2)
 *              row r+1: cols=0b00100  ld=0b01000  rd=0b00010   ld<<1, rd>>1
 *              free   = ~(cols|ld|rd) & full = 0b10001
 *
 *           Picking candidates is `free & -free` (lowest set bit). No loops
 *           over columns, no arrays: ~10x faster. Both versions must agree
 *           with the known counts (OEIS A000170).
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/03_n_queens_bitmask.cpp
 * Read    : ../README.md  (section "N-Queens")
 */
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Readable version: also returns the boards.
// ---------------------------------------------------------------------------
class NQueens {
public:
    explicit NQueens(int n) : n_(n), cols_(n), d1_(2 * n), d2_(2 * n), q_(n) {}

    std::vector<std::vector<std::string>> solve_all() {
        place(0);
        return std::move(boards_);
    }

private:
    void place(int r) {
        if (r == n_) { boards_.push_back(render()); return; }
        for (int c = 0; c < n_; ++c) {
            if (cols_[c] || d1_[r + c] || d2_[r - c + n_]) continue;      // prune
            q_[r] = c;
            cols_[c] = d1_[r + c] = d2_[r - c + n_] = true;                 // choose
            place(r + 1);                                                   // explore
            cols_[c] = d1_[r + c] = d2_[r - c + n_] = false;                // un-choose
        }
    }
    std::vector<std::string> render() const {
        std::vector<std::string> b(n_, std::string(n_, '.'));
        for (int r = 0; r < n_; ++r) b[r][q_[r]] = 'Q';
        return b;
    }

    int n_;
    std::vector<bool> cols_, d1_, d2_;      // vector<bool>: packed bits already
    std::vector<int> q_;
    std::vector<std::vector<std::string>> boards_;
};

// ---------------------------------------------------------------------------
// Bitmask version: counts only.
// ---------------------------------------------------------------------------
static long count_bits(int n, std::uint32_t cols, std::uint32_t ld, std::uint32_t rd) {
    const std::uint32_t full = (1u << n) - 1;
    if (cols == full) return 1;                       // every column has a queen
    long total = 0;
    std::uint32_t free = ~(cols | ld | rd) & full;    // safe squares in this row
    while (free) {
        const std::uint32_t bit = free & (0u - free); // lowest set bit
        free ^= bit;                                  // remove it from the set
        // choose `bit`, explore next row; un-choose is free: the masks are
        // passed BY VALUE, so the caller's copies were never modified.
        total += count_bits(n, cols | bit, ((ld | bit) << 1) & full, (rd | bit) >> 1);
    }
    return total;
}

static bool valid(const std::vector<std::string>& b) {
    const int n = int(b.size());
    std::vector<int> q(n);
    for (int r = 0; r < n; ++r) q[r] = int(b[r].find('Q'));
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            if (q[i] == q[j] || std::abs(q[i] - q[j]) == j - i) return false;
    return true;
}

int main() {
    constexpr std::array<long, 14> known = { 1, 1, 0, 0, 2, 10, 4, 40, 92, 352, 724, 2680, 14200, 73712 };
    using clk = std::chrono::steady_clock;

    std::cout << " N  solutions  readable(ms)  bitmask(ms)\n";
    for (int n = 1; n <= 13; ++n) {
        const auto t0 = clk::now();
        long readable = -1;
        if (n <= 11) {                              // keep the slow one short
            auto boards = NQueens(n).solve_all();
            readable = long(boards.size());
            for (const auto& b : boards) assert(valid(b));
            if (n == 6) {
                std::cout << "  the 4 solutions for N=6, first one:\n";
                for (const auto& row : boards[0]) std::cout << "    " << row << '\n';
            }
        }
        const auto t1 = clk::now();
        const long fast = count_bits(n, 0, 0, 0);
        const auto t2 = clk::now();
        assert(fast == known[n]);
        if (readable >= 0) assert(readable == fast);
        auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        std::cout << ' ' << (n < 10 ? " " : "") << n << "  " << fast << "\t     "
                  << (readable >= 0 ? std::to_string(ms(t1 - t0)).substr(0, 6) : "  -   ")
                  << "\t   " << std::to_string(ms(t2 - t1)).substr(0, 6) << '\n';
    }
    std::cout << "\nall N-Queens checks passed (times are -O0 + sanitizers; try OPT=1 SAN=0)\n";
    return 0;
}
