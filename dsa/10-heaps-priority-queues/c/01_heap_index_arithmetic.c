/*
 * 01_heap_index_arithmetic.c — a complete binary tree stored in a flat array
 *
 * Concept : A heap is a *complete* binary tree, so it needs no pointers: node i
 *           has its parent at (i-1)/2 and children at 2i+1 and 2i+2 (0-based).
 *           This program prints the tree shape from an array, verifies the
 *           index formulas for every node, and checks the heap property.
 * Build   : make run P=dsa/10-heaps-priority-queues/c/01_heap_index_arithmetic.c
 * Read    : ../README.md  (section "Array representation")
 */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

/* O(1) each: pure arithmetic, this is the whole reason heaps live in arrays.
 *
 *   index:   0   1   2   3   4   5   6
 *   array: [ 1 | 3 | 2 | 7 | 4 | 5 | 9 ]
 *
 *               1(0)
 *             /      \
 *          3(1)       2(2)
 *          /  \       /  \
 *       7(3) 4(4)  5(5) 9(6)
 *
 *   parent(4) = (4-1)/2 = 1   left(1) = 3   right(1) = 4
 */
static size_t parent(size_t i) { return (i - 1) / 2; } /* only valid for i > 0 */
static size_t left(size_t i)   { return 2 * i + 1; }
static size_t right(size_t i)  { return 2 * i + 2; }

/* O(n): every node compared with its parent once. */
static bool is_min_heap(const int *a, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (a[parent(i)] > a[i])
            return false;
    return true;
}

/* O(n): print level by level. Level d holds indices [2^d - 1, 2^(d+1) - 2]. */
static void print_levels(const int *a, size_t n)
{
    size_t first = 0, width = 1;
    int level = 0;
    while (first < n) {
        printf("  level %d:", level);
        for (size_t i = first; i < first + width && i < n; i++)
            printf(" %d", a[i]);
        printf("\n");
        first += width;   /* next level starts right after this one */
        width *= 2;       /* and is twice as wide */
        level++;
    }
}

/* O(log n): height of a complete tree with n nodes = floor(log2 n). */
static int height(size_t n)
{
    int h = -1;
    while (n) { n >>= 1; h++; }
    return h;
}

int main(void)
{
    int heap[] = {1, 3, 2, 7, 4, 5, 9, 8, 10, 6};
    size_t n = sizeof heap / sizeof heap[0];

    printf("array as a tree (n=%zu, height=%d):\n", n, height(n));
    print_levels(heap, n);

    /* Verify the formulas are inverse of each other for every node. */
    for (size_t i = 0; i < n; i++) {
        if (left(i) < n)  assert(parent(left(i)) == i);
        if (right(i) < n) assert(parent(right(i)) == i);
        /* Leaves are exactly the second half: indices n/2 .. n-1.
         * That is why build-heap starts sifting at n/2 - 1. */
        bool is_leaf = left(i) >= n;
        assert(is_leaf == (i >= n / 2));
    }
    printf("parent/child formulas verified for all %zu nodes\n", n);
    printf("leaves are indices %zu..%zu (the second half)\n", n / 2, n - 1);

    assert(is_min_heap(heap, n));
    printf("is_min_heap: yes\n");

    int not_heap[] = {1, 3, 2, 0};  /* 0 is under 3: violates the property */
    assert(!is_min_heap(not_heap, 4));
    printf("is_min_heap({1,3,2,0}): no (0 sits under 3)\n");

    /* 1-based variant used in many textbooks: parent i/2, children 2i, 2i+1.
     * Same tree, shifted by one; slot 0 is wasted but the maths is shorter. */
    for (size_t i = 1; i <= n; i++) {
        size_t l1 = 2 * i, r1 = 2 * i + 1;
        if (l1 <= n) assert(left(i - 1) == l1 - 1);
        if (r1 <= n) assert(right(i - 1) == r1 - 1);
    }

    assert(height(1) == 0 && height(2) == 1 && height(3) == 1 && height(4) == 2);
    assert(height(1023) == 9 && height(1024) == 10);
    printf("height checks ok: a million items is only ~20 levels\n");
    printf("all checks passed\n");
    return 0;
}
