/*
 * 03_placement_new.cpp — construct objects in memory you already have
 *
 * Concept : `new (addr) T(args)` runs T's constructor at addr and allocates
 *           NOTHING. It separates "get memory" from "make an object". You
 *           must call p->~T() yourself and must never `delete` it. This is how
 *           std::vector, std::optional, memory pools and "objects in a static
 *           buffer / SRAM / shared memory" work.
 * Build   : make run P=dsa/02-memory-management/cpp/03_placement_new.cpp
 * Read    : ../README.md  (section "3.11 Placement new")
 */
#include <cassert>
#include <cstddef>
#include <iostream>
#include <new>
#include <string>
#include <utility>

static int g_live = 0;

struct Sensor {
    std::string name;
    int value;
    Sensor(std::string n, int v) : name(std::move(n)), value(v) { ++g_live; }
    ~Sensor() { --g_live; }
};

// A fixed-capacity vector with NO heap use: storage is inside the object.
// (Embedded-friendly; similar to boost::static_vector / ETL vector.)
template <typename T, std::size_t N>
class StaticVector {
public:
    StaticVector() = default;
    StaticVector(const StaticVector &) = delete;
    StaticVector &operator=(const StaticVector &) = delete;
    ~StaticVector() { clear(); }

    template <typename... Args>
    T &emplace_back(Args &&...args)
    {
        assert(size_ < N && "StaticVector full");
        // Raw storage slot -> placement new constructs the T in place.
        T *p = new (slot(size_)) T(std::forward<Args>(args)...);
        ++size_;
        return *p;
    }
    void pop_back()
    {
        assert(size_ > 0);
        --size_;
        ptr(size_)->~T();            // explicit destructor call: memory stays
    }
    void clear() { while (size_) pop_back(); }
    std::size_t size() const { return size_; }
    T &operator[](std::size_t i) { return *ptr(i); }

private:
    void *slot(std::size_t i) { return storage_ + i * sizeof(T); }
    // std::launder: tells the compiler "an object was created here by placement new".
    T *ptr(std::size_t i) { return std::launder(reinterpret_cast<T *>(storage_ + i * sizeof(T))); }

    alignas(T) unsigned char storage_[N * sizeof(T)];   // raw, correctly aligned bytes
    std::size_t size_ = 0;
};

int main()
{
    // 1. Basic: a buffer with the right size AND alignment.
    alignas(Sensor) unsigned char buf[sizeof(Sensor)];
    /*
     *   buf (stack bytes)          after placement new
     *   +-------------------+      +-------------------+
     *   | ?? ?? ?? ?? ?? ?? |  ->  | Sensor{"imu", 42} |   no allocation happened
     *   +-------------------+      +-------------------+
     */
    Sensor *s = new (buf) Sensor("imu", 42);
    assert(static_cast<void *>(s) == static_cast<void *>(buf));
    assert(s->value == 42 && g_live == 1);
    s->~Sensor();                    // NOT delete s: we never allocated
    assert(g_live == 0);

    // 2. Container built on placement new.
    {
        StaticVector<Sensor, 4> v;
        v.emplace_back("temp", 21);
        v.emplace_back("press", 1013);
        v.emplace_back("hum", 40);
        assert(v.size() == 3 && v[1].name == "press" && g_live == 3);
        v.pop_back();
        assert(g_live == 2);
    }                                // ~StaticVector destroys the rest
    assert(g_live == 0);

    // 3. Re-using one buffer for objects of different types (one at a time).
    alignas(std::max_align_t) unsigned char scratch[64];
    auto *d = new (scratch) double(3.5);
    assert(*d == 3.5);
    auto *str = new (scratch) std::string("now a string");   // double needs no destructor
    assert(str->size() == 12);
    str->~basic_string();

    std::cout << "placement new: all checks passed, no heap allocation for the objects themselves.\n";
    return 0;
}
