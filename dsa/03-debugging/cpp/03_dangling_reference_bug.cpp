/*
 * 03_dangling_reference_bug.cpp — INTENTIONAL BUG: reference into a vector that reallocates
 *
 * Bug     : 'first' is a reference to v[0]. push_back grows the vector past its
 *           capacity, so the elements move to a NEW buffer and the old one is
 *           freed. 'first' now refers into freed memory. The most common
 *           use-after-free in modern C++ (also with iterators, string_view,
 *           pointers to map values after rehash, span over a temporary...).
 * Finds it: AddressSanitizer (heap-use-after-free; the "freed by" stack shows
 *           std::vector::_M_realloc_insert / push_back); valgrind.
 * Build   : make run P=dsa/03-debugging/cpp/03_dangling_reference_bug.cpp   -> ASan report, exit 1
 * Read    : ../README.md  (section "Bug 9: dangling reference after vector growth")
 */
#include <iostream>
#include <vector>

int main()
{
    std::vector<int> v{1, 2, 3};
    v.shrink_to_fit();                 // capacity == 3: the next push_back MUST reallocate
    int &first = v[0];                 // reference into the current buffer
    v.push_back(4);                    // reallocates: old buffer freed, 'first' dangles
    first = 100;                       // BUG: writes into freed memory
    std::cout << "v[0] = " << v[0] << '\n';   // prints 1, not 100
    return 0;
}
