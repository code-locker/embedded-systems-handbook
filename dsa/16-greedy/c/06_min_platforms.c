/*
 * 06_min_platforms.c — minimum railway platforms (= max overlapping intervals)
 *
 * Concept : Sort arrivals and departures SEPARATELY, then sweep time with two
 *           pointers: arrival -> +1 platform, departure -> -1. The peak is the
 *           answer. Same problem as "meeting rooms II", "max concurrent
 *           connections", "how many DMA buffers are in flight at once".
 *           Checked against counting overlaps at every time instant.
 * Build   : make run P=dsa/16-greedy/c/06_min_platforms.c
 * Read    : ../README.md  (section "Minimum platforms")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/*
 * A train occupies its platform during the CLOSED interval [arr, dep]:
 * if one train departs at 10 and another arrives at 10, they need 2 platforms.
 * That is why we process the arrival first when times are equal (arr <= dep).
 *
 *   arr: 900  940  950  1100 1500 1800
 *   dep: 910 1120 1130 1200 1900 2000
 *
 *   time  event  in use  peak
 *   900   arr      1      1
 *   910   dep      0      1
 *   940   arr      1      1
 *   950   arr      2      2
 *   1100  arr      3      3   <- three trains at once
 *   1120  dep      2      3
 *   ...
 *
 * O(n log n) for the sorts, O(n) sweep. Sorts the caller's arrays in place.
 */
static int min_platforms(int *arr, int *dep, int n)
{
    qsort(arr, (size_t)n, sizeof *arr, cmp_int);
    qsort(dep, (size_t)n, sizeof *dep, cmp_int);
    int i = 0, j = 0, in_use = 0, peak = 0;
    while (i < n) {
        if (arr[i] <= dep[j]) {     /* next event is an arrival (ties: arrival first) */
            in_use++;
            i++;
            if (in_use > peak) peak = in_use;
        } else {                    /* next event is a departure */
            in_use--;
            j++;
        }
    }
    /* Once all arrivals are processed the count only falls, so we can stop. */
    return peak;
}

/* Reference: for every arrival instant, count trains present then. O(n^2).
 * The maximum overlap of closed intervals is always attained at some start. */
static int brute(const int *arr, const int *dep, int n)
{
    int best = 0;
    for (int k = 0; k < n; k++) {
        int c = 0;
        for (int i = 0; i < n; i++)
            if (arr[i] <= arr[k] && arr[k] <= dep[i]) c++;
        if (c > best) best = c;
    }
    return best;
}

int main(void)
{
    int arr[] = {900, 940, 950, 1100, 1500, 1800};
    int dep[] = {910, 1120, 1130, 1200, 1900, 2000};
    int p = min_platforms(arr, dep, 6);
    printf("platforms needed = %d\n", p);
    assert(p == 3);

    int a2[] = {10, 10}, d2[] = {10, 20};
    assert(min_platforms(a2, d2, 2) == 2); /* arrive at the same instant -> 2 */

    srand(1606);
    for (int t = 0; t < 5000; t++) {
        int n = 1 + rand() % 30, a[32], d[32], ca[32], cd[32];
        for (int i = 0; i < n; i++) {
            a[i] = rand() % 50;
            d[i] = a[i] + rand() % 15;
            ca[i] = a[i];
            cd[i] = d[i];
        }
        int ref = brute(a, d, n);
        assert(min_platforms(ca, cd, n) == ref);
    }
    printf("5000 random timetables: sweep == brute force. OK\n");
    return 0;
}
