/*
 * 06_sudoku_solver.c — Sudoku by backtracking, plain and with pruning
 *
 * Concept : Find an empty cell, try digits 1..9 that do not clash with its
 *           row / column / 3x3 box, recurse, undo on failure.
 *           Version A picks the FIRST empty cell (simple).
 *           Version B picks the cell with the FEWEST candidates (the "most
 *           constrained variable" / MRV heuristic) and keeps candidates as
 *           9-bit masks, so a check is one AND instead of 27 compares.
 *           Both are verified by an independent validator, by agreeing with
 *           each other, and by preserving every given clue.
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/06_sudoku_solver.c
 * Read    : ../README.md  (sections "Sudoku solver", "Pruning")
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef int Grid[9][9];             /* 0 = empty */

/* ---- Version A: straightforward ---------------------------------------- */
static long nodes_a;

static int can_place(Grid g, int r, int c, int d)
{
    int br = r / 3 * 3, bc = c / 3 * 3;            /* top-left of the box */
    for (int i = 0; i < 9; ++i) {
        if (g[r][i] == d || g[i][c] == d) return 0;
        if (g[br + i / 3][bc + i % 3] == d) return 0;
    }
    return 1;
}

static int solve_a(Grid g)
{
    ++nodes_a;
    for (int r = 0; r < 9; ++r)
        for (int c = 0; c < 9; ++c) {
            if (g[r][c]) continue;
            for (int d = 1; d <= 9; ++d) {
                if (!can_place(g, r, c, d)) continue;
                g[r][c] = d;                       /* CHOOSE               */
                if (solve_a(g)) return 1;          /* EXPLORE: stop at the */
                g[r][c] = 0;                       /* first solution       */
            }                                      /* UN-CHOOSE            */
            return 0;      /* no digit fits this cell -> dead end, back up */
        }
    return 1;              /* no empty cell left: solved                   */
}

/* ---- Version B: bitmasks + most-constrained cell ----------------------- */
/*
 * rows[r] bit d set  <=> digit d already used in row r (same for cols, boxes).
 * Candidates of (r,c) = ~(rows[r] | cols[c] | boxes[b]) & 0x3FE   (bits 1..9)
 *
 *   rows[r]  = 0b0100100110   (digits 1,2,5,8 used)
 *   cols[c]  = 0b0000011000   (3,4)
 *   boxes[b] = 0b1000000010   (1,9)
 *   OR       = 0b1100111110
 *   free     = 0b0011000000   -> digits 6 and 7 only: 2 branches, not 9
 */
static unsigned rows[9], cols[9], boxes[9];
static long nodes_b;

static int box_of(int r, int c) { return r / 3 * 3 + c / 3; }

static int solve_b(Grid g)
{
    ++nodes_b;
    int best_r = -1, best_c = -1, best_cnt = 10;
    unsigned best_free = 0;
    for (int r = 0; r < 9; ++r)
        for (int c = 0; c < 9; ++c) {
            if (g[r][c]) continue;
            unsigned free = ~(rows[r] | cols[c] | boxes[box_of(r, c)]) & 0x3FEu;
            int cnt = __builtin_popcount(free);
            if (cnt < best_cnt) { best_cnt = cnt; best_r = r; best_c = c; best_free = free; }
            if (cnt == 0) return 0;     /* PRUNE: some cell is impossible now */
        }
    if (best_r < 0) return 1;           /* no empty cell: solved */
    int r = best_r, c = best_c, b = box_of(r, c);
    while (best_free) {
        int d = __builtin_ctz(best_free);           /* lowest candidate digit */
        best_free &= best_free - 1;                 /* drop it from the set   */
        unsigned bit = 1u << d;
        g[r][c] = d; rows[r] |= bit; cols[c] |= bit; boxes[b] |= bit;
        if (solve_b(g)) return 1;
        g[r][c] = 0; rows[r] &= ~bit; cols[c] &= ~bit; boxes[b] &= ~bit;
    }
    return 0;
}

static int init_masks(Grid g)
{
    memset(rows, 0, sizeof rows); memset(cols, 0, sizeof cols); memset(boxes, 0, sizeof boxes);
    for (int r = 0; r < 9; ++r)
        for (int c = 0; c < 9; ++c) {
            int d = g[r][c];
            if (!d) continue;
            unsigned bit = 1u << d;
            if ((rows[r] | cols[c] | boxes[box_of(r, c)]) & bit) return 0; /* bad puzzle */
            rows[r] |= bit; cols[c] |= bit; boxes[box_of(r, c)] |= bit;
        }
    return 1;
}

/* ---- independent validator --------------------------------------------- */
static int valid_solution(Grid g)
{
    for (int i = 0; i < 9; ++i) {
        unsigned rm = 0, cm = 0, bm = 0;
        for (int j = 0; j < 9; ++j) {
            int a = g[i][j], b = g[j][i], c = g[i / 3 * 3 + j / 3][i % 3 * 3 + j % 3];
            if (a < 1 || a > 9 || b < 1 || b > 9 || c < 1 || c > 9) return 0;
            rm |= 1u << a; cm |= 1u << b; bm |= 1u << c;
        }
        if (rm != 0x3FEu || cm != 0x3FEu || bm != 0x3FEu) return 0;
    }
    return 1;
}

static void load(Grid g, const char *s)
{
    for (int i = 0; i < 81; ++i) g[i / 9][i % 9] = s[i] == '.' ? 0 : s[i] - '0';
}

static void print_grid(Grid g)
{
    for (int r = 0; r < 9; ++r) {
        if (r && r % 3 == 0) printf("  ------+-------+------\n");
        printf("  ");
        for (int c = 0; c < 9; ++c)
            printf("%c%s", g[r][c] ? '0' + g[r][c] : '.', c % 3 == 2 && c < 8 ? " | " : " ");
        printf("\n");
    }
}

int main(void)
{
    static const char *puzzles[] = {
        /* classic easy one (Wikipedia's example) */
        "53..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79",
        /* a "hard" one for naive solvers */
        "..9748...7.........2.1.9.....7...24..64.1.59..98...3.....8.3.2.........6...2759..",
        /* very sparse: 17 clues (minimum possible for a unique sudoku) */
        "...8.1..........435............7.8........1...2..3....6......75..34........2..6..",
    };
    int np = (int)(sizeof puzzles / sizeof puzzles[0]);

    for (int p = 0; p < np; ++p) {
        Grid ga, gb, orig;
        load(orig, puzzles[p]);
        memcpy(ga, orig, sizeof ga);
        memcpy(gb, orig, sizeof gb);

        if (p == 0) { printf("puzzle 0:\n"); print_grid(orig); }

        nodes_b = 0;
        assert(init_masks(gb));
        assert(solve_b(gb));
        assert(valid_solution(gb));

        if (p < 2) {                    /* the 17-clue one is slow for version A */
            nodes_a = 0;
            assert(solve_a(ga));
            assert(valid_solution(ga));
            assert(memcmp(ga, gb, sizeof ga) == 0);   /* unique puzzle -> same */
            printf("puzzle %d: plain = %ld nodes, bitmask+MRV = %ld nodes\n", p, nodes_a, nodes_b);
        } else {
            printf("puzzle %d (17 clues): bitmask+MRV = %ld nodes\n", p, nodes_b);
        }
        for (int r = 0; r < 9; ++r)               /* clues never changed */
            for (int c = 0; c < 9; ++c)
                if (orig[r][c]) assert(gb[r][c] == orig[r][c]);
        if (p == 0) { printf("solution:\n"); print_grid(gb); }
    }

    /* An impossible puzzle: two 5s in the first row. */
    Grid bad;
    load(bad, "55..7....6..195....98....6.8...6...34..8.3..17...2...6.6....28....419..5....8..79");
    assert(!init_masks(bad));
    /* A subtly impossible one: consistent clues, but cell (0,8) has no candidate. */
    load(bad, "12345678.........9...............................................................");
    assert(init_masks(bad));
    nodes_b = 0;
    assert(!solve_b(bad));
    printf("unsolvable puzzle rejected after %ld node(s) (MRV sees the empty cell with 0 candidates)\n", nodes_b);

    printf("\nall sudoku checks passed\n");
    return 0;
}
