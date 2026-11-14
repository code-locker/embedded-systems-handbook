/*
 * 01_activity_selection.c — activity selection / interval scheduling by earliest finish time
 *
 * Concept : Pick the activity that FINISHES first, throw away everything that
 *           overlaps it, repeat. The exchange argument proves this is optimal.
 *           Checked against a brute force over all 2^n subsets on random inputs.
 * Build   : make run P=dsa/16-greedy/c/01_activity_selection.c
 * Read    : ../README.md  (section "Activity selection")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int start;
    int finish; /* half-open interval [start, finish): touching is allowed */
    int id;
} Activity;

/* qsort comparator: by finish time ascending, ties by start (any tie rule works) */
static int by_finish(const void *a, const void *b)
{
    const Activity *x = a, *y = b;
    if (x->finish != y->finish) return (x->finish > y->finish) - (x->finish < y->finish);
    return (x->start > y->start) - (x->start < y->start);
}

/*
 * Greedy: O(n log n) for the sort + O(n) for the scan.
 * Sorts acts in place (caller's array). Writes chosen ids into out[], returns count.
 *
 *   time  0 1 2 3 4 5 6 7 8 9
 *   A     [===)                 finish 2  <- take (earliest finish)
 *   B       [=====)             finish 4  overlaps A -> skip
 *   C           [===)           finish 5  starts at 3 >= 2 -> take
 *   D             [=====)       finish 7  starts at 4 < 5 -> skip
 *   E                 [===)     finish 8  starts at 6 >= 5 -> take
 */
static int select_activities(Activity *acts, int n, int *out)
{
    qsort(acts, (size_t)n, sizeof acts[0], by_finish);
    int count = 0;
    int last_finish = -2147483647; /* "nothing chosen yet" */
    for (int i = 0; i < n; i++) {
        /* Compatible iff it starts after (or exactly when) the last chosen one ends. */
        if (acts[i].start >= last_finish) {
            out[count++] = acts[i].id;
            last_finish = acts[i].finish;
        }
    }
    return count;
}

/* Brute force: O(2^n * n^2). Only for n <= 12 — this is the "truth" we check against. */
static int brute_force(const Activity *acts, int n)
{
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        int ok = 1, cnt = 0;
        for (int i = 0; i < n && ok; i++) {
            if (!(mask & (1u << i))) continue;
            cnt++;
            for (int j = i + 1; j < n; j++) {
                if (!(mask & (1u << j))) continue;
                /* overlap of half-open intervals */
                if (acts[i].start < acts[j].finish && acts[j].start < acts[i].finish) { ok = 0; break; }
            }
        }
        if (ok && cnt > best) best = cnt;
    }
    return best;
}

/* Why NOT "shortest first" or "earliest start"? Show counter-examples. */
static int by_start(const void *a, const void *b)
{
    const Activity *x = a, *y = b;
    return (x->start > y->start) - (x->start < y->start);
}
static int by_length(const void *a, const void *b)
{
    const Activity *x = a, *y = b;
    int lx = x->finish - x->start, ly = y->finish - y->start;
    return (lx > ly) - (lx < ly);
}
static int greedy_with(Activity *acts, int n, int (*cmp)(const void *, const void *))
{
    qsort(acts, (size_t)n, sizeof acts[0], cmp);
    /* pick in sorted order whatever does not overlap anything already picked */
    int chosen[16], count = 0;
    for (int i = 0; i < n; i++) {
        int ok = 1;
        for (int k = 0; k < count; k++) {
            const Activity *c = &acts[chosen[k]];
            if (acts[i].start < c->finish && c->start < acts[i].finish) { ok = 0; break; }
        }
        if (ok) chosen[count++] = i;
    }
    return count;
}

int main(void)
{
    /* ---- the worked example from the README ---- */
    Activity demo[] = {
        {0, 2, 'A'}, {1, 4, 'B'}, {3, 5, 'C'}, {4, 7, 'D'}, {6, 8, 'E'}, {2, 9, 'F'},
    };
    int n = (int)(sizeof demo / sizeof demo[0]);
    int out[16];
    int k = select_activities(demo, n, out);
    printf("Chosen activities:");
    for (int i = 0; i < k; i++) printf(" %c", out[i]);
    printf("  (count = %d)\n", k);
    assert(k == 3 && out[0] == 'A' && out[1] == 'C' && out[2] == 'E');

    /* ---- counter-example: earliest START fails ----
     *   [==========================)   long one starts first
     *     [==)  [==)  [==)             three short ones inside it */
    Activity s[] = {{0, 10, 0}, {1, 3, 1}, {4, 6, 2}, {7, 9, 3}};
    int gs = greedy_with(s, 4, by_start);
    printf("earliest-start greedy picks %d, optimum is 3\n", gs);
    assert(gs == 1);

    /* ---- counter-example: SHORTEST first fails ----
     *   [======)      [======)
     *         [===)               short one overlaps both long ones */
    Activity l[] = {{0, 6, 0}, {5, 8, 1}, {7, 13, 2}};
    int gl = greedy_with(l, 3, by_length);
    printf("shortest-first greedy picks %d, optimum is 2\n", gl);
    assert(gl == 1 && brute_force(l, 3) == 2);

    /* ---- randomised check against brute force ---- */
    srand(16);
    for (int trial = 0; trial < 2000; trial++) {
        int m = 1 + rand() % 12;
        Activity a[12];
        for (int i = 0; i < m; i++) {
            int st = rand() % 20;
            a[i].start = st;
            a[i].finish = st + 1 + rand() % 8;
            a[i].id = i;
        }
        int expect = brute_force(a, m);
        int got = select_activities(a, m, out);
        assert(got == expect);
    }
    printf("2000 random instances: greedy == brute force. OK\n");
    return 0;
}
