/*
 * 01_references_vs_pointers.cpp — when to use T&, const T&, T*, and what each costs
 *
 * Concept : A reference is a pointer the compiler dereferences for you,
 *           with three restrictions: never null, never re-seated, always
 *           initialised. Use a reference when "there is always exactly one
 *           object"; use a pointer when "maybe none" (nullable) or when you
 *           need to re-point (iterating, re-linking nodes).
 * Build   : make run P=dsa/01-pointers/cpp/01_references_vs_pointers.cpp
 * Read    : ../README.md  (section "3.13 C++ references vs pointers")
 */
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

struct Frame { int seq; std::vector<unsigned char> pixels; };

// Read-only, big object, never null      -> const T&
static std::size_t bytes(const Frame &f) { return f.pixels.size(); }
// Must modify, never null                -> T&
static void bump(Frame &f) { ++f.seq; }
// Optional ("may be absent")             -> T* (nullptr = absent)
static int seq_or(const Frame *f, int dflt) { return f ? f->seq : dflt; }
// Cheap to copy (int, small struct)      -> by value
static int twice(int v) { return 2 * v; }

// Pointer re-seats: walk with it. A reference could not do this loop.
static const Frame *find(const std::vector<Frame> &v, int seq)
{
    for (const Frame *p = v.data(), *end = v.data() + v.size(); p != end; ++p)
        if (p->seq == seq) return p;
    return nullptr;
}

int main()
{
    Frame f{1, std::vector<unsigned char>(1920 * 1080)};
    bump(f);
    assert(f.seq == 2 && bytes(f) == 1920u * 1080u);
    assert(seq_or(&f, -1) == 2 && seq_or(nullptr, -1) == -1);
    assert(twice(21) == 42);

    std::vector<Frame> q{{10, {}}, {11, {}}, {12, {}}};
    const Frame *hit = find(q, 11);
    assert(hit && hit == &q[1]);
    assert(find(q, 99) == nullptr);

    /*
     *  Same machine code, different rules:
     *
     *  void bump(Frame &f) { ++f.seq; }     void bump(Frame *f) { ++f->seq; }
     *        both compile to:   add DWORD PTR [rdi], 1      (rdi = the address)
     *
     *  int &r = x;  -> r is bound once, forever, to x.  sizeof(r) == sizeof(x).
     *  int *p = &x; -> p is its own 8-byte object that can change.
     */
    int x = 1, y = 2;
    int &r = x;
    int *p = &x;
    r = y;            // copies y's VALUE into x; r still refers to x
    p = &y;           // re-points p
    *p = 20;
    assert(x == 2 && y == 20 && &r == &x);
    static_assert(sizeof(r) == sizeof(int), "sizeof a reference is sizeof the referee");

    // Dangling references are possible too: a reference to a vector element
    // dies when the vector reallocates (see dsa/03-debugging/cpp/03_dangling_reference_bug.cpp).
    std::vector<int> v{1, 2, 3};
    v.reserve(100);           // capacity fixed -> references stay valid below
    int &first = v[0];
    v.push_back(4);           // no reallocation thanks to reserve
    first = 100;
    assert(v[0] == 100);

    // Reference to pointer: modify the caller's pointer (C++ version of T**).
    auto advance = [](const int *&ptr) { ++ptr; };
    const int *it = v.data();
    advance(it);
    assert(*it == 2);

    std::cout << "references vs pointers: all checks passed.\n";
    return 0;
}
