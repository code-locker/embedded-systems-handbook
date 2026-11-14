/*
 * 04_sudoku.cpp — Sudoku solver class: bitmask candidates, most-constrained
 *                 cell first, and counting solutions (uniqueness check)
 *
 * Concept : Same backtracking as the C version, wrapped in a class with
 *           std::array storage (no heap). A second mode COUNTS solutions and
 *           stops at a limit — that is how puzzle generators prove a puzzle
 *           has exactly one answer (count with limit 2: result 1 = unique).
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/04_sudoku.cpp
 * Read    : ../README.md  (section "Sudoku solver")
 */
#include <array>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

class Sudoku {
public:
    using Grid = std::array<std::array<int, 9>, 9>;

    // Returns nullopt if the clues already contradict each other.
    static std::optional<Sudoku> parse(std::string_view s) {
        Sudoku z;
        for (int i = 0; i < 81; ++i) {
            if (s[i] == '.' || s[i] == '0') continue;
            if (!z.can(i / 9, i % 9, s[i] - '0')) return std::nullopt;
            z.set(i / 9, i % 9, s[i] - '0');
        }
        return z;
    }

    bool solve() { return search(1) == 1; }               // find one
    long count_solutions(long limit) { Sudoku copy = *this; return copy.search(limit); }

    const Grid& grid() const { return g_; }

    bool valid_complete() const {
        for (int i = 0; i < 9; ++i) {
            std::bitset<10> r, c, b;
            for (int j = 0; j < 9; ++j) {
                r.set(g_[i][j]); c.set(g_[j][i]);
                b.set(g_[i / 3 * 3 + j / 3][i % 3 * 3 + j % 3]);
            }
            if (r.test(0) || c.test(0) || b.test(0)) return false;  // 0 = empty
            if (r.count() != 9 || c.count() != 9 || b.count() != 9) return false;
        }
        return true;
    }

    friend std::ostream& operator<<(std::ostream& os, const Sudoku& z) {
        for (int r = 0; r < 9; ++r) {
            os << "  ";
            for (int c = 0; c < 9; ++c) os << (z.g_[r][c] ? char('0' + z.g_[r][c]) : '.') << (c % 3 == 2 ? "  " : " ");
            os << '\n';
            if (r % 3 == 2 && r < 8) os << '\n';
        }
        return os;
    }

private:
    static int box(int r, int c) { return r / 3 * 3 + c / 3; }
    std::uint16_t free_mask(int r, int c) const {
        return std::uint16_t(~(row_[r] | col_[c] | box_[box(r, c)]) & 0x3FE);
    }
    bool can(int r, int c, int d) const { return g_[r][c] == 0 && (free_mask(r, c) >> d & 1); }
    void set(int r, int c, int d) {
        const std::uint16_t bit = std::uint16_t(1u << d);
        g_[r][c] = d; row_[r] |= bit; col_[c] |= bit; box_[box(r, c)] |= bit;
    }
    void unset(int r, int c) {
        const std::uint16_t bit = std::uint16_t(1u << g_[r][c]);
        row_[r] &= std::uint16_t(~bit); col_[c] &= std::uint16_t(~bit); box_[box(r, c)] &= std::uint16_t(~bit);
        g_[r][c] = 0;
    }

    // Returns the number of solutions found, never more than `limit`.
    // When limit == 1 and a solution is found, the grid is LEFT solved.
    long search(long limit) {
        int br = -1, bc = -1, best = 10;
        for (int r = 0; r < 9; ++r)
            for (int c = 0; c < 9; ++c)
                if (!g_[r][c]) {
                    const int n = std::bitset<16>(free_mask(r, c)).count();
                    if (n < best) { best = n; br = r; bc = c; }
                }
        if (br < 0) return 1;                     // full grid = one solution
        if (best == 0) return 0;                  // dead end: prune
        long found = 0;
        for (int d = 1; d <= 9 && found < limit; ++d) {
            if (!(free_mask(br, bc) >> d & 1)) continue;
            set(br, bc, d);                       // choose
            found += search(limit - found);       // explore
            if (found >= limit && limit == 1) return found;  // keep solved grid
            unset(br, bc);                        // un-choose
        }
        return found;
    }

    Grid g_{};                                    // value-initialised: all 0
    std::array<std::uint16_t, 9> row_{}, col_{}, box_{};
};

int main() {
    constexpr std::string_view puzzle =
        "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79";
    constexpr std::string_view expected =
        "534678912672195348198342567859761423426853791713924856961537284287419635345286179";

    auto z = Sudoku::parse(puzzle);
    assert(z);
    std::cout << "puzzle:\n" << *z;
    assert(z->count_solutions(2) == 1);           // proven unique
    assert(z->solve() && z->valid_complete());
    for (int i = 0; i < 81; ++i) assert(z->grid()[i / 9][i % 9] == expected[i] - '0');
    std::cout << "\nsolution (unique, matches the published answer):\n" << *z;

    // An empty grid has MANY solutions; a counting search with a limit stops early.
    auto empty = Sudoku::parse(std::string(81, '.'));
    assert(empty && empty->count_solutions(2) == 2);
    assert(empty->count_solutions(50) == 50);
    assert(empty->solve() && empty->valid_complete());
    std::cout << "\nempty grid: >= 50 solutions (search stopped at the limit), one of them is valid\n";

    // Remove five clues from the unique puzzle: now several solutions.
    std::string loose(puzzle);
    loose[0] = '.'; loose[1] = '.'; loose[4] = '.'; loose[9] = '.'; loose[12] = '.';
    auto l = Sudoku::parse(loose);
    assert(l);
    const long cnt = l->count_solutions(1000);
    std::cout << "with 5 clues removed: " << cnt << " solution(s)\n";
    assert(cnt >= 1);

    // Contradictory clues are rejected at parse time.
    std::string bad(puzzle);
    bad[1] = '5';                                 // second 5 in row 0
    assert(!Sudoku::parse(bad));
    std::cout << "contradictory puzzle rejected by parse()\n";

    std::cout << "\nall sudoku checks passed\n";
    return 0;
}
