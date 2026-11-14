/*
 * 02_std_vector_and_array.cpp — std::vector and std::array in practice
 *
 * Concept : std::array<T,N> = a C array that knows its size, can be copied,
 *           returned and compared, with zero overhead (lives inline, no heap).
 *           std::vector<T> = the dynamic array: watch its real growth factor,
 *           use reserve() to avoid reallocations, and learn the iterator /
 *           pointer / reference INVALIDATION rules (the #1 vector bug).
 *           Also: erase-remove idiom, emplace_back vs push_back, 2D layouts,
 *           and the vector<bool> oddity.
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/02_std_vector_and_array.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

// std::array keeps its size in the TYPE, so a function can take it by
// reference without the C "pointer + length" pair and without decay.
template <std::size_t N>
int sum(const std::array<int, N> &a)
{
    return std::accumulate(a.begin(), a.end(), 0);
}

// Returning an array by value: impossible with C arrays, trivial here.
std::array<int, 4> make_lut()
{
    return {1, 2, 4, 8};
}

// A 2D matrix the right way: ONE contiguous vector + row-major indexing.
// vector<vector<int>> is N+1 separate heap blocks (pointer chasing, poor
// cache use); this is one block, exactly like a framebuffer.
class Matrix {
public:
    Matrix(std::size_t rows, std::size_t cols)
        : rows_(rows), cols_(cols), d_(rows * cols, 0) {}
    int &operator()(std::size_t r, std::size_t c) { return d_[r * cols_ + c]; }
    int operator()(std::size_t r, std::size_t c) const { return d_[r * cols_ + c]; }
    std::size_t rows() const { return rows_; }
    std::size_t cols() const { return cols_; }
    const int *data() const { return d_.data(); }

private:
    std::size_t rows_, cols_;
    std::vector<int> d_;
};

int main()
{
    std::puts("== 1. std::array ==");
    std::array<int, 5> a{5, 3, 1, 4, 2};
    static_assert(sizeof(a) == 5 * sizeof(int), "no overhead: just the ints");
    static_assert(std::is_trivially_copyable_v<std::array<int, 5>>);
    CHECK(a.size() == 5 && sum(a) == 15);
    std::array<int, 5> b = a; // copy (C arrays cannot be assigned)
    std::sort(b.begin(), b.end());
    CHECK(b == (std::array<int, 5>{1, 2, 3, 4, 5})); // operator== works
    CHECK(a != b);
    CHECK(make_lut()[3] == 8);
    bool threw = false;
    try {
        (void)a.at(5); // checked access throws; a[5] would be UB
    } catch (const std::out_of_range &) {
        threw = true;
    }
    CHECK(threw);
    std::puts("  sized, copyable, comparable, returnable; at() checks bounds");

    std::puts("\n== 2. vector growth factor (this standard library) ==");
    std::vector<int> v;
    std::size_t last = v.capacity(), reallocs = 0;
    std::printf("  capacities:");
    for (int i = 0; i < 1000; ++i) {
        v.push_back(i);
        if (v.capacity() != last) {
            std::printf(" %zu", v.capacity());
            last = v.capacity();
            ++reallocs;
        }
    }
    std::printf("\n  -> %zu reallocations for 1000 push_backs\n", reallocs);
    // libstdc++ / libc++ double; MSVC grows by 1.5. Either way geometric.
    CHECK(reallocs < 20);

    std::puts("\n== 3. reserve() removes reallocations ==");
    std::vector<int> reserved;
    reserved.reserve(1000);
    const int *before = reserved.data();
    for (int i = 0; i < 1000; ++i)
        reserved.push_back(i);
    CHECK(reserved.data() == before); // never moved
    std::puts("  reserve(1000) then 1000 pushes: buffer never moved");

    std::puts("\n== 4. invalidation: why this is the #1 vector bug ==");
    {
        std::vector<int> w{1, 2, 3};
        w.shrink_to_fit(); // make capacity == size so the next push reallocates
        const int *old_data = w.data();
        // int &first = w[0];  w.push_back(4);  first = 9;  <-- DANGLING:
        //
        //   before:  w.data_ --> [1 2 3]          first --> &[0]
        //   push_back reallocates:
        //   after :  w.data_ --> [1 2 3 4 . .]    first --> freed memory!
        //
        // ASan reports heap-use-after-free. We only compare addresses here.
        w.push_back(4);
        CHECK(w.data() != old_data);
        std::puts("  push_back past capacity moved the buffer: every old"
                  " pointer/ref/iterator is now dangling");
        // Rule: any operation that may reallocate (push_back, insert,
        // resize, reserve) invalidates ALL; erase invalidates from the
        // erased element onward. Keep INDICES, not pointers, across growth.
    }

    std::puts("\n== 5. erase-remove idiom and erase in a loop ==");
    {
        std::vector<int> w{1, 2, 3, 4, 5, 6, 7, 8};
        // remove_if moves the keepers to the front (O(n)) and returns the new
        // logical end; erase then chops the tail. Erasing one by one would be
        // O(n^2) because every erase shifts the tail.
        w.erase(std::remove_if(w.begin(), w.end(), [](int x) { return x % 2 == 0; }),
                w.end());
        CHECK((w == std::vector<int>{1, 3, 5, 7}));
        // If you must erase while iterating, use erase's return value:
        for (auto it = w.begin(); it != w.end();) {
            if (*it == 3)
                it = w.erase(it); // NOT ++it after erase: `it` is invalid
            else
                ++it;
        }
        CHECK((w == std::vector<int>{1, 5, 7}));
        std::puts("  remove evens -> {1,3,5,7}; erase 3 safely -> {1,5,7}");
    }

    std::puts("\n== 6. emplace_back vs push_back ==");
    {
        std::vector<std::string> names;
        names.push_back(std::string(3, 'a')); // builds a temporary, then moves it
        names.emplace_back(3, 'b');           // builds directly in the slot
        CHECK(names[0] == "aaa" && names[1] == "bbb");
        std::puts("  emplace_back forwards ctor args: no temporary");
    }

    std::puts("\n== 7. 2D matrix as one contiguous vector ==");
    {
        Matrix m(3, 4);
        int k = 0;
        for (std::size_t r = 0; r < m.rows(); ++r)
            for (std::size_t c = 0; c < m.cols(); ++c)
                m(r, c) = k++;
        CHECK(m.data()[2 * 4 + 1] == m(2, 1)); // row-major
        CHECK(m(2, 3) == 11);
        std::puts("  m(r,c) == data[r*cols + c]: one allocation, cache friendly");
    }

    std::puts("\n== 8. the vector<bool> oddity ==");
    {
        std::vector<bool> bits(100, false);
        bits[42] = true;
        // vector<bool> is bit-packed: operator[] returns a proxy object, not a
        // bool&. So `bool *p = &bits[0];` does not compile and `auto x = bits[0]`
        // is a proxy. Prefer std::vector<char> or std::bitset when you need
        // real references.
        auto proxy = bits[42];
        static_assert(!std::is_same_v<decltype(proxy), bool>);
        CHECK(bits[42] && !bits[41]);
        std::puts("  vector<bool> is a bitset in disguise (proxy references)");
    }

    std::puts("\nall checks passed");
    return 0;
}
