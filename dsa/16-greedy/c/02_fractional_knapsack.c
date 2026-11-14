/*
 * 02_fractional_knapsack.c — fractional knapsack: take by value/weight ratio
 *
 * Concept : When items can be CUT, the best kilogram to put in the bag is always
 *           the most valuable-per-kilogram one, so sort by ratio and pour.
 *           When items can NOT be cut (0/1 knapsack) the same greedy is wrong —
 *           shown here against an exhaustive 0/1 search.
 * Build   : make run P=dsa/16-greedy/c/02_fractional_knapsack.c
 * Read    : ../README.md  (section "Fractional knapsack")
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    double value;
    double weight; /* > 0 */
} Item;

/* Sort by value/weight DESCENDING. Compare v1*w2 vs v2*w1 to avoid two divisions. */
static int by_ratio_desc(const void *a, const void *b)
{
    const Item *x = a, *y = b;
    double lhs = x->value * y->weight, rhs = y->value * x->weight;
    return (lhs < rhs) - (lhs > rhs);
}

/*
 * O(n log n): sort, then one pass.
 *
 *   capacity 50
 *   item  value weight ratio
 *   A       60    10    6.0   take all   -> cap 40, value  60
 *   B      100    20    5.0   take all   -> cap 20, value 160
 *   C      120    30    4.0   take 20/30 -> cap  0, value 240
 */
static double fractional_knapsack(Item *items, int n, double capacity)
{
    qsort(items, (size_t)n, sizeof items[0], by_ratio_desc);
    double total = 0.0;
    for (int i = 0; i < n && capacity > 0; i++) {
        if (items[i].weight <= capacity) {      /* whole item fits */
            total += items[i].value;
            capacity -= items[i].weight;
        } else {                                /* take the fraction that fits, bag is full */
            total += items[i].value * (capacity / items[i].weight);
            capacity = 0;
        }
    }
    return total;
}

/* Exhaustive 0/1 knapsack, O(2^n): the right answer when items cannot be split. */
static double zero_one_brute(const Item *items, int n, double capacity)
{
    double best = 0;
    for (unsigned mask = 0; mask < (1u << n); mask++) {
        double w = 0, v = 0;
        for (int i = 0; i < n; i++)
            if (mask & (1u << i)) { w += items[i].weight; v += items[i].value; }
        if (w <= capacity && v > best) best = v;
    }
    return best;
}

/* Greedy-by-ratio applied to 0/1 (no splitting) — the WRONG algorithm, for contrast. */
static double zero_one_greedy(Item *items, int n, double capacity)
{
    qsort(items, (size_t)n, sizeof items[0], by_ratio_desc);
    double total = 0;
    for (int i = 0; i < n; i++)
        if (items[i].weight <= capacity) { total += items[i].value; capacity -= items[i].weight; }
    return total;
}

/*
 * Independent re-implementation used as the reference: a different sort
 * (insertion sort, real division) and a "take min(weight, capacity)" loop.
 * main() also checks the fractional answer is never below the 0/1 optimum,
 * because allowing cuts can only help (it is a relaxation).
 */
static double fractional_by_insertion(const Item *src, int n, double capacity)
{
    Item a[16];
    for (int i = 0; i < n; i++) a[i] = src[i];
    for (int i = 1; i < n; i++) { /* insertion sort, ratio desc */
        Item key = a[i];
        int j = i - 1;
        while (j >= 0 && a[j].value / a[j].weight < key.value / key.weight) { a[j + 1] = a[j]; j--; }
        a[j + 1] = key;
    }
    double total = 0;
    for (int i = 0; i < n; i++) {
        double take = a[i].weight < capacity ? a[i].weight : capacity;
        total += a[i].value / a[i].weight * take;
        capacity -= take;
    }
    return total;
}

int main(void)
{
    Item items[] = {{60, 10}, {100, 20}, {120, 30}};
    Item copy[3] = {items[0], items[1], items[2]};
    double frac = fractional_knapsack(items, 3, 50);
    printf("fractional knapsack, cap 50: %.1f\n", frac);
    assert(fabs(frac - 240.0) < 1e-9);

    /* Same items, 0/1: greedy by ratio takes A+B = 160, but B+C = 220 is optimal. */
    double g01 = zero_one_greedy(copy, 3, 50);
    double b01 = zero_one_brute(copy, 3, 50);
    printf("0/1 greedy-by-ratio: %.1f   0/1 optimum: %.1f   -> greedy FAILS for 0/1\n", g01, b01);
    assert(g01 == 160 && b01 == 220);

    srand(1602);
    for (int t = 0; t < 3000; t++) {
        int n = 1 + rand() % 10;
        Item it[16];
        for (int i = 0; i < n; i++) {
            it[i].value = 1 + rand() % 100;
            it[i].weight = 1 + rand() % 30;
        }
        double cap = rand() % 80;
        double ref = fractional_by_insertion(it, n, cap);
        double z = zero_one_brute(it, n, cap);
        double got = fractional_knapsack(it, n, cap);
        assert(fabs(got - ref) < 1e-6);
        assert(got + 1e-9 >= z); /* relaxation is never worse */
    }
    printf("3000 random instances: OK\n");
    return 0;
}
