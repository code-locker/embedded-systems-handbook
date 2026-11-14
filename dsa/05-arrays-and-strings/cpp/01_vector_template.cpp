/*
 * 01_vector_template.cpp — a mini std::vector<T>: RAII, placement new, moves
 *
 * Concept : The C IntVec from c/02 generalised to any T. The key differences
 *           from C: memory is RAW (operator new) and objects are created in
 *           it with placement new and destroyed with explicit ~T(), because
 *           capacity > size means "memory exists but no object lives there".
 *           Growth MOVES elements (std::move_if_noexcept), the rule of five
 *           gives correct copy/move, and iterators are plain T* pointers.
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/01_vector_template.cpp
 * Read    : ../README.md  (section "Dynamic array" and "C vs C++")
 */
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <numeric>
#include <stdexcept>
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

/*
 *   Vector<std::string> v;   (size 3, capacity 4)
 *   +-----------+        raw storage from ::operator new
 *   | data_  ------->  [ string | string | string | (raw bytes) ]
 *   | size_ = 3 |        constructed objects       not an object yet
 *   | cap_  = 4 |
 *   +-----------+
 *   push_back: placement-new into slot 3   (no object to assign to!)
 *   pop_back : call ~string() on slot 2, slot becomes raw again
 */
template <typename T>
class Vector {
public:
    using iterator = T *;
    using const_iterator = const T *;

    Vector() = default;

    Vector(std::initializer_list<T> il)
    {
        reserve(il.size());
        for (const T &x : il)
            push_back(x);
    }

    // Copy: O(n). Allocate exactly size() and copy-construct each element.
    Vector(const Vector &other)
    {
        reserve(other.size_);
        for (const T &x : other)
            push_back(x);
    }

    // Move: O(1). Steal the buffer, leave `other` empty but valid.
    //   before:  this{null,0,0}   other{p,3,4}
    //   after :  this{p,3,4}      other{null,0,0}
    Vector(Vector &&other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          cap_(std::exchange(other.cap_, 0))
    {
    }

    // Copy-and-swap: one assignment operator handles copy AND move, and
    // gives the strong exception guarantee (if the copy throws, *this is
    // untouched).
    Vector &operator=(Vector other) noexcept
    {
        swap(other);
        return *this;
    }

    ~Vector()
    {
        clear();                          // destroy live objects
        ::operator delete(data_);         // then release raw memory
    }

    void swap(Vector &o) noexcept
    {
        std::swap(data_, o.data_);
        std::swap(size_, o.size_);
        std::swap(cap_, o.cap_);
    }

    // Amortised O(1).
    void push_back(const T &x) { emplace_back(x); }
    void push_back(T &&x) { emplace_back(std::move(x)); }

    template <typename... Args>
    T &emplace_back(Args &&...args)
    {
        if (size_ == cap_)
            grow(cap_ ? cap_ * 2 : 1);
        T *slot = data_ + size_;
        ::new (static_cast<void *>(slot)) T(std::forward<Args>(args)...);
        ++size_;
        return *slot;
    }

    // O(1): destroy the last object; memory stays (capacity unchanged).
    void pop_back()
    {
        --size_;
        data_[size_].~T();
    }

    // O(n - pos): shift tail left by move-assignment, destroy last.
    iterator erase(iterator pos)
    {
        std::move(pos + 1, end(), pos);
        pop_back();
        return pos;
    }

    void clear() noexcept
    {
        while (size_)
            pop_back();
    }

    // O(n) when it reallocates.
    void reserve(std::size_t n)
    {
        if (n > cap_)
            grow(n);
    }

    T &operator[](std::size_t i) { return data_[i]; }             // unchecked
    const T &operator[](std::size_t i) const { return data_[i]; }
    T &at(std::size_t i)                                          // checked
    {
        if (i >= size_)
            throw std::out_of_range("Vector::at");
        return data_[i];
    }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }
    iterator begin() { return data_; }
    iterator end() { return data_ + size_; }
    const_iterator begin() const { return data_; }
    const_iterator end() const { return data_ + size_; }

private:
    void grow(std::size_t new_cap)
    {
        // 1. raw memory (no constructors run)
        T *fresh = static_cast<T *>(::operator new(new_cap * sizeof(T)));
        std::size_t i = 0;
        try {
            // 2. move (or copy, if T's move might throw) each element over.
            //    move_if_noexcept is why you should mark move ctors noexcept:
            //    otherwise std::vector COPIES on growth to stay exception-safe.
            for (; i < size_; ++i)
                ::new (static_cast<void *>(fresh + i))
                    T(std::move_if_noexcept(data_[i]));
        } catch (...) {
            for (std::size_t j = 0; j < i; ++j)
                fresh[j].~T();
            ::operator delete(fresh);
            throw;
        }
        // 3. destroy the moved-from originals and free the old block
        for (std::size_t j = 0; j < size_; ++j)
            data_[j].~T();
        ::operator delete(data_);
        data_ = fresh;
        cap_ = new_cap;
        ++reallocations;
    }

    T *data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t cap_ = 0;

public:
    std::size_t reallocations = 0; // instrumentation for the lesson
};

// A type that counts constructions/destructions to prove we never leak or
// double-destroy objects, and counts copies vs moves.
struct Tracked {
    static inline int alive = 0, copies = 0, moves = 0;
    int v;
    explicit Tracked(int x) : v(x) { ++alive; }
    Tracked(const Tracked &o) : v(o.v) { ++alive; ++copies; }
    Tracked(Tracked &&o) noexcept : v(o.v) { ++alive; ++moves; }
    Tracked &operator=(const Tracked &o) { v = o.v; ++copies; return *this; }
    Tracked &operator=(Tracked &&o) noexcept { v = o.v; ++moves; return *this; }
    ~Tracked() { --alive; }
};

int main()
{
    std::puts("== 1. push_back / growth ==");
    {
        Vector<int> v;
        for (int i = 0; i < 1000; ++i)
            v.push_back(i);
        CHECK(v.size() == 1000 && v.capacity() == 1024);
        CHECK(std::accumulate(v.begin(), v.end(), 0L) == 999L * 1000 / 2);
        std::printf("  1000 pushes -> capacity %zu after %zu reallocations\n",
                    v.capacity(), v.reallocations);
        CHECK(v.reallocations == 11); // 1,2,4,...,1024
        // Raw pointers ARE iterators, so <algorithm> just works:
        std::reverse(v.begin(), v.end());
        CHECK(v[0] == 999);
    }

    std::puts("\n== 2. non-trivial T: std::string ==");
    {
        Vector<std::string> v{"beagle", "bone", "ai64"};
        v.emplace_back(5, 'x'); // constructs "xxxxx" in place, no temporary
        CHECK(v.size() == 4 && v[3] == "xxxxx");
        Vector<std::string> copy = v;      // deep copy
        copy[0] = "changed";
        CHECK(v[0] == "beagle");           // original untouched
        Vector<std::string> moved = std::move(v);
        CHECK(moved.size() == 4 && v.empty()); // NOLINT: v valid-but-empty
        moved.erase(moved.begin() + 1);
        CHECK(moved.size() == 3 && moved[1] == "ai64");
        bool threw = false;
        try {
            moved.at(10);
        } catch (const std::out_of_range &) {
            threw = true;
        }
        CHECK(threw);
        std::puts("  copy is deep, move steals, at() bounds-checks");
    }

    std::puts("\n== 3. object lifetimes are exact ==");
    {
        Vector<Tracked> v;
        for (int i = 0; i < 100; ++i)
            v.emplace_back(i);
        CHECK(Tracked::alive == 100);
        std::printf("  100 emplace_backs: %d moves during growth, %d copies\n",
                    Tracked::moves, Tracked::copies);
        CHECK(Tracked::copies == 0); // noexcept move => growth moves, never copies
        v.pop_back();
        CHECK(Tracked::alive == 99);
    }
    CHECK(Tracked::alive == 0); // destructor destroyed every element
    std::puts("  every constructed object destroyed exactly once");

    std::puts("\n== 4. same behaviour as std::vector ==");
    {
        Vector<int> mine;
        std::vector<int> ref;
        std::srand(7);
        for (int step = 0; step < 5000; ++step) {
            int op = std::rand() % 4;
            if (op < 3 || ref.empty()) {
                int x = std::rand();
                mine.push_back(x);
                ref.push_back(x);
            } else if (op == 3 && std::rand() % 2) {
                mine.pop_back();
                ref.pop_back();
            } else {
                std::size_t at = static_cast<std::size_t>(std::rand()) % ref.size();
                mine.erase(mine.begin() + at);
                ref.erase(ref.begin() + static_cast<std::ptrdiff_t>(at));
            }
            CHECK(mine.size() == ref.size());
        }
        CHECK(std::equal(mine.begin(), mine.end(), ref.begin(), ref.end()));
        std::puts("  5000 random push/pop/erase operations match std::vector");
    }

    std::puts("\nall checks passed");
    return 0;
}
