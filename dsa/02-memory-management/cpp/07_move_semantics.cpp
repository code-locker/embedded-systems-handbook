/*
 * 07_move_semantics.cpp — moving = stealing resources instead of copying them
 *
 * Concept : An rvalue (a temporary, or something you wrapped in std::move) is
 *           an object nobody will look at again, so it is safe to STEAL its
 *           heap buffer: copy 3 pointers instead of N bytes. std::move does not
 *           move anything — it is just a cast to T&&. The moved-from object is
 *           left "valid but unspecified" (for std types: usually empty).
 *           std::vector only moves elements during growth if the move
 *           constructor is noexcept; otherwise it copies (strong guarantee).
 * Build   : make run P=dsa/02-memory-management/cpp/07_move_semantics.cpp
 * Read    : ../README.md  (section "3.15 Move semantics")
 */
#include <cassert>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct Counts { int copies = 0, moves = 0; };

template <bool Noexcept>
struct Frame {
    std::vector<unsigned char> pixels;
    static inline Counts c;
    explicit Frame(std::size_t n) : pixels(n) {}
    Frame(const Frame &o) : pixels(o.pixels) { ++c.copies; }
    Frame(Frame &&o) noexcept(Noexcept) : pixels(std::move(o.pixels)) { ++c.moves; }
    Frame &operator=(const Frame &) = default;
    Frame &operator=(Frame &&) = default;
};

static void take_by_value(std::string s) { assert(!s.empty()); }
static void take_rvalue(std::string &&s) { std::string mine = std::move(s); assert(!mine.empty()); }

static std::vector<int> make_big()
{
    std::vector<int> v(1000, 1);
    return v;                         // NRVO / implicit move: never a deep copy
}

int main()
{
    // 1. What a move does to a std::string with a heap buffer.
    std::string a(100, 'x');
    const char *buf = a.data();
    std::string b = std::move(a);     // steals the pointer
    /*
     *   before:  a {ptr ●──► "xxxx...x" (100 B heap), size 100}
     *   after :  a {ptr ●──► "" (SSO), size 0}
     *            b {ptr ●──► "xxxx...x" (SAME heap buffer), size 100}
     */
    assert(b.data() == buf);          // same buffer, no copy
    assert(a.empty());                // libstdc++: moved-from string is empty
    a = "reusable";                   // moved-from objects may be assigned again
    std::cout << "string move: buffer pointer transferred, source now \"" << a << "\" after reassign\n";

    // 2. std::move is a cast. Value categories:
    int x = 1;
    static_assert(std::is_same<decltype(std::move(x)), int &&>::value, "std::move = static_cast<T&&>");
    (void)x;

    take_by_value(b);                 // copy (b is an lvalue)
    take_by_value(std::move(b));      // move into the parameter
    take_rvalue(std::string("temp")); // temporaries bind to && directly

    // 3. Return values are moved (or elided), never deep-copied.
    std::vector<int> v = make_big();
    assert(v.size() == 1000);

    // 4. vector growth: noexcept move -> moves; potentially-throwing move -> copies!
    {
        std::vector<Frame<true>> frames;
        for (int i = 0; i < 17; i++) frames.emplace_back(1024);
        std::cout << "noexcept move ctor:     copies=" << Frame<true>::c.copies
                  << " moves=" << Frame<true>::c.moves << '\n';
        assert(Frame<true>::c.copies == 0 && Frame<true>::c.moves > 0);
    }
    {
        std::vector<Frame<false>> frames;
        for (int i = 0; i < 17; i++) frames.emplace_back(1024);
        std::cout << "move ctor NOT noexcept: copies=" << Frame<false>::c.copies
                  << " moves=" << Frame<false>::c.moves << "  <- every reallocation deep-copies!\n";
        assert(Frame<false>::c.copies > 0 && Frame<false>::c.moves == 0);
    }

    // 5. emplace_back constructs in place; push_back(T(...)) constructs + moves.
    std::vector<std::string> names;
    names.reserve(2);
    names.emplace_back(50, 'n');      // constructed directly inside the vector
    names.push_back(std::string(50, 'm'));   // temporary, then moved in
    assert(names.size() == 2);

    // 6. Don't std::move a const object: it silently copies.
    const std::string c(64, 'c');
    std::string d = std::move(c);     // const&& binds to the COPY constructor
    assert(c.size() == 64 && d.size() == 64);

    std::cout << "All checks passed.\n";
    return 0;
}
