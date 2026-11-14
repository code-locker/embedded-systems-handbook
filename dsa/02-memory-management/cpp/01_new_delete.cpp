/*
 * 01_new_delete.cpp — what new / delete / new[] / delete[] really do
 *
 * Concept : `new T(args)` = (1) call operator new(sizeof(T)) to get raw bytes
 *           (glibc: malloc underneath), (2) run T's constructor in them.
 *           `delete p` = (1) run ~T(), (2) operator delete(p) (free).
 *           `new T[n]` may store n in a hidden "array cookie" in front of the
 *           array so `delete[]` knows how many destructors to run — which is
 *           why mixing new[] with delete (or malloc with delete) is UB.
 * Build   : make run P=dsa/02-memory-management/cpp/01_new_delete.cpp
 * Read    : ../README.md  (section "3.10 C++: new and delete")
 */
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>

static int g_ctor = 0, g_dtor = 0;
static std::size_t g_last_array_request = 0;

struct Widget {
    int id;
    explicit Widget(int i = 0) : id(i) { ++g_ctor; }
    ~Widget() { ++g_dtor; }

    // Class-specific operator new[] lets us SEE how many bytes new[] asks for.
    static void *operator new[](std::size_t bytes)
    {
        g_last_array_request = bytes;
        return ::operator new[](bytes);
    }
    static void operator delete[](void *p) noexcept { ::operator delete[](p); }
};

struct Trivial { int v; };   // no destructor -> no cookie needed

int main()
{
    // 1. Single object.
    Widget *w = new Widget(7);       // operator new(sizeof(Widget)) + Widget(7)
    assert(w->id == 7 && g_ctor == 1);
    delete w;                        // ~Widget() + operator delete
    assert(g_dtor == 1);
    w = nullptr;
    delete w;                        // deleting nullptr is a no-op (like free(NULL))

    // 2. Arrays: n constructors, n destructors.
    g_ctor = g_dtor = 0;
    Widget *arr = new Widget[3];
    std::cout << "new Widget[3]: sizeof(Widget)=" << sizeof(Widget) << ", operator new[] asked for "
              << g_last_array_request << " bytes ("
              << g_last_array_request - 3 * sizeof(Widget) << " extra = the array cookie storing n)\n";
    assert(g_ctor == 3);
    assert(g_last_array_request == 3 * sizeof(Widget) + sizeof(std::size_t));   // Itanium C++ ABI
    delete[] arr;                    // reads the cookie: runs 3 destructors
    assert(g_dtor == 3);
    //  delete arr;   <- would run ONE destructor and free the wrong address: UB
    //                   (ASan: "alloc-dealloc-mismatch (operator new [] vs operator delete)")

    /*
     *  new Widget[3] memory:
     *   operator new[] ──► +--------+--------+--------+--------+
     *                      | n = 3  | W[0]   | W[1]   | W[2]   |
     *                      +--------+--------+--------+--------+
     *                                ^ pointer you get back (cookie is hidden in front)
     */

    Trivial *t = new Trivial[4]();   // () = value-initialise to zero
    assert(t[3].v == 0);
    delete[] t;

    // 3. new never returns nullptr — it THROWS std::bad_alloc. Use nothrow to get nullptr.
    // 2^60 bytes: a valid size_t, but no machine can provide it.
#if defined(__SANITIZE_ADDRESS__)
    // ASan treats a failed allocation as a bug and aborts (unless run with
    // ASAN_OPTIONS=allocator_may_return_null=1), so only try it without ASan.
    std::cout << "(ASan build: skipping the nothrow out-of-memory demo)\n";
#else
    volatile std::size_t huge = (std::size_t(1) << 60) / sizeof(int);
    int *maybe = new (std::nothrow) int[huge];
    std::cout << "new (std::nothrow) int[2^58] -> " << maybe << '\n';
    assert(maybe == nullptr);
#endif
    try {
        // n * sizeof(int) does not even fit in size_t: the new-expression
        // throws std::bad_array_new_length before calling operator new[].
        volatile std::size_t vn = static_cast<std::size_t>(-1) / 2;
        int *p = new int[vn];
        delete[] p;
    } catch (const std::bad_array_new_length &) {
        std::cout << "new int[huge] threw std::bad_array_new_length\n";
    } catch (const std::bad_alloc &) {
        std::cout << "new int[huge] threw std::bad_alloc\n";
    }

    // 4. operator new on its own = raw memory, like malloc (no constructor).
    void *raw = ::operator new(64);
    ::operator delete(raw);

    // 5. Never mix families: malloc<->free, new<->delete, new[]<->delete[].
    int *cm = static_cast<int *>(std::malloc(sizeof(int)));
    std::free(cm);

    std::cout << "All checks passed.\n";
    return 0;
}
