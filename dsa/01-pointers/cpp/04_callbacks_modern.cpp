/*
 * 04_callbacks_modern.cpp — function pointers vs lambdas vs std::function vs templates
 *
 * Concept : C callbacks are (function pointer, void *user_data). C++ gives
 *           three upgrades: lambdas (functions written in place that can
 *           CAPTURE context), std::function (type-erased "anything callable",
 *           may heap-allocate), and templates (the callable type is known at
 *           compile time, so the call can be inlined — this is why std::sort
 *           beats qsort).
 * Build   : make run P=dsa/01-pointers/cpp/04_callbacks_modern.cpp
 * Read    : ../README.md  (section "3.7 Function pointers" -> C++ part)
 */
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// 1. C style: function pointer + void* context.
using c_callback = void (*)(int value, void *user);
static void c_for_each(const std::vector<int> &v, c_callback cb, void *user)
{
    for (int x : v) cb(x, user);
}
static void c_sum(int value, void *user) { *static_cast<long *>(user) += value; }

// 2. Template: any callable; the compiler sees the exact type -> inlinable.
template <typename F>
static void tmpl_for_each(const std::vector<int> &v, F &&f)
{
    for (int x : v) f(x);
}

// 3. std::function: any callable with this signature, chosen at RUN time.
static void fn_for_each(const std::vector<int> &v, const std::function<void(int)> &f)
{
    for (int x : v) f(x);
}

static int cmp_c(const void *a, const void *b)
{
    int x = *static_cast<const int *>(a), y = *static_cast<const int *>(b);
    return (x > y) - (x < y);
}

int main()
{
    std::vector<int> v{5, 3, 9, 1};

    long total = 0;
    c_for_each(v, c_sum, &total);
    assert(total == 18);

    // A capture-less lambda converts to a plain function pointer:
    c_callback as_ptr = [](int value, void *user) { *static_cast<long *>(user) -= value; };
    c_for_each(v, as_ptr, &total);
    assert(total == 0);

    // Capturing lambdas carry their context; no void* needed.
    int count_big = 0, threshold = 4;
    tmpl_for_each(v, [&count_big, threshold](int x) { count_big += (x > threshold); });
    assert(count_big == 2);

    std::vector<int> seen;
    fn_for_each(v, [&seen](int x) { seen.push_back(x); });
    assert(seen == v);

    // qsort (function pointer, opaque) vs std::sort (template, inlined comparator).
    std::vector<int> a = v, b = v;
    std::qsort(a.data(), a.size(), sizeof(int), cmp_c);
    std::sort(b.begin(), b.end(), [](int x, int y) { return x < y; });
    assert(a == b);

    // Dispatch table, C++ edition: name -> std::function.
    std::map<std::string, std::function<int(int)>> cmds{
        {"double", [](int x) { return 2 * x; }},
        {"negate", [](int x) { return -x; }},
        {"offset", [base = 100](int x) { return base + x; }},   // init-capture
    };
    assert(cmds.at("double")(21) == 42);
    assert(cmds.at("offset")(1) == 101);
    assert(cmds.find("reboot") == cmds.end());

    // Pointer to member function: the C++ way to point at a method.
    struct Sensor { int v; int read() const { return v; } };
    int (Sensor::*getter)() const = &Sensor::read;
    Sensor s{7};
    assert((s.*getter)() == 7);
    assert(std::invoke(getter, s) == 7);

    std::cout << "callbacks: all checks passed.\n";
    return 0;
}
