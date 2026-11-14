/*
 * 01_references.cpp — C++ references: an alias, not an object
 *
 * Concept : A reference is another NAME for an existing object. It must be
 *           initialised, can never be re-seated, can never be null. Under the
 *           hood the compiler usually passes a pointer, but the language hides
 *           the & and * for you. const T& lets you pass big objects cheaply.
 * Build   : make run P=dsa/00-foundations/cpp/01_references.cpp
 * Read    : ../README.md  (section "C vs C++: references")
 */
#include <cassert>
#include <iostream>
#include <string>

// C style: the caller must write &x, the callee must write *p, p might be NULL.
static void inc_ptr(int *p) { if (p) ++*p; }

// C++ style: the caller writes x, the callee writes r. Cannot be null.
static void inc_ref(int &r) { ++r; }

// Pass by const reference: no copy of the string, and no way to modify it.
static std::size_t count_spaces(const std::string &s)
{
    std::size_t n = 0;
    for (char c : s)
        n += (c == ' ');
    return n;
}

// Returning a reference lets the caller assign THROUGH the function call.
static int &element(int (&arr)[4], int i) { return arr[i]; }  // arr: reference to array of 4

int main()
{
    int x = 10;
    inc_ptr(&x);
    inc_ref(x);
    std::cout << "after inc_ptr and inc_ref: x = " << x << '\n';
    assert(x == 12);

    /*  int  x = 12;        int &alias = x;
     *
     *        x, alias            Both names label the SAME box.
     *      +----------+          &alias == &x — there is no separate
     *      |    12    |          "reference object" you can take the
     *      +----------+          address of.
     *      0x7ffd...a4
     */
    int &alias = x;
    alias = 99;
    assert(x == 99 && &alias == &x);
    std::cout << "&x = " << &x << ", &alias = " << &alias << " (same address)\n";

    // A reference cannot be re-seated: this ASSIGNS y's value into x.
    int y = 5;
    alias = y;
    assert(x == 5 && &alias == &x);

    int arr[4] = {1, 2, 3, 4};
    element(arr, 2) = 30;               // writes into arr[2]
    assert(arr[2] == 30);
    static_assert(sizeof(arr) == 4 * sizeof(int), "reference to array keeps its size");

    std::string text = "a b c d";
    assert(count_spaces(text) == 3);
    // const& can bind to a temporary (lifetime extended to the reference's scope):
    const std::string &tmp = std::string("x y");
    assert(count_spaces(tmp) == 1);

    // Range-for: auto& modifies in place, auto copies.
    for (int &v : arr) v *= 2;
    assert(arr[0] == 2 && arr[3] == 8);

    std::cout << "All checks passed.\n";
    return 0;
}
