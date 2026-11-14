/*
 * 05_gas_station.c — gas station: find the start of a circular route
 *
 * Concept : If total gas >= total cost a solution exists. Run a tank from the
 *           candidate start; the moment it goes negative at station i, no start
 *           in [candidate..i] can work (they all arrive at i with even less),
 *           so jump the candidate to i+1. One pass, O(n).
 *           Checked against the O(n^2) "try every start" simulation.
 * Build   : make run P=dsa/16-greedy/c/05_gas_station.c
 * Read    : ../README.md  (section "Gas station")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

/*
 *   gas  = [1, 2, 3, 4, 5]
 *   cost = [3, 4, 5, 1, 2]
 *   diff = [-2,-2,-2, 3, 3]   total = 0 >= 0 -> some start works
 *
 *   i  diff tank(after)  action
 *   0   -2     -2        negative: start = 1, tank = 0
 *   1   -2     -2        negative: start = 2, tank = 0
 *   2   -2     -2        negative: start = 3, tank = 0
 *   3    3      3
 *   4    3      6        end: answer = 3
 *
 * Why skipping is safe: starting at s, the tank was >= 0 when arriving at every
 * station s+1..i. Starting at some k in (s, i] throws away that non-negative
 * prefix, so the tank at i can only be lower — still negative.
 */
static int can_complete_circuit(const int *gas, const int *cost, int n)
{
    long total = 0, tank = 0;
    int start = 0;
    for (int i = 0; i < n; i++) {
        int d = gas[i] - cost[i];
        total += d;
        tank += d;
        if (tank < 0) { start = i + 1; tank = 0; }
    }
    return total >= 0 ? start : -1;
}

/* O(n^2): simulate the full loop from every start; return the smallest valid start. */
static int brute(const int *gas, const int *cost, int n)
{
    for (int s = 0; s < n; s++) {
        long tank = 0;
        int ok = 1;
        for (int k = 0; k < n; k++) {
            int i = (s + k) % n;
            tank += gas[i] - cost[i];
            if (tank < 0) { ok = 0; break; }
        }
        if (ok) return s;
    }
    return -1;
}

static int valid_start(const int *gas, const int *cost, int n, int s)
{
    long tank = 0;
    for (int k = 0; k < n; k++) {
        int i = (s + k) % n;
        tank += gas[i] - cost[i];
        if (tank < 0) return 0;
    }
    return 1;
}

int main(void)
{
    int gas[] = {1, 2, 3, 4, 5}, cost[] = {3, 4, 5, 1, 2};
    int s = can_complete_circuit(gas, cost, 5);
    printf("start station = %d\n", s);
    assert(s == 3);

    int g2[] = {2, 3, 4}, c2[] = {3, 4, 3};
    assert(can_complete_circuit(g2, c2, 3) == -1);
    printf("gas=[2,3,4] cost=[3,4,3] -> -1 (impossible)\n");

    srand(1605);
    for (int t = 0; t < 20000; t++) {
        int n = 1 + rand() % 12, g[16], c[16];
        for (int i = 0; i < n; i++) { g[i] = rand() % 6; c[i] = rand() % 6; }
        int ref = brute(g, c, n), got = can_complete_circuit(g, c, n);
        /* the greedy returns a valid start whenever one exists (it may not be the
         * smallest index when several work, so check validity, not equality) */
        assert((ref == -1) == (got == -1));
        if (got != -1) assert(valid_start(g, c, n, got));
    }
    printf("20000 random circuits: greedy agrees with brute force. OK\n");
    return 0;
}
