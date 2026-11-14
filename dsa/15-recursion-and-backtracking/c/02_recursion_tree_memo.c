/*
 * 02_recursion_tree_memo.c — recursion trees, call counting, memoisation preview,
 *                            Tower of Hanoi
 *
 * Concept : A function that calls itself TWICE builds a binary recursion tree.
 *           Naive Fibonacci recomputes the same subtrees exponentially often;
 *           caching each answer (memoisation) collapses the tree to a line:
 *           O(phi^n) -> O(n). Hanoi is the honest exponential: 2^n - 1 moves,
 *           every one of them needed.
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/02_recursion_tree_memo.c
 * Read    : ../README.md  (sections "Recursion tree", "Memoisation preview")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Recursion tree of fib(5). Every node is one call. Duplicated subtrees are
 * marked: fib(3) is computed 2x, fib(2) 3x, fib(1) 5x.
 *
 *                         fib(5)
 *                /                    \
 *            fib(4)                   fib(3)*
 *          /        \                /      \
 *      fib(3)*      fib(2)*      fib(2)*    fib(1)
 *      /    \       /    \       /    \
 *   fib(2)* fib(1) fib(1) fib(0) fib(1) fib(0)
 *   /    \
 * fib(1) fib(0)
 *
 * calls(n) = 1 + calls(n-1) + calls(n-2)  ->  calls(5) = 15, grows like 1.618^n
 */
static unsigned long long naive_calls;

static uint64_t fib_naive(unsigned n)
{
    ++naive_calls;
    if (n < 2) return n;
    return fib_naive(n - 1) + fib_naive(n - 2);
}

/* Memoised: same recursion, but answer each distinct n only once.
   memo[n] == UINT64_MAX means "not computed yet". */
#define MAXN 94                         /* fib(93) is the last that fits u64 */
static uint64_t memo[MAXN];
static unsigned long long memo_calls;

static uint64_t fib_memo(unsigned n)
{
    ++memo_calls;
    if (n < 2) return n;
    if (memo[n] != UINT64_MAX)          /* cache hit: the whole subtree is pruned */
        return memo[n];
    memo[n] = fib_memo(n - 1) + fib_memo(n - 2);
    return memo[n];
}
/*
 * With the memo the tree collapses: fib(5) -> fib(4) -> fib(3) -> fib(2) -> ...
 * and each right child is a cache hit. ~2n calls total.
 */

static uint64_t fib_iter(unsigned n)    /* bottom-up, O(1) space — chapter 17 */
{
    uint64_t a = 0, b = 1;
    for (unsigned i = 0; i < n; ++i) { uint64_t t = a + b; a = b; b = t; }
    return a;
}

/* ------------------------------------------------------------------------- */
/* Tower of Hanoi                                                             */
/* ------------------------------------------------------------------------- */
/*
 * Move n disks from A to C using B:
 *   1. move n-1 disks A -> B  (using C)      <- trust the recursion!
 *   2. move the biggest disk A -> C
 *   3. move n-1 disks B -> C  (using A)
 * moves(n) = 2*moves(n-1) + 1 = 2^n - 1.
 *
 * The "leap of faith": you do NOT trace the inner calls in your head. You
 * assume hanoi(n-1) works (induction hypothesis) and only check that
 * (a) the base case is right and (b) the step combines it correctly.
 */
static int peg[3][16], top[3];          /* simulated pegs to verify legality */
static unsigned long hanoi_moves;

static void move_disk(int from, int to)
{
    assert(top[from] > 0);
    int d = peg[from][--top[from]];
    assert(top[to] == 0 || peg[to][top[to] - 1] > d);  /* never big on small */
    peg[to][top[to]++] = d;
    ++hanoi_moves;
}

static void hanoi(int n, int from, int to, int via, int print)
{
    if (n == 0) return;                 /* base case: nothing to move */
    hanoi(n - 1, from, via, to, print);
    move_disk(from, to);
    if (print) printf("  move disk %d: %c -> %c\n", n, 'A' + from, 'A' + to);
    hanoi(n - 1, via, to, from, print);
}

static void hanoi_run(int n, int print)
{
    memset(top, 0, sizeof top);
    for (int d = n; d >= 1; --d) peg[0][top[0]++] = d;   /* biggest at bottom */
    hanoi_moves = 0;
    hanoi(n, 0, 2, 1, print);
    assert(top[2] == n && top[0] == 0 && top[1] == 0);
    assert(hanoi_moves == (1ul << n) - 1);
}

int main(void)
{
    printf("== naive vs memoised Fibonacci ==\n");
    printf("  n   fib(n)        naive calls   memo calls\n");
    for (unsigned n = 0; n <= 30; n += 5) {
        naive_calls = 0;
        uint64_t a = fib_naive(n);
        for (unsigned i = 0; i < MAXN; ++i) memo[i] = UINT64_MAX;
        memo_calls = 0;
        uint64_t b = fib_memo(n);
        assert(a == b && b == fib_iter(n));
        /* closed form for the naive call count: 2*fib(n+1) - 1 */
        assert(naive_calls == 2 * fib_iter(n + 1) - 1);
        assert(memo_calls <= 2ull * n + 1);
        printf("  %-3u %-12llu  %-12llu  %llu\n", n, (unsigned long long)a,
               naive_calls, memo_calls);
    }
    for (unsigned i = 0; i < MAXN; ++i) memo[i] = UINT64_MAX;
    assert(fib_memo(90) == fib_iter(90));
    printf("fib(90) = %llu (memo: instant; naive would need ~10^19 calls)\n",
           (unsigned long long)fib_memo(90));

    printf("\n== Tower of Hanoi, 3 disks ==\n");
    hanoi_run(3, 1);
    printf("  %lu moves (2^3 - 1)\n", hanoi_moves);
    for (int n = 1; n <= 15; ++n) hanoi_run(n, 0);
    printf("verified legal and optimal (2^n - 1 moves) for n = 1..15\n");

    printf("\nall recursion-tree / memo checks passed\n");
    return 0;
}
