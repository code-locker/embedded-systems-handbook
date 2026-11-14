/*
 * 02_nullptr.cpp — why C++11 added nullptr instead of NULL / 0
 *
 * Concept : In C++, NULL is an integer constant (0 or 0L). With overloading
 *           and templates, "0" picks the INT overload, not the pointer one.
 *           nullptr has its own type, std::nullptr_t, which converts to every
 *           pointer type and to no integer type.
 * Build   : make run P=dsa/01-pointers/cpp/02_nullptr.cpp
 * Read    : ../README.md  (section "3.14 nullptr")
 */
#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <type_traits>

struct Frame { int seq; };

static std::string kind(int) { return "int"; }
static std::string kind(long) { return "long"; }
static std::string kind(const char *) { return "pointer"; }

// Perfect forwarding loses "0 is a null pointer": the template sees an int.
template <typename T>
static std::string forward_kind(T &&v) { return kind(std::forward<T>(v)); }

int main()
{
    assert(kind(nullptr) == "pointer");
    assert(kind(0) == "int");                  // "null pointer" written as 0 -> wrong overload
    assert(kind(0L) == "long");
    // kind(NULL) is "long" or ambiguous depending on the platform: never use it in C++.

    assert(forward_kind(nullptr) == "pointer");
    // forward_kind(0) would call kind(int): the literal 0 is no longer a null pointer constant.

    static_assert(std::is_same<decltype(nullptr), std::nullptr_t>::value, "own type");
    static_assert(sizeof(std::nullptr_t) == sizeof(void *), "same size as a pointer");
    // int n = nullptr;   error: cannot convert 'std::nullptr_t' to 'int'

    int *p = nullptr;
    void (*fp)() = nullptr;
    int Frame::*pm = nullptr;    // even pointer-to-member works (see 05_*)
    (void)pm;
    assert(!p && !fp);

    std::unique_ptr<int> up = nullptr;    // smart pointers accept nullptr too
    assert(!up);

    std::cout << "kind(nullptr) = " << kind(nullptr) << ", kind(0) = " << kind(0) << '\n';
    std::cout << "All checks passed.\n";
    return 0;
}
