/*
 * 03_smart_pointers_preview.cpp — raw pointer + ownership rules = smart pointer
 *
 * Concept : A raw pointer does not say who frees the memory. A smart pointer
 *           is a small object that OWNS a heap object and frees it in its
 *           destructor (RAII):
 *             unique_ptr<T> — exactly one owner, move-only, zero overhead
 *             shared_ptr<T> — reference-counted shared ownership
 *             weak_ptr<T>   — observes a shared_ptr without owning it
 *           Raw pointers / references remain fine for NON-owning access.
 *           Full treatment: dsa/02-memory-management.
 * Build   : make run P=dsa/01-pointers/cpp/03_smart_pointers_preview.cpp
 * Read    : ../README.md  (section "3.15 Smart pointers preview")
 */
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

static int g_alive = 0;   // counts live Buffers so we can PROVE they are freed

struct Buffer {
    std::string name;
    explicit Buffer(std::string n) : name(std::move(n)) { ++g_alive; std::cout << "  + " << name << '\n'; }
    ~Buffer() { --g_alive; std::cout << "  - " << name << '\n'; }
};

// Takes ownership: the parameter type SAYS so.
static void consume(std::unique_ptr<Buffer> b) { assert(b); }   // freed at the end of this call

// Only looks: raw pointer / reference, no ownership.
static const std::string &peek(const Buffer &b) { return b.name; }

int main()
{
    std::cout << "unique_ptr:\n";
    {
        auto a = std::make_unique<Buffer>("frame-A");
        /*
         *  a (stack, 8 bytes)        heap
         *  +-----------+            +-----------------+
         *  | ptr  ●────┼──────────► | Buffer "frame-A"|
         *  +-----------+            +-----------------+
         *  ~unique_ptr() runs at '}' and deletes the Buffer.
         */
        static_assert(sizeof(a) == sizeof(Buffer *), "unique_ptr costs nothing");
        assert(peek(*a) == "frame-A");
        auto b = std::move(a);       // ownership MOVES; a becomes empty
        assert(!a && b);
        consume(std::move(b));       // ownership moves into consume(); freed there
        assert(!b && g_alive == 0);
    }

    std::cout << "shared_ptr:\n";
    {
        auto s1 = std::make_shared<Buffer>("frame-S");
        {
            std::shared_ptr<Buffer> s2 = s1;    // count 2
            assert(s1.use_count() == 2);
        }                                        // count back to 1
        assert(s1.use_count() == 1 && g_alive == 1);

        std::weak_ptr<Buffer> w = s1;            // does NOT raise the count
        assert(s1.use_count() == 1);
        if (auto locked = w.lock())              // temporary shared_ptr while using
            assert(locked->name == "frame-S");
        s1.reset();                              // last owner gone -> freed
        assert(w.expired() && g_alive == 0);
    }

    std::cout << "All checks passed: every Buffer was freed (g_alive = " << g_alive << ").\n";
    return 0;
}
