/*
 * 04_object_pool.cpp — typed ObjectPool<T>: slabs of raw storage, placement new,
 *                      and std::unique_ptr with a custom deleter that RETURNS to the pool
 *
 * Concept : The C pool hands out raw bytes. A C++ pool must also run T's
 *           constructor and destructor at the right moments. Raw aligned
 *           storage + placement new does that; a unique_ptr whose deleter
 *           calls pool.release() means users cannot forget to give objects
 *           back — RAII for pooled objects (cf. GstBufferPool recycling a
 *           GstBuffer when its refcount drops to zero).
 * Build   : make run P=dsa/19-system-design-structures/cpp/04_object_pool.cpp
 * Read    : ../README.md  (section "Object pool / slab allocator")
 */
#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <string>
#include <utility>
#include <vector>

template <typename T>
class ObjectPool {
    // A slot is EITHER a live T or a link in the free list — never both.
    union Slot {
        Slot* next;
        alignas(T) unsigned char storage[sizeof(T)];
    };

public:
    explicit ObjectPool(std::size_t per_slab = 64) : per_slab_(per_slab) {}
    ObjectPool(const ObjectPool&) = delete;              // slabs have one owner
    ObjectPool& operator=(const ObjectPool&) = delete;
    ~ObjectPool() { assert(live_ == 0 && "objects outlived their pool"); }

    struct Releaser {
        ObjectPool* pool;
        void operator()(T* p) const { pool->destroy(p); }
    };
    using Handle = std::unique_ptr<T, Releaser>;

    // O(1) amortised: pop a slot, construct T in it.
    template <typename... Args>
    Handle make(Args&&... args) {
        if (!free_) grow();
        Slot* s = free_;
        free_ = s->next;
        T* obj = ::new (static_cast<void*>(s->storage)) T(std::forward<Args>(args)...);
        ++live_;
        return Handle(obj, Releaser{this});
    }

    std::size_t live() const { return live_; }
    std::size_t capacity() const { return slabs_.size() * per_slab_; }

private:
    void destroy(T* p) {
        p->~T();                                         // run destructor explicitly
        Slot* s = reinterpret_cast<Slot*>(p);            // storage is first member of Slot
        s->next = free_;
        free_ = s;
        --live_;
    }

    void grow() {
        slabs_.push_back(std::make_unique<Slot[]>(per_slab_));
        Slot* slab = slabs_.back().get();
        for (std::size_t i = per_slab_; i-- > 0;) {
            slab[i].next = free_;
            free_ = &slab[i];
        }
    }

    std::size_t per_slab_;
    std::size_t live_ = 0;
    Slot* free_ = nullptr;
    std::vector<std::unique_ptr<Slot[]>> slabs_;         // RAII: slabs freed with the pool
};

// An object with a non-trivial constructor/destructor so we can count them.
struct Connection {
    static inline int constructed = 0, destroyed = 0;
    int fd;
    std::string peer;                                    // owns heap memory: leak if ~T skipped
    Connection(int f, std::string p) : fd(f), peer(std::move(p)) { ++constructed; }
    ~Connection() { ++destroyed; }
};

int main() {
    // Declared first => destroyed last, after every Handle below.
    ObjectPool<Connection> pool(16);

    std::cout << "== RAII handles return objects to the pool ==\n";
    {
        auto c1 = pool.make(3, "10.0.0.1:443");
        auto c2 = pool.make(4, "10.0.0.2:22");
        std::cout << "live=" << pool.live() << " c1.peer=" << c1->peer << "\n";
        assert(pool.live() == 2);
        Connection* raw1 = c1.get();
        c1.reset();                                       // back to the pool
        auto c3 = pool.make(5, "10.0.0.3:80");
        assert(c3.get() == raw1);                         // LIFO reuse of the same slot
        std::cout << "c3 reused c1's slot\n";
    }                                                     // c2, c3 released here
    assert(pool.live() == 0);
    assert(Connection::constructed == Connection::destroyed);

    std::cout << "\n== random stress: ctor/dtor balance, no overlap ==\n";
    std::mt19937 rng(11);
    std::vector<ObjectPool<Connection>::Handle> live;
    std::size_t peak = 0;
    for (int op = 0; op < 20000; ++op) {
        if (live.empty() || rng() % 5 < 3) {
            live.push_back(pool.make(op, "peer-" + std::to_string(op)));
        } else {
            std::size_t k = rng() % live.size();
            auto& h = live[k];
            assert(h->peer == "peer-" + std::to_string(h->fd));   // not overwritten
            std::swap(h, live.back());
            live.pop_back();                                 // handle dies => release
        }
        peak = std::max(peak, live.size());
        assert(pool.live() == live.size());
    }
    std::cout << "peak live " << peak << ", pool capacity " << pool.capacity()
              << " (grew only when the free list was empty)\n";
    assert(pool.capacity() < peak + 16);
    live.clear();
    assert(Connection::constructed == Connection::destroyed);
    std::cout << "constructed=" << Connection::constructed
              << " destroyed=" << Connection::destroyed << "\n";
    std::cout << "OK\n";
    return 0;
}
