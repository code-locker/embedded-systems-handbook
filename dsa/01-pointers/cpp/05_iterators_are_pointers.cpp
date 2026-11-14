/*
 * 05_iterators_are_pointers.cpp — STL iterators are generalised pointers
 *
 * Concept : The STL was designed so that a raw pointer IS a valid iterator
 *           for an array: *it, ++it, it != end, it + n. std::vector<T>::iterator
 *           is basically a T*. Every STL algorithm therefore works on plain C
 *           arrays and on memory you got from a driver (mmap'd buffers).
 *           [begin, end) half-open ranges come straight from "one past the end".
 * Build   : make run P=dsa/01-pointers/cpp/05_iterators_are_pointers.cpp
 * Read    : ../README.md  (section "3.2 Pointer arithmetic" and "C vs C++")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <numeric>
#include <vector>

// A tiny non-owning view: (pointer, length). This is what std::span (C++20) is.
template <typename T>
class View {
public:
    View(T *data, std::size_t n) : data_(data), n_(n) {}
    T *begin() const { return data_; }
    T *end() const { return data_ + n_; }          // one past the end
    std::size_t size() const { return n_; }
    T &operator[](std::size_t i) const { return data_[i]; }
private:
    T *data_;
    std::size_t n_;
};

int main()
{
    int raw[] = {4, 1, 3, 2};
    // Raw pointers fed directly to STL algorithms:
    std::sort(raw, raw + 4);
    assert(std::is_sorted(std::begin(raw), std::end(raw)));
    assert(std::accumulate(raw, raw + 4, 0) == 10);
    int *pos = std::find(raw, raw + 4, 3);
    assert(pos - raw == 2);                       // iterator difference = index

    std::vector<int> v{10, 20, 30};
    // vector storage is a contiguous array: &v[0] + i == &v[i]
    assert(&*v.begin() == v.data());
    assert(v.data() + 2 == &v[2]);
    auto it = v.begin() + 1;                      // random access like a pointer
    assert(*it == 20 && it[1] == 30);

    // View over memory we do not own (e.g. a DMA buffer from a driver).
    std::uint8_t dma[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    View<std::uint8_t> view(dma + 2, 4);          // bytes 2..5
    unsigned sum = 0;
    for (std::uint8_t b : view) sum += b;         // range-for uses begin()/end()
    assert(sum == 2 + 3 + 4 + 5 && view[0] == 2);
    std::fill(view.begin(), view.end(), 0xFF);
    assert(dma[1] == 1 && dma[2] == 0xFF && dma[5] == 0xFF && dma[6] == 6);

    // Iterator invalidation = dangling pointer: after push_back that grows the
    // capacity, old iterators/pointers point into FREED memory.
    std::vector<int> w{1};
    w.shrink_to_fit();
    const int *before = w.data();
    w.push_back(2);                               // capacity 1 -> must reallocate
    std::cout << "vector data moved on growth: " << (before != w.data() ? "yes" : "no")
              << " -> never keep pointers/iterators across push_back\n";

    std::cout << "All checks passed.\n";
    return 0;
}
