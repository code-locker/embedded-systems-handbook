/*
 * 02_counting_operator_new.cpp - replace global operator new to SEE every heap allocation
 *
 * Concept : operator new / operator delete are ordinary replaceable functions.
 *           Define them in your program and every `new`, every std::vector
 *           growth, every std::string longer than its small buffer goes through
 *           YOUR code. Here they forward to malloc/free and count. This is how
 *           allocation profilers and "no allocation in the real-time path"
 *           checks (audio, video pipelines) are built.
 * Build   : make run P=dsa/02-memory-management/cpp/02_counting_operator_new.cpp
 * Read    : ../README.md  (section "3.10 C++: new and delete" -> "operator new is malloc")
 */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <vector>

static std::size_t g_allocs = 0, g_frees = 0, g_bytes = 0;

// --- replacements (all forms the program might use) ------------------------
void *operator new(std::size_t n)
{
    ++g_allocs;
    g_bytes += n;
    if (void *p = std::malloc(n ? n : 1))   // new(0) must return a unique pointer
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    try { return ::operator new(n); } catch (...) { return nullptr; }
}
void operator delete(void *p) noexcept
{
    if (p) { ++g_frees; std::free(p); }
}
void operator delete[](void *p) noexcept { ::operator delete(p); }
void operator delete(void *p, std::size_t) noexcept { ::operator delete(p); }   // sized (C++14)
void operator delete[](void *p, std::size_t) noexcept { ::operator delete(p); }

// Measure the allocations made by a block of code.
struct Scope {
    const char *name;
    std::size_t a0 = g_allocs, b0 = g_bytes;
    explicit Scope(const char *n) : name(n) {}
    std::size_t allocs() const { return g_allocs - a0; }
    ~Scope() { std::printf("  %-48s %3zu allocation(s), %6zu bytes\n", name, g_allocs - a0, g_bytes - b0); }
};

int main()
{
    std::printf("Heap allocations made by everyday C++:\n");
    {
        Scope s("new int");
        delete new int(5);
        assert(s.allocs() == 1);
    }
    {
        Scope s("std::string short (\"cam0\") - SSO, in-object");
        std::string str = "cam0";
        assert(s.allocs() == 0);           // small-string optimisation: no heap
    }
    {
        Scope s("std::string 40 chars");
        std::string str(40, 'x');
        assert(s.allocs() == 1);
    }
    {
        Scope s("vector<int> push_back x1000, no reserve");
        std::vector<int> v;
        for (int i = 0; i < 1000; i++) v.push_back(i);
        assert(s.allocs() == 11);          // libstdc++ doubles: 1,2,4,...,1024 -> 11 allocations
    }
    {
        Scope s("vector<int> reserve(1000) then push_back x1000");
        std::vector<int> v;
        v.reserve(1000);
        for (int i = 0; i < 1000; i++) v.push_back(i);
        assert(s.allocs() == 1);
    }
    {
        Scope s("shared_ptr<int>(new int) - object + control block");
        std::shared_ptr<int> p(new int(1));
        assert(s.allocs() == 2);
    }
    {
        Scope s("make_shared<int>() - ONE block for both");
        auto p = std::make_shared<int>(1);
        assert(s.allocs() == 1);
    }
    {
        Scope s("make_unique<int>()");
        auto p = std::make_unique<int>(1);
        assert(s.allocs() == 1);
    }
    {
        Scope s("std::map<int,int> with 100 inserts (1 per node)");
        std::map<int, int> m;
        for (int i = 0; i < 100; i++) m[i] = i;
        assert(s.allocs() == 100);
    }
    std::printf("\ntotal: %zu allocations, %zu frees, %zu bytes requested\n", g_allocs, g_frees, g_bytes);
    assert(g_allocs >= g_frees);
    std::printf("All checks passed.\n");
    return 0;
}
