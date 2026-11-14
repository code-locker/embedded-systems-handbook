/*
 * 03_c_vs_cpp_differences.cpp — the C/C++ differences that actually bite
 *
 * Concept : C++ is NOT a superset of C. Valid C can be an error or MEAN
 *           SOMETHING DIFFERENT in C++. Each check below is a place where the
 *           two languages differ; the matching C behaviour is in the comment.
 * Build   : make run P=dsa/00-foundations/cpp/03_c_vs_cpp_differences.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <type_traits>

struct Point { int x, y; };          // C needs "struct Point p;" or a typedef

// Overloading on int vs pointer — why nullptr exists.
static const char *which(int) { return "int"; }
static const char *which(const char *) { return "pointer"; }

// In C++ a const global has INTERNAL linkage by default (in C: external).
const int kLimit = 8;
constexpr int square(int v) { return v * v; }     // evaluated at compile time when possible

int main()
{
    // 1. 'A' is a char in C++ (an int in C).
    static_assert(sizeof('A') == 1, "char literal is char in C++");

    // 2. void* does NOT implicitly convert to T* in C++. In C:  int *p = malloc(...); is fine.
    int *p = static_cast<int *>(std::malloc(4 * sizeof(int)));
    assert(p != nullptr);
    p[0] = 1;
    std::free(p);

    // 3. Struct tag is a type name: no 'struct' keyword needed.
    Point pt{1, 2};
    assert(pt.x + pt.y == 3);

    // 4. bool is a built-in type with true/false keywords (C11: <stdbool.h> macros).
    bool b = true;
    static_assert(std::is_same<decltype(b), bool>::value, "bool is a real type");

    // 5. nullptr has its own type; NULL is often just 0 and picks the int overload.
    assert(std::string(which(nullptr)) == "pointer");
    assert(std::string(which(0)) == "int");

    // 6. const ints are real constant expressions: usable as array bounds.
    int arr[kLimit] = {};
    static_assert(sizeof(arr) == 8 * sizeof(int), "const int is a constant expression");
    static_assert(square(4) == 16, "constexpr runs in the compiler");

    // 7. f() means "no parameters" in C++. In C (before C23), f() means
    //    "unspecified parameters" — use f(void) in C.

    // 8. Designated initializers: C99 has .x = 1; C++ only from C++20, in order.

    // 9. Variable length arrays: C99 has int a[n]; standard C++ does not (gcc
    //    allows it as an extension; -Wpedantic warns). Use std::vector.

    // 10. Implicit int, K&R declarations, 'restrict', _Generic: C only.
    //     Classes, templates, references, exceptions, namespaces: C++ only.

    // 11. Goto/switch may jump past declarations in C; in C++ jumping past an
    //     initialisation is an error.

    std::cout << "sizeof('A') = " << sizeof('A') << " in C++ (C says " << sizeof(int) << ")\n";
    std::cout << "which(nullptr) = " << which(nullptr) << ", which(0) = " << which(0) << '\n';
    std::cout << "All checks passed.\n";
    return 0;
}
