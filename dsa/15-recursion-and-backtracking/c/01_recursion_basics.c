/*
 * 01_recursion_basics.c — how recursion uses the call stack: frames, base case,
 *                         recursive case, tail recursion
 *
 * Concept : Every call gets its own stack frame (arguments, locals, return
 *           address). Recursion = a function that solves a smaller copy of
 *           its own problem. We print real frame addresses to SEE the stack
 *           grow downwards, then show the tail-recursive form and its loop.
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/01_recursion_basics.c
 * Read    : ../README.md  (sections "How recursion uses the stack", "Tail recursion")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* 1. factorial — the "hello world" of recursion                              */
/* ------------------------------------------------------------------------- */
/*
 * fact(4) unwinds like this (stack grows DOWN in memory on x86-64/arm64):
 *
 *   call phase (pushing)            return phase (popping)
 *   fact(4) = 4 * fact(3)  ...       4 * 6  = 24   <- final answer
 *     fact(3) = 3 * fact(2)          3 * 2  = 6
 *       fact(2) = 2 * fact(1)        2 * 1  = 2
 *         fact(1) = 1   BASE CASE    returns 1
 *
 * The multiplication happens AFTER the inner call returns, so every frame
 * must stay alive until then: O(n) stack space.
 */
static uint64_t fact(unsigned n)
{
    if (n <= 1)               /* base case: stops the descent. Forget it ->   */
        return 1;             /* infinite recursion -> stack overflow (SIGSEGV) */
    return n * fact(n - 1);   /* recursive case: problem shrinks by 1 each call */
}

/* ------------------------------------------------------------------------- */
/* 2. Watching the frames                                                     */
/* ------------------------------------------------------------------------- */
static uintptr_t frame_addr[16];   /* address of a local in each frame */

static unsigned depth_probe(unsigned depth, unsigned max)
{
    volatile int local = (int)depth;     /* volatile: force it into memory */
    /* __builtin_frame_address(0) = this call's frame pointer. We use it rather
       than &local because AddressSanitizer may move locals to a heap "fake
       stack" (to catch use-after-return), hiding the real layout. */
    frame_addr[depth] = (uintptr_t)__builtin_frame_address(0);
    if (depth == max)
        return depth;
    return depth_probe(depth + 1, max) + 0u * (unsigned)local;
    /* "+ 0*local" keeps local live after the call, so the compiler cannot
       turn this into a tail call and reuse the frame (even at -O2). */
}

/* ------------------------------------------------------------------------- */
/* 3. Tail recursion                                                          */
/* ------------------------------------------------------------------------- */
/*
 * A call is a TAIL call when it is the very last thing the function does:
 * nothing is left to compute after it returns. Then the current frame is
 * useless and a compiler MAY reuse it (gcc -O2 does; -O0 does not; C does not
 * guarantee it). The trick: carry the partial result in an accumulator.
 *
 *   fact_tail(4, 1) -> fact_tail(3, 4) -> fact_tail(2, 12) -> fact_tail(1, 24) -> 24
 */
static uint64_t fact_tail(unsigned n, uint64_t acc)
{
    if (n <= 1)
        return acc;
    return fact_tail(n - 1, acc * n);    /* tail call: nothing pending */
}

/* The mechanical loop version of the tail call: parameters become variables,
   the call becomes "update the variables and jump to the top". */
static uint64_t fact_loop(unsigned n)
{
    uint64_t acc = 1;
    while (n > 1) {          /* the base-case test becomes the loop condition */
        acc *= n;            /* acc = acc * n         (new 2nd argument)      */
        n -= 1;              /* n   = n - 1           (new 1st argument)      */
    }
    return acc;
}

/* ------------------------------------------------------------------------- */
/* 4. More shapes of recursion                                                */
/* ------------------------------------------------------------------------- */
/* Linear recursion on an array: sum(a, n) = a[n-1] + sum(a, n-1). */
static long sum_array(const int *a, size_t n)
{
    if (n == 0)
        return 0;
    return a[n - 1] + sum_array(a, n - 1);
}

/* Divide in half: depth O(log n) instead of O(n) -> much safer for big n. */
static long sum_halves(const int *a, size_t lo, size_t hi)   /* [lo, hi) */
{
    if (hi - lo == 0) return 0;
    if (hi - lo == 1) return a[lo];
    size_t mid = lo + (hi - lo) / 2;
    return sum_halves(a, lo, mid) + sum_halves(a, mid, hi);
}

/* Fast power: x^n = (x^(n/2))^2 (* x if n odd). O(log n) calls. */
static uint64_t power(uint64_t x, unsigned n)
{
    if (n == 0) return 1;
    uint64_t half = power(x, n / 2);     /* compute ONCE, not twice! calling  */
    uint64_t r = half * half;            /* power() twice makes it O(n) again */
    return (n & 1u) ? r * x : r;
}

/* Recursion on strings: palindrome check by peeling both ends. */
static int is_pal(const char *s, size_t lo, size_t hi)        /* [lo, hi] */
{
    if (lo >= hi) return 1;              /* 0 or 1 chars left */
    if (s[lo] != s[hi]) return 0;
    return is_pal(s, lo + 1, hi - 1);
}

/* Reverse a string in place, recursively (swap ends, recurse inward). */
static void reverse_rec(char *s, size_t lo, size_t hi)
{
    if (lo >= hi) return;
    char t = s[lo]; s[lo] = s[hi]; s[hi] = t;
    reverse_rec(s, lo + 1, hi - 1);
}

/* gcd: Euclid, naturally tail recursive. */
static unsigned gcd(unsigned a, unsigned b) { return b == 0 ? a : gcd(b, a % b); }

int main(void)
{
    printf("== factorial ==\n");
    uint64_t expect = 1;
    for (unsigned n = 0; n <= 20; ++n) {      /* 21! overflows uint64_t */
        if (n > 1) expect *= n;
        assert(fact(n) == expect);
        assert(fact_tail(n, 1) == expect);
        assert(fact_loop(n) == expect);
    }
    printf("fact(10) = %llu, fact(20) = %llu\n",
           (unsigned long long)fact(10), (unsigned long long)fact(20));

    printf("\n== stack frames of depth_probe(0..5) ==\n");
    depth_probe(0, 5);
    for (unsigned d = 0; d <= 5; ++d) {
        long diff = d ? (long)(frame_addr[d - 1] - frame_addr[d]) : 0;
        printf("  depth %u: frame = %#lx  (%ld bytes below caller)\n",
               d, (unsigned long)frame_addr[d], diff);
    }
    /* Each deeper frame lives at a LOWER address: the stack grows down. */
    for (unsigned d = 1; d <= 5; ++d)
        assert(frame_addr[d] < frame_addr[d - 1]);
    printf("  -> addresses decrease: each call pushes a new frame below its caller\n");

    printf("\n== shapes ==\n");
    int a[1000];
    long ref = 0;
    for (int i = 0; i < 1000; ++i) { a[i] = i * 3 - 700; ref += a[i]; }
    assert(sum_array(a, 1000) == ref);
    assert(sum_halves(a, 0, 1000) == ref);
    printf("sum of 1000 ints = %ld (linear depth 1000, halving depth ~10)\n", ref);

    for (unsigned n = 0; n < 40; ++n) {
        uint64_t p = 1;
        for (unsigned i = 0; i < n; ++i) p *= 3;
        assert(power(3, n) == p);
    }
    printf("power(2, 40) = %llu\n", (unsigned long long)power(2, 40));

    const char *pals[] = { "", "a", "abba", "racecar", "abca" };
    int pal_expect[]   = { 1,  1,   1,      1,         0      };
    for (int i = 0; i < 5; ++i) {
        size_t len = strlen(pals[i]);
        int r = len ? is_pal(pals[i], 0, len - 1) : 1;
        assert(r == pal_expect[i]);
        printf("is_pal(\"%s\") = %d\n", pals[i], r);
    }
    char s[] = "recursion";
    reverse_rec(s, 0, strlen(s) - 1);
    assert(strcmp(s, "noisrucer") == 0);
    printf("reverse(\"recursion\") = %s\n", s);

    assert(gcd(48, 18) == 6 && gcd(17, 5) == 1 && gcd(0, 9) == 9);
    printf("gcd(48,18) = %u\n", gcd(48, 18));

    printf("\nall recursion-basics checks passed\n");
    return 0;
}
