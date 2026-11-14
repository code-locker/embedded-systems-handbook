/*
 * 03_recursion_to_iteration.c — converting recursion into loops with an
 *                               explicit stack
 *
 * Concept : The CPU's call stack is just a stack of frames. You can keep the
 *           same frames yourself in a heap array. That removes the depth
 *           limit (8 MB user stack, 16 KB kernel stack!) and is how the Linux
 *           kernel and embedded firmware avoid deep recursion.
 *           Three recipes: (1) tail recursion -> plain loop, (2) "pre-order"
 *           recursion -> push children, (3) general recursion -> frame
 *           struct with a "resume point" (state machine).
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/03_recursion_to_iteration.c
 * Read    : ../README.md  (section "Recursion -> iteration")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Recipe 2: flood fill (DFS on a grid)                                       */
/* ------------------------------------------------------------------------- */
#define R 8
#define C 12
static const char *grid_init[R] = {
    "............",
    ".####..###..",
    ".#..#..#.#..",
    ".#..####.#..",
    ".#.......#..",
    ".#########..",
    "...........#",
    "##########.#",
};

/* Recursive: fill the 4-connected region of '.' starting at (r,c). Depth can
   reach R*C — on a 4000x4000 image that is 16M frames: stack overflow. */
static int fill_rec(char g[R][C + 1], int r, int c)
{
    if (r < 0 || r >= R || c < 0 || c >= C || g[r][c] != '.')
        return 0;
    g[r][c] = 'o';
    return 1 + fill_rec(g, r + 1, c) + fill_rec(g, r - 1, c)
             + fill_rec(g, r, c + 1) + fill_rec(g, r, c - 1);
}

/* Iterative: the "frame" is just (r,c). Push work instead of calling.
   Memory: we own `stack` (malloc'd here, freed here). Worst case each cell is
   pushed by each of its 4 neighbours -> capacity 4*R*C + 1. */
static int fill_iter(char g[R][C + 1], int r0, int c0)
{
    typedef struct { int r, c; } Cell;
    size_t cap = 4u * R * C + 1, sp = 0;
    Cell *stack = malloc(cap * sizeof *stack);
    assert(stack);
    int count = 0;
    stack[sp++] = (Cell){ r0, c0 };                 /* "call" fill(r0,c0)   */
    while (sp > 0) {
        Cell x = stack[--sp];                       /* "enter" a frame       */
        if (x.r < 0 || x.r >= R || x.c < 0 || x.c >= C || g[x.r][x.c] != '.')
            continue;                               /* base case = skip      */
        g[x.r][x.c] = 'o';
        ++count;
        assert(sp + 4 <= cap);
        stack[sp++] = (Cell){ x.r + 1, x.c };       /* the 4 recursive calls */
        stack[sp++] = (Cell){ x.r - 1, x.c };
        stack[sp++] = (Cell){ x.r, x.c + 1 };
        stack[sp++] = (Cell){ x.r, x.c - 1 };
    }
    free(stack);
    return count;
}

/* ------------------------------------------------------------------------- */
/* Recipe 3: Hanoi with explicit frames and resume points                      */
/* ------------------------------------------------------------------------- */
/*
 *   void hanoi(n, from, to, via) {
 *       if (n == 0) return;
 *       hanoi(n-1, from, via, to);     // <- resume point 0 -> 1
 *       move(from, to);                // stage 1
 *       hanoi(n-1, via, to, from);     // <- resume point 1 -> 2
 *   }                                  // stage 2: return
 *
 * A frame remembers its arguments AND where to continue ("stage"), exactly
 * like the saved return address in a real stack frame:
 *
 *   explicit stack (top on the right), n=2:
 *   [ {2,A,C,B,s0} ]
 *   [ {2,..,s1} {1,A,B,C,s0} ]
 *   [ {2,..,s1} {1,..,s1} {0,..} ] -> n==0 pops immediately
 *   [ {2,..,s1} {1,..,s1} ]        -> move A->B, push second child ...
 */
typedef struct { int n, from, to, via, stage; } Frame;

static int moves_rec_log[1 << 12][2], nrec;
static int moves_it_log[1 << 12][2], nit;

static void hanoi_rec(int n, int from, int to, int via)
{
    if (n == 0) return;
    hanoi_rec(n - 1, from, via, to);
    moves_rec_log[nrec][0] = from; moves_rec_log[nrec][1] = to; ++nrec;
    hanoi_rec(n - 1, via, to, from);
}

static void hanoi_iter(int n, int from, int to, int via)
{
    Frame stack[32];                     /* depth is n+1 <= 32: fixed array OK */
    int sp = 0;
    stack[sp++] = (Frame){ n, from, to, via, 0 };
    while (sp > 0) {
        Frame *f = &stack[sp - 1];
        if (f->n == 0) { --sp; continue; }            /* base case: return */
        switch (f->stage) {
        case 0:                                       /* first recursive call */
            f->stage = 1;
            stack[sp++] = (Frame){ f->n - 1, f->from, f->via, f->to, 0 };
            break;                                    /* NB: f may now be stale
                                                         only if we realloc'd;
                                                         fixed array -> fine */
        case 1:                                       /* the work + 2nd call */
            moves_it_log[nit][0] = f->from; moves_it_log[nit][1] = f->to; ++nit;
            f->stage = 2;
            stack[sp++] = (Frame){ f->n - 1, f->via, f->to, f->from, 0 };
            break;
        default:                                      /* stage 2: return */
            --sp;
        }
        assert(sp <= 32);
    }
}

/* ------------------------------------------------------------------------- */
/* Recipe 1: tail recursion -> loop (binary search)                           */
/* ------------------------------------------------------------------------- */
static int bsearch_rec(const int *a, int lo, int hi, int key)   /* [lo, hi) */
{
    if (lo >= hi) return -1;
    int mid = lo + (hi - lo) / 2;
    if (a[mid] == key) return mid;
    return a[mid] < key ? bsearch_rec(a, mid + 1, hi, key)    /* tail calls */
                        : bsearch_rec(a, lo, mid, key);
}

static int bsearch_iter(const int *a, int lo, int hi, int key)
{
    while (lo < hi) {                     /* same test as the base case       */
        int mid = lo + (hi - lo) / 2;
        if (a[mid] == key) return mid;
        if (a[mid] < key) lo = mid + 1;   /* rebind args instead of calling   */
        else              hi = mid;
    }
    return -1;
}

int main(void)
{
    printf("== flood fill: recursive vs explicit stack ==\n");
    char g1[R][C + 1], g2[R][C + 1];
    for (int r = 0; r < R; ++r) { strcpy(g1[r], grid_init[r]); strcpy(g2[r], grid_init[r]); }
    int starts[][2] = { {0, 0}, {2, 2}, {2, 8}, {7, 10} };
    for (int i = 0; i < 4; ++i) {
        int a = fill_rec(g1, starts[i][0], starts[i][1]);
        int b = fill_iter(g2, starts[i][0], starts[i][1]);
        printf("  fill from (%d,%d): recursive=%d iterative=%d\n",
               starts[i][0], starts[i][1], a, b);
        assert(a == b);
    }
    for (int r = 0; r < R; ++r) { assert(strcmp(g1[r], g2[r]) == 0); printf("  %s\n", g1[r]); }

    printf("\n== Hanoi: recursive vs explicit frames ==\n");
    for (int n = 0; n <= 11; ++n) {
        nrec = nit = 0;
        hanoi_rec(n, 0, 2, 1);
        hanoi_iter(n, 0, 2, 1);
        assert(nrec == nit && nrec == (1 << n) - 1);
        assert(memcmp(moves_rec_log, moves_it_log, sizeof(int) * 2 * (size_t)nrec) == 0);
    }
    printf("  identical move sequences for n = 0..11\n");

    printf("\n== binary search: tail recursion vs loop ==\n");
    int a[100];
    for (int i = 0; i < 100; ++i) a[i] = 2 * i;
    for (int k = -3; k < 205; ++k) {
        int x = bsearch_rec(a, 0, 100, k), y = bsearch_iter(a, 0, 100, k);
        assert(x == y);
        assert(x == ((k >= 0 && k < 200 && k % 2 == 0) ? k / 2 : -1));
    }
    printf("  agree on 208 keys\n");

    printf("\nall recursion->iteration checks passed\n");
    return 0;
}
