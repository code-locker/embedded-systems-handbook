/*
 * 10_std_string_internals.cpp — what is inside a std::string: SSO, growth,
 *                               copy vs move, counted heap allocations
 *
 * Concept : std::string is the C string builder (pointer, size, capacity)
 *           wrapped in RAII, plus one big optimisation: Small String
 *           Optimisation (SSO). Short strings (<= 15 chars in libstdc++/MSVC,
 *           <= 22 in libc++) are stored INSIDE the string object itself, so
 *           they cost zero heap allocations. We prove it by replacing the
 *           global operator new to count allocations, by checking whether
 *           data() points inside the object, and by watching capacity().
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/10_std_string_internals.cpp
 * Read    : ../notes/strings.md  (section "How std::string works inside")
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

// ---------------------------------------------------------------------------
// Counting allocator: replace the GLOBAL operator new/delete. Every heap
// allocation made by any std:: container in this program goes through here.
// (Replacing these is explicitly allowed by the standard. They forward to
// malloc/free, so ASan still tracks every byte.)
static std::size_t g_allocs = 0;
static std::size_t g_bytes = 0;

void *operator new(std::size_t n)
{
    ++g_allocs;
    g_bytes += n;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
// (array forms default to these single-object forms)

// RAII helper: "how many allocations happened in this scope?"
struct AllocScope {
    std::size_t start = g_allocs;
    std::size_t count() const { return g_allocs - start; }
};

// Is p inside the bytes of object obj? -> the characters live in the object
template <class T>
static bool points_inside(const void *p, const T &obj)
{
    auto a = reinterpret_cast<std::uintptr_t>(p);
    auto lo = reinterpret_cast<std::uintptr_t>(&obj);
    return a >= lo && a < lo + sizeof(T);
}

int main()
{
    std::puts("== 1. the object itself ==");
    /*
     * libstdc++ (GCC) layout, 32 bytes on 64-bit:
     *
     *   std::string s = "hi";            (SSO: chars stored INSIDE)
     *   +----------------+--------+--------------------------------+
     *   | _M_p (pointer) | size=2 | local buf[16]: h i \0 . . . .  |
     *   +-------|--------+--------+--------------------------------+
     *           +---------------------^  points to its OWN buffer
     *
     *   std::string s(100, 'x');         (heap)
     *   +----------------+--------+--------------------------------+
     *   | _M_p ----------+ size=100 | capacity=100 | (unused)       |
     *   +----------------|---------+---------------------------------+
     *                    v
     *          heap: [x x x ... x \0]   (capacity + 1 bytes)
     *
     * The union trick: when on the heap, the 16-byte local buffer is reused
     * to store the capacity. libc++ (clang) packs even harder: 24 bytes
     * total, 22 chars inline, a flag bit says which mode it is in.
     */
    std::printf("  sizeof(std::string) = %zu bytes\n", sizeof(std::string));
    std::string empty;
    std::printf("  empty string capacity() = %zu  (the inline SSO buffer)\n",
                empty.capacity());
    CHECK(empty.size() == 0 && empty.c_str()[0] == '\0');

    std::puts("\n== 2. SSO: where do the characters live? ==");
    std::printf("  %-6s %-9s %-7s %s\n", "len", "capacity", "allocs", "data() inside object?");
    std::size_t sso_limit = 0;
    for (std::size_t len : {0u, 1u, 7u, 15u, 16u, 22u, 23u, 31u, 64u}) {
        AllocScope sc;
        std::string s(len, 'a');
        bool inside = points_inside(s.data(), s);
        std::printf("  %-6zu %-9zu %-7zu %s\n", len, s.capacity(), sc.count(),
                    inside ? "yes (SSO, no heap)" : "no  -> heap");
        // The core SSO fact, implementation independent:
        // chars inside the object  <=>  no allocation was needed.
        CHECK(inside == (sc.count() == 0));
        if (inside)
            sso_limit = len;
    }
    std::printf("  => this library stores up to %zu chars inline\n",
                empty.capacity());
#if defined(__GLIBCXX__)
    CHECK(empty.capacity() == 15 && sso_limit == 15);
    // Peek at the first member: in libstdc++ it IS the data pointer.
    std::string peek(40, 'p');
    const char *first_member;
    std::memcpy(&first_member, &peek, sizeof first_member);
    std::printf("  libstdc++: first 8 bytes of the object = %p, data() = %p\n",
                static_cast<const void *>(first_member),
                static_cast<const void *>(peek.data()));
    CHECK(first_member == peek.data());
#endif

    std::puts("\n== 3. growth: capacity doubles (amortised O(1) push_back) ==");
    {
        AllocScope sc;
        std::string s;
        std::size_t last = s.capacity();
        std::printf("  capacity steps:");
        for (int i = 0; i < 1000; ++i) {
            s.push_back('x');
            if (s.capacity() != last) {
                last = s.capacity();
                std::printf(" %zu", last);
            }
        }
        std::printf("\n  1000 push_backs -> %zu allocations\n", sc.count());
        CHECK(sc.count() <= 8); // log2(1000/15) ~ 6
    }
    {
        AllocScope sc;
        std::string s;
        s.reserve(1000); // one allocation up front
        for (int i = 0; i < 1000; ++i)
            s += 'x';
        std::printf("  with reserve(1000) first -> %zu allocation\n", sc.count());
        CHECK(sc.count() == 1);
    }

    std::puts("\n== 4. copy vs move ==");
    std::string big(1000, 'B');
    {
        AllocScope sc;
        std::string copy = big; // deep copy: allocate + memcpy 1000 bytes, O(n)
        std::printf("  copy of a 1000-char string: %zu allocation(s), O(n)\n",
                    sc.count());
        CHECK(sc.count() == 1 && copy == big);
    }
    {
        AllocScope sc;
        const char *old_data = big.data();
        std::string stolen = std::move(big); // steal the pointer, O(1)
        /*
         *  before:  big    [ptr ---> heap "BBB..."] size 1000
         *  after :  stolen [ptr ---> heap "BBB..."] size 1000  (same block!)
         *           big    [ptr -> own SSO buffer ""] size 0  (valid, unspecified)
         */
        std::printf("  move: %zu allocations, same buffer reused: %s\n",
                    sc.count(), stolen.data() == old_data ? "yes" : "no");
        CHECK(sc.count() == 0);
        CHECK(stolen.data() == old_data);
        CHECK(stolen.size() == 1000);
        // big is "valid but unspecified": you may assign to it or clear it.
        big.clear();
        big = "reused";
        CHECK(big == "reused");
    }
    {
        // Moving a SHORT string still copies its chars (they live inside the
        // object, there is no pointer to steal) — but that is <= 15 bytes.
        std::string small = "short";
        AllocScope sc;
        std::string moved = std::move(small);
        CHECK(sc.count() == 0 && moved == "short");
    }

    std::puts("\n== 5. pass by value vs const& vs move into a container ==");
    std::vector<std::string> v;
    v.reserve(4); // so vector growth doesn't add allocations to our count
    std::string name(50, 'n');
    {
        AllocScope sc;
        v.push_back(name); // copy: name is still needed afterwards
        CHECK(sc.count() == 1);
    }
    {
        AllocScope sc;
        v.push_back(std::move(name)); // move: 0 allocations
        CHECK(sc.count() == 0);
    }
    {
        AllocScope sc;
        v.emplace_back(40, 'e'); // construct in place: exactly 1 (its buffer)
        CHECK(sc.count() == 1);
    }
    std::puts("  push_back(copy)=1 alloc, push_back(move)=0, emplace_back=1");

    std::puts("\n== 6. concatenation in a loop ==");
    {
        AllocScope sc;
        std::string out;
        for (int i = 0; i < 200; ++i)
            out += "chunk "; // amortised: few reallocations
        std::size_t plus_eq = sc.count();
        AllocScope sc2;
        std::string out2;
        for (int i = 0; i < 200; ++i)
            out2 = out2 + "chunk "; // builds a NEW temporary every time
        std::printf("  s += x : %zu allocs;  s = s + x : %zu allocs\n", plus_eq,
                    sc2.count());
        CHECK(out == out2);
        CHECK(plus_eq < sc2.count());
    }

    std::printf("\ntotal heap allocations in this program: %zu (%zu bytes)\n",
                g_allocs, g_bytes);
    std::puts("all checks passed");
    return 0;
}
