/*
 * 02_overloading_and_mangling.cpp — how C++ overloading works at the LINKER level
 *
 * Concept : C has one symbol per function name, so you cannot have two
 *           functions called area(). C++ allows overloading by encoding the
 *           parameter types into the symbol name ("name mangling"):
 *               area(int)            -> _Z4areai
 *               area(double, double) -> _Z4areadd
 *           extern "C" turns mangling OFF so C code (and dlsym) can find it.
 * Build   : make run P=dsa/00-foundations/cpp/02_overloading_and_mangling.cpp
 * Read    : ../README.md  (section "C vs C++: overloading, name mangling, extern \"C\"")
 *
 * See the real symbols:
 *   nm build/dsa/00-foundations/cpp/02_overloading_and_mangling | grep -E 'area|c_api'
 *   nm -C build/dsa/00-foundations/cpp/02_overloading_and_mangling | grep -E 'area|c_api'   # demangled
 *   echo _Z4areadd | c++filt
 */
#include <cassert>
#include <cstdlib>
#include <cxxabi.h>      // abi::__cxa_demangle (gcc/clang)
#include <iostream>
#include <memory>
#include <string>
#include <typeinfo>

// Three overloads: the compiler picks one at COMPILE time from the argument types.
int area(int side) { return side * side; }
double area(double w, double h) { return w * h; }
long area(long r) { return 3 * r * r; }   // deliberately silly "circle" to show a 3rd overload

// extern "C": plain, unmangled symbol "c_api_version" — callable from C,
// findable with dlsym(), and exactly how C headers are wrapped for C++:
//
//   #ifdef __cplusplus
//   extern "C" {
//   #endif
//   int c_api_version(void);
//   #ifdef __cplusplus
//   }
//   #endif
extern "C" int c_api_version(void) { return 3; }

// Demangle a symbol at run time (what c++filt does).
static std::string demangle(const char *mangled)
{
    int status = 0;
    std::unique_ptr<char, void (*)(void *)> p(
        abi::__cxa_demangle(mangled, nullptr, nullptr, &status), std::free);
    return status == 0 ? std::string(p.get()) : std::string(mangled);
}

int main()
{
    assert(area(3) == 9);
    assert(area(2.0, 4.5) == 9.0);
    assert(area(2L) == 12L);
    assert(c_api_version() == 3);

    const char *symbols[] = {"_Z4areai", "_Z4areadd", "_Z4areal", "_ZNSt6vectorIiSaIiEE9push_backERKi"};
    std::cout << "mangled symbol                      -> demangled\n";
    for (const char *s : symbols)
        std::cout << "  " << s << std::string(std::string(s).size() < 34 ? 34 - std::string(s).size() : 1, ' ') << "-> " << demangle(s) << '\n';
    assert(demangle("_Z4areadd") == "area(double, double)");

    // The type names the compiler uses internally are also mangled:
    std::cout << "typeid(area(1.0, 1.0)).name() is mangled; demangled = "
              << demangle(typeid(area(1.0, 1.0)).name()) << '\n';

    std::cout << "c_api_version has NO mangling: nm shows plain 'c_api_version'\n";
    std::cout << "All checks passed.\n";
    return 0;
}
