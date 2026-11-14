/*
 * 04_jump_game.c — jump game I (can you reach the end?) and II (fewest jumps)
 *
 * Concept : Track the FARTHEST index reachable so far. Jump game I is one pass:
 *           if you ever stand beyond `farthest`, you are stuck. Jump game II is a
 *           BFS done implicitly: the indices reachable with k jumps form one
 *           contiguous window, so count windows instead of using a queue.
 *           Both checked against an O(n^2) DP on random arrays.
 * Build   : make run P=dsa/16-greedy/c/04_jump_game.c
 * Read    : ../README.md  (section "Jump game")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * a[i] = maximum jump length from i.  O(n) time, O(1) space.
 *
 *   a = [2, 3, 1, 1, 4]
 *   i=0 far=max(0,0+2)=2
 *   i=1 far=max(2,1+3)=4   >= last index 4 -> reachable
 *
 *   a = [3, 2, 1, 0, 4]
 *   i=0 far=3, i=1 far=3, i=2 far=3, i=3 far=3, i=4 > far -> STUCK at the 0
 */
static int can_reach_end(const int *a, int n)
{
    int far = 0;
    for (int i = 0; i < n; i++) {
        if (i > far) return 0;               /* this index can't be reached at all */
        if (i + a[i] > far) far = i + a[i];
        if (far >= n - 1) return 1;
    }
    return 1;
}

/*
 * Minimum jumps, O(n). Returns -1 if unreachable.
 *
 *   a = [2, 3, 1, 1, 4]
 *   index:    0 | 1  2 | 3  4
 *   jumps:    0 |  1   |  2         <- each "level" is a window [lo, hi]
 *
 *   window0 = [0,0]  farthest from it = 2      -> window1 = [1,2]
 *   window1 = [1,2]  farthest from it = 1+3=4  -> window2 = [3,4] contains 4 -> 2 jumps
 */
static int min_jumps(const int *a, int n)
{
    if (n <= 1) return 0;
    int jumps = 0, cur_end = 0, far = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i + a[i] > far) far = i + a[i];
        if (i == cur_end) {          /* finished scanning the current window */
            if (far <= i) return -1; /* the next window would be empty */
            jumps++;
            cur_end = far;
            if (cur_end >= n - 1) break;
        }
    }
    return jumps;
}

/* Reference: dp[i] = fewest jumps to reach i, O(n^2). */
static int dp_min_jumps(const int *a, int n)
{
    int dp[64];
    const int INF = 1 << 29;
    dp[0] = 0;
    for (int i = 1; i < n; i++) dp[i] = INF;
    for (int i = 0; i < n; i++) {
        if (dp[i] == INF) continue;
        for (int j = i + 1; j <= i + a[i] && j < n; j++)
            if (dp[i] + 1 < dp[j]) dp[j] = dp[i] + 1;
    }
    return dp[n - 1] == INF ? -1 : dp[n - 1];
}

int main(void)
{
    int a1[] = {2, 3, 1, 1, 4};
    int a2[] = {3, 2, 1, 0, 4};
    int a3[] = {2, 3, 0, 1, 4};
    int a4[] = {0};
    printf("[2,3,1,1,4] reachable=%d min jumps=%d\n", can_reach_end(a1, 5), min_jumps(a1, 5));
    printf("[3,2,1,0,4] reachable=%d min jumps=%d\n", can_reach_end(a2, 5), min_jumps(a2, 5));
    assert(can_reach_end(a1, 5) && min_jumps(a1, 5) == 2);
    assert(!can_reach_end(a2, 5) && min_jumps(a2, 5) == -1);
    assert(min_jumps(a3, 5) == 2);
    assert(can_reach_end(a4, 1) && min_jumps(a4, 1) == 0);

    srand(1604);
    for (int t = 0; t < 20000; t++) {
        int n = 1 + rand() % 40, a[64];
        for (int i = 0; i < n; i++) a[i] = rand() % 4; /* zeros make traps likely */
        int ref = dp_min_jumps(a, n);
        assert(min_jumps(a, n) == ref);
        assert(can_reach_end(a, n) == (ref != -1));
    }
    printf("20000 random arrays: greedy == DP. OK\n");
    return 0;
}
