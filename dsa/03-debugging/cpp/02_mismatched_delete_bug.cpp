/*
 * 02_mismatched_delete_bug.cpp — INTENTIONAL BUG: new[] released with delete
 *
 * Bug     : An array allocated with new[] is released with plain delete.
 *           Only one destructor runs (the other 3 Sensors leak their
 *           strings) and operator delete gets the wrong address (the array
 *           cookie sits in front). Undefined behaviour.
 * Finds it: AddressSanitizer. Because Sensor has a destructor, new[] put an
 *           8-byte cookie in front, so delete receives an address 8 bytes INSIDE
 *           the block: "attempting free on address which was not malloc()-ed
 *           ... located 8 bytes inside of 168-byte region ... allocated by
 *           operator new[]" (SUMMARY: bad-free). For a type WITHOUT a
 *           destructor (no cookie) ASan says "alloc-dealloc-mismatch (operator
 *           new [] vs operator delete)". valgrind: "Mismatched free() / delete
 *           / delete []" or "Invalid free()". gcc warns at compile time when it
 *           can see both sides (-Wmismatched-new-delete) — here it cannot.
 * Build   : make run P=dsa/03-debugging/cpp/02_mismatched_delete_bug.cpp   -> ASan report, exit 1
 * Read    : ../README.md  (section "Bug 8: mismatched new[]/delete")
 */
#include <iostream>
#include <string>

struct Sensor {
    std::string name = "a-name-long-enough-to-live-on-the-heap";
    int value = 0;
};

static Sensor *make_sensors(int n) { return new Sensor[n]; }   // caller must delete[]

static void destroy(Sensor *s) { delete s; }                   // BUG: should be delete[]

int main()
{
    Sensor *s = make_sensors(4);
    s[3].value = 42;
    std::cout << "last sensor value " << s[3].value << '\n';
    destroy(s);
    return 0;
}
