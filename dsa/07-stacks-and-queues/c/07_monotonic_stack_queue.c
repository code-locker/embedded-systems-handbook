/*
 * 07_monotonic_stack_queue.c — monotonic stack (next greater element) and
 *                              monotonic deque (sliding window maximum)
 *
 * Concept : Keep the stack/deque SORTED by throwing away elements that can
 *           never be the answer again. Every element is pushed once and
 *           popped at most once, so a loop that looks O(n*k) is really O(n).
 *           Both are compared against the obvious brute force.
 * Build   : make run P=dsa/07-stacks-and-queues/c/07_monotonic_stack_queue.c
 * Read    : ../README.md  (section "Monotonic stack and queue")
 */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------------ */
/* 1. Next greater element                                                  */
/* ------------------------------------------------------------------------ */
/*
 * For each a[i], find the first element to its RIGHT that is larger
 * (-1 if none).
 *
 * Idea: walk left to right; the stack holds INDICES of elements still
 * "waiting" for their answer. Their values are decreasing from bottom to
 * top (a monotonic decreasing stack). When a bigger value x arrives, it is
 * the answer for every waiting element smaller than x — pop them all.
 *
 *   a = [2, 1, 5, 3, 4]
 *   i=0 x=2: stack []        push 0          -> [2]
 *   i=1 x=1: 1 < 2           push 1          -> [2 1]
 *   i=2 x=5: pop 1 (ans 5), pop 2 (ans 5)    -> [5]
 *   i=3 x=3: push                            -> [5 3]
 *   i=4 x=4: pop 3 (ans 4), push             -> [5 4]
 *   end    : 5 and 4 keep answer -1
 *   result = [5, 5, -1, 4, -1]
 *
 * O(n) time: each index is pushed once and popped at most once (2n ops),
 * even though there is a while loop inside the for loop. O(n) space.
 * Real use: stock span, histogram's largest rectangle, "daily temperatures".
 */
static void next_greater(const int *a, int n, int *ans, int *stack)
{
    int top = 0;
    for (int i = 0; i < n; i++) {
        ans[i] = -1;
        while (top > 0 && a[stack[top - 1]] < a[i])
            ans[stack[--top]] = a[i];
        stack[top++] = i;
    }
}

static void next_greater_brute(const int *a, int n, int *ans) /* O(n^2) */
{
    for (int i = 0; i < n; i++) {
        ans[i] = -1;
        for (int j = i + 1; j < n; j++)
            if (a[j] > a[i]) {
                ans[i] = a[j];
                break;
            }
    }
}

/* ------------------------------------------------------------------------ */
/* 2. Sliding window maximum with a monotonic deque                         */
/* ------------------------------------------------------------------------ */
/*
 * For each window of size k, output its maximum.
 *
 * The deque holds INDICES whose values are DECREASING front to back.
 *   - front = index of the max of the current window
 *   - before pushing i, pop from the BACK every index whose value <= a[i]:
 *     they are older AND not bigger, so they can never be a max again
 *   - pop from the FRONT if that index has slid out of the window
 *
 *   a = [1, 3, -1, -3, 5, 3, 6, 7], k = 3
 *
 *   i  a[i]  deque(values)   window max
 *   0   1    [1]
 *   1   3    [3]             (1 popped: 3 beats it)
 *   2  -1    [3 -1]          3
 *   3  -3    [3 -1 -3]       3
 *   4   5    [5]             5   (all popped from back)
 *   5   3    [5 3]           5
 *   6   6    [6]             6
 *   7   7    [7]             7
 *
 * O(n) total (each index enters/leaves once), vs O(n*k) brute force.
 * Real use: peak detection in a signal / audio level meter, rate limiting,
 * "max latency in the last k packets".
 *
 * The deque is a plain array with head/tail indices: since every index is
 * pushed at most once, an array of n slots never overflows and never needs
 * to wrap.
 */
static int sliding_max(const int *a, int n, int k, int *out, int *dq)
{
    int head = 0, tail = 0, m = 0;
    for (int i = 0; i < n; i++) {
        if (head < tail && dq[head] <= i - k)
            head++; /* front left the window */
        while (head < tail && a[dq[tail - 1]] <= a[i])
            tail--; /* dominated: older and not bigger */
        dq[tail++] = i;
        if (i >= k - 1)
            out[m++] = a[dq[head]];
    }
    return m;
}

static int sliding_max_brute(const int *a, int n, int k, int *out)
{
    int m = 0;
    for (int i = 0; i + k <= n; i++) {
        int best = a[i];
        for (int j = i + 1; j < i + k; j++)
            if (a[j] > best)
                best = a[j];
        out[m++] = best;
    }
    return m;
}

int main(void)
{
    int a[] = {2, 1, 5, 3, 4};
    int n = 5, ans[5], st[5];
    next_greater(a, n, ans, st);
    printf("next greater of [2 1 5 3 4] = [");
    for (int i = 0; i < n; i++)
        printf(i ? " %d" : "%d", ans[i]);
    printf("]\n");
    int want[] = {5, 5, -1, 4, -1};
    for (int i = 0; i < n; i++)
        CHECK(ans[i] == want[i]);

    int b[] = {1, 3, -1, -3, 5, 3, 6, 7};
    int out[8], dq[8];
    int m = sliding_max(b, 8, 3, out, dq);
    printf("sliding max k=3 of [1 3 -1 -3 5 3 6 7] = [");
    for (int i = 0; i < m; i++)
        printf(i ? " %d" : "%d", out[i]);
    printf("]\n");
    int wantm[] = {3, 3, 5, 5, 6, 7};
    CHECK(m == 6);
    for (int i = 0; i < m; i++)
        CHECK(out[i] == wantm[i]);

    /* Randomised cross-check against brute force */
    enum { R = 300 };
    int *big = malloc(R * sizeof *big), *bans = malloc(R * sizeof *bans),
        *bst = malloc(R * sizeof *bst), *bref = malloc(R * sizeof *bref);
    CHECK(big && bans && bst && bref);
    unsigned seed = 7;
    for (int trial = 0; trial < 50; trial++) {
        for (int i = 0; i < R; i++) {
            seed = seed * 1664525u + 1013904223u;
            big[i] = (int)(seed >> 24) % 50; /* many duplicates on purpose */
        }
        next_greater(big, R, bans, bst);
        next_greater_brute(big, R, bref);
        for (int i = 0; i < R; i++)
            CHECK(bans[i] == bref[i]);
        int k = 1 + trial % 17;
        int m1 = sliding_max(big, R, k, bans, bst);
        int m2 = sliding_max_brute(big, R, k, bref);
        CHECK(m1 == m2);
        for (int i = 0; i < m1; i++)
            CHECK(bans[i] == bref[i]);
    }
    printf("50 random arrays: monotonic == brute force\n");
    free(big);
    free(bans);
    free(bst);
    free(bref);

    printf("07_monotonic_stack_queue: all checks passed\n");
    return 0;
}
