/*
 * 08_parentheses_and_pruning.c — generate balanced parentheses; measure what
 *                                pruning buys you (combination sum)
 *
 * Concept : Pruning = refusing to enter a subtree that cannot contain a valid
 *           answer. Generate-parentheses never builds an invalid prefix (it
 *           only adds ')' when open > close), so it visits ~Catalan(n) leaves
 *           instead of 2^(2n). Combination-sum with SORTED candidates can
 *           `break` (not just `continue`) as soon as a candidate exceeds the
 *           remaining target. We count nodes with and without pruning and
 *           check both give identical answers, and that answers match brute
 *           force.
 * Build   : make run P=dsa/15-recursion-and-backtracking/c/08_parentheses_and_pruning.c
 * Read    : ../README.md  (sections "Generate parentheses", "Pruning")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Generate parentheses                                                       */
/* ------------------------------------------------------------------------- */
/*
 * Tree for n = 2 (o = open count, c = close count). Rules:
 *   add '(' if o < n          add ')' if c < o     <- the pruning rules
 *
 *                     ""
 *                     |  (            ')' illegal: c(0) < o(0) false
 *                    "("
 *              (  /        \  )
 *             "(("          "()"
 *               | )           | (
 *            "(()"          "()("
 *               | )           | )
 *            "(())"         "()()"      <- 2 = Catalan(2) leaves, no dead ends
 */
#define MAXP 10
static char buf[2 * MAXP + 1];
static char (*out)[2 * MAXP + 1];        /* heap array of results (owned by main) */
static long nout, gen_nodes;

static void gen(int n, int open, int close, int len)
{
    ++gen_nodes;
    if (len == 2 * n) { buf[len] = '\0'; memcpy(out[nout++], buf, (size_t)len + 1); return; }
    if (open < n)     { buf[len] = '('; gen(n, open + 1, close, len + 1); }
    if (close < open) { buf[len] = ')'; gen(n, open, close + 1, len + 1); }
    /* Un-choose is implicit: buf[len] is simply overwritten by the next try. */
}

static int balanced(const char *s)
{
    int depth = 0;
    for (; *s; ++s) {
        depth += *s == '(' ? 1 : -1;
        if (depth < 0) return 0;
    }
    return depth == 0;
}

static long catalan(int n)            /* C(n) = C(2n,n)/(n+1) */
{
    long c = 1;
    for (int i = 0; i < n; ++i) c = c * 2 * (2 * i + 1) / (i + 2);
    return c;
}

static int cmp_str(const void *a, const void *b) { return strcmp(a, b); }

/* ------------------------------------------------------------------------- */
/* Combination sum: numbers (reusable) summing to target                      */
/* ------------------------------------------------------------------------- */
static int cand[] = { 2, 3, 5, 7, 8, 11 };   /* must be SORTED for the break */
#define NC ((int)(sizeof cand / sizeof cand[0]))
static long found_cnt, nodes;
static unsigned long checksum;               /* order-sensitive hash of results */
static int chosen[64], nch;

static void record(void)
{
    unsigned long h = 1469598103u;
    for (int i = 0; i < nch; ++i) h = h * 1099511u + (unsigned long)chosen[i];
    checksum += h;                           /* sum => independent of order  */
    ++found_cnt;
}

/* Without pruning: explore until remaining < 0 (overshoot detected late). */
static void comb_plain(int start, int remaining)
{
    ++nodes;
    if (remaining < 0) return;
    if (remaining == 0) { record(); return; }
    for (int i = start; i < NC; ++i) {
        chosen[nch++] = cand[i];
        comb_plain(i, remaining - cand[i]);  /* i (not i+1): reuse allowed */
        --nch;
    }
}

/* With pruning: candidates are sorted, so once cand[i] > remaining every later
   one is too. `break` cuts the whole rest of the loop, not one branch. */
static void comb_pruned(int start, int remaining)
{
    ++nodes;
    if (remaining == 0) { record(); return; }
    for (int i = start; i < NC; ++i) {
        if (cand[i] > remaining) break;      /* PRUNE */
        chosen[nch++] = cand[i];
        comb_pruned(i, remaining - cand[i]);
        --nch;
    }
}

/* Brute force reference: count multisets via a different method entirely —
   the classic "number of ways" DP over coins (chapter 17 preview). */
static long ways_dp(int target)
{
    long dp[256] = { 1 };
    for (int i = 0; i < NC; ++i)
        for (int s = cand[i]; s <= target; ++s) dp[s] += dp[s - cand[i]];
    return dp[target];
}

int main(void)
{
    printf("== generate parentheses ==\n");
    out = malloc(sizeof *out * 20000);        /* Catalan(10) = 16796 */
    assert(out);
    for (int n = 0; n <= MAXP; ++n) {
        nout = gen_nodes = 0;
        gen(n, 0, 0, 0);
        assert(nout == catalan(n));
        for (long i = 0; i < nout; ++i) assert(balanced(out[i]));
        qsort(out, (size_t)nout, sizeof *out, cmp_str);
        for (long i = 1; i < nout; ++i) assert(strcmp(out[i - 1], out[i]) != 0);
        if (n == 3) {
            printf("  n=3:");
            for (long i = 0; i < nout; ++i) printf(" %s", out[i]);
            printf("\n");
        }
        /* brute force for small n: test all 2^(2n) strings */
        if (n <= 6) {
            long cnt = 0;
            char s[2 * MAXP + 1];
            for (unsigned m = 0; m < (1u << (2 * n)); ++m) {
                for (int b = 0; b < 2 * n; ++b) s[b] = (m >> b & 1) ? ')' : '(';
                s[2 * n] = '\0';
                cnt += balanced(s);
            }
            assert(cnt == nout);
        }
        printf("  n=%-2d results=%-6ld nodes visited=%-7ld (all 2^(2n) strings = %lu)\n",
               n, nout, gen_nodes, 1ul << (2 * n));
    }
    free(out);

    printf("\n== combination sum: plain vs pruned ==\n");
    printf("  target  answers  plain nodes  pruned nodes\n");
    for (int target = 1; target <= 60; ++target) {
        found_cnt = nodes = 0; checksum = 0; nch = 0;
        comb_plain(0, target);
        long a = found_cnt, na = nodes; unsigned long ha = checksum;
        found_cnt = nodes = 0; checksum = 0; nch = 0;
        comb_pruned(0, target);
        assert(found_cnt == a && checksum == ha);   /* same set of answers */
        assert(nodes <= na);
        assert(a == ways_dp(target));                /* independent count  */
        if (target % 15 == 0)
            printf("  %-6d  %-7ld  %-11ld  %ld\n", target, a, na, nodes);
    }

    printf("\nall parentheses / pruning checks passed\n");
    return 0;
}
