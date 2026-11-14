/*
 * 06_rule_of_0_3_5.cpp — which special member functions a class must write
 *
 * Concept : The five special members: destructor, copy constructor, copy
 *           assignment, move constructor, move assignment.
 *           Rule of 3 (C++98): if you need ANY of dtor/copy-ctor/copy-assign,
 *             you need all three (a class that owns a raw resource).
 *           Rule of 5 (C++11): add the two moves, for performance.
 *           Rule of 0: better — own resources through members that already
 *             manage themselves (vector, string, unique_ptr) and write NONE.
 * Build   : make run P=dsa/02-memory-management/cpp/06_rule_of_0_3_5.cpp
 * Read    : ../README.md  (section "3.14 Rule of 0/3/5")
 */
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---- The bug that motivates the rule of 3 ------------------------------------
// struct Naive { int *data; Naive(size_t n) : data(new int[n]) {} ~Naive() { delete[] data; } };
// Naive a(10); Naive b = a;   // compiler-generated copy copies the POINTER
//   a.data ──► [ heap ints ] ◄── b.data
// end of scope: ~b deletes it, ~a deletes it AGAIN -> double free.

// ---- Rule of 5: a class owning a raw buffer ----------------------------------
class Buffer {
public:
    explicit Buffer(std::size_t n = 0) : size_(n), data_(n ? new int[n]() : nullptr) {}
    ~Buffer() { delete[] data_; }                                     // 1. destructor

    Buffer(const Buffer &o) : size_(o.size_), data_(o.size_ ? new int[o.size_] : nullptr)   // 2. copy ctor
    {
        std::copy(o.data_, o.data_ + size_, data_);                    // DEEP copy
        ++copies;
    }
    Buffer &operator=(const Buffer &o)                                  // 3. copy assignment
    {
        Buffer tmp(o);                  // copy-and-swap: strong exception safety,
        swap(tmp);                      // self-assignment safe, old data freed by tmp
        return *this;
    }
    Buffer(Buffer &&o) noexcept                                         // 4. move ctor
        : size_(std::exchange(o.size_, 0)), data_(std::exchange(o.data_, nullptr))
    {
        ++moves;                        // steal the pointer: O(1), no allocation
    }
    Buffer &operator=(Buffer &&o) noexcept                              // 5. move assignment
    {
        if (this != &o) {
            delete[] data_;
            size_ = std::exchange(o.size_, 0);
            data_ = std::exchange(o.data_, nullptr);
            ++moves;
        }
        return *this;
    }
    void swap(Buffer &o) noexcept { std::swap(size_, o.size_); std::swap(data_, o.data_); }

    std::size_t size() const { return size_; }
    int &operator[](std::size_t i) { return data_[i]; }
    const int *data() const { return data_; }

    static inline int copies = 0, moves = 0;
private:
    std::size_t size_;
    int *data_;
};

// ---- Rule of 0: same features, zero special members written -------------------
class Buffer0 {
public:
    explicit Buffer0(std::size_t n = 0) : data_(n) {}
    std::size_t size() const { return data_.size(); }
    int &operator[](std::size_t i) { return data_[i]; }
private:
    std::vector<int> data_;          // vector already does deep copy + move + free
    std::string label_ = "frame";
};

// ---- Move-only via rule of 0: unique_ptr member deletes copying automatically -
class Device {
public:
    Device() : regs_(std::make_unique<int[]>(16)) {}
private:
    std::unique_ptr<int[]> regs_;
};

int main()
{
    Buffer a(4);
    a[0] = 7;
    Buffer b = a;                    // copy ctor: independent memory
    b[0] = 99;
    assert(a[0] == 7 && b[0] == 99 && a.data() != b.data());
    assert(Buffer::copies == 1);

    Buffer c = std::move(a);         // move ctor: a is now empty
    assert(c[0] == 7 && a.size() == 0 && a.data() == nullptr);
    assert(Buffer::moves == 1);

    b = c;                           // copy assignment
    b = b;                           // self-assignment must be safe
    assert(b[0] == 7);
    c = Buffer(8);                   // move assignment from a temporary
    assert(c.size() == 8);

    Buffer0 x(3);
    x[1] = 5;
    Buffer0 y = x;                   // generated copy = vector's deep copy
    y[1] = 6;
    Buffer0 z = std::move(y);        // generated move
    assert(x[1] == 5 && z[1] == 6);

    Device d1;
    Device d2 = std::move(d1);       // movable
    // Device d3 = d2;               // error: copy ctor implicitly deleted (unique_ptr)
    (void)d2;

    static_assert(std::is_nothrow_move_constructible<Buffer>::value, "noexcept moves matter (see 07)");
    std::cout << "copies=" << Buffer::copies << " moves=" << Buffer::moves << "\nAll checks passed.\n";
    return 0;
}
