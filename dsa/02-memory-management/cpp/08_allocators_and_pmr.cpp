/*
 * 08_allocators_and_pmr.cpp — give STL containers YOUR memory: custom allocator and std::pmr
 *
 * Concept : Every STL container takes an Allocator. The default,
 *           std::allocator<T>, calls operator new (-> malloc). You can supply
 *           your own: here a minimal arena allocator for std::vector.
 *           C++17 <memory_resource> makes this easy and runtime-swappable:
 *           std::pmr::monotonic_buffer_resource hands out memory from a buffer
 *           you provide (bump allocation, free is a no-op, all released at
 *           once) — ideal for real-time loops: zero malloc per frame.
 * Build   : make run P=dsa/02-memory-management/cpp/08_allocators_and_pmr.cpp
 * Read    : ../README.md  (section "3.16 Allocators and std::pmr")
 */
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory_resource>
#include <new>
#include <string>
#include <vector>

// ---- 1. A classic (C++11-style) custom allocator over a fixed arena ----------
struct Arena {
    alignas(std::max_align_t) unsigned char buf[64 * 1024];
    std::size_t off = 0;
    std::size_t allocations = 0;
    void *take(std::size_t n, std::size_t align)
    {
        std::size_t start = (off + align - 1) & ~(align - 1);
        if (start + n > sizeof buf) throw std::bad_alloc();
        off = start + n;
        ++allocations;
        return buf + start;
    }
};

template <typename T>
struct ArenaAllocator {
    using value_type = T;                      // the only required typedef
    Arena *arena;
    explicit ArenaAllocator(Arena &a) noexcept : arena(&a) {}
    template <typename U>
    ArenaAllocator(const ArenaAllocator<U> &o) noexcept : arena(o.arena) {}   // rebind support
    T *allocate(std::size_t n) { return static_cast<T *>(arena->take(n * sizeof(T), alignof(T))); }
    void deallocate(T *, std::size_t) noexcept {}   // bump arena: individual frees are no-ops
    template <typename U> bool operator==(const ArenaAllocator<U> &o) const { return arena == o.arena; }
    template <typename U> bool operator!=(const ArenaAllocator<U> &o) const { return arena != o.arena; }
};

// ---- 2. A memory_resource that counts what reaches the "real" heap ----------
class CountingResource : public std::pmr::memory_resource {
public:
    std::size_t calls = 0, bytes = 0;
private:
    void *do_allocate(std::size_t n, std::size_t a) override
    {
        ++calls;
        bytes += n;
        return std::pmr::new_delete_resource()->allocate(n, a);
    }
    void do_deallocate(void *p, std::size_t n, std::size_t a) override
    {
        std::pmr::new_delete_resource()->deallocate(p, n, a);
    }
    bool do_is_equal(const std::pmr::memory_resource &o) const noexcept override { return this == &o; }
};

int main()
{
    std::cout << "1. std::vector with a custom ArenaAllocator\n";
    {
        Arena arena;
        std::vector<int, ArenaAllocator<int>> v{ArenaAllocator<int>(arena)};
        for (int i = 0; i < 1000; i++) v.push_back(i);
        std::cout << "   1000 push_backs -> " << arena.allocations << " arena allocations, "
                  << arena.off << " bytes of the arena used (old buffers are never reclaimed)\n";
        assert(v[999] == 999);
        auto *p = reinterpret_cast<unsigned char *>(v.data());
        assert(p >= arena.buf && p < arena.buf + sizeof arena.buf);   // lives in the arena
    }

    std::cout << "2. std::pmr::monotonic_buffer_resource on a stack buffer\n";
    {
        CountingResource upstream;                         // where overflow would go
        alignas(std::max_align_t) unsigned char stack_buf[32 * 1024];
        for (int frame = 0; frame < 100; frame++) {
            // A new resource per frame: everything allocated in it is released
            // at once when it goes out of scope. No per-object free work.
            std::pmr::monotonic_buffer_resource pool(stack_buf, sizeof stack_buf, &upstream);
            std::pmr::vector<std::pmr::string> labels(&pool);   // allocator propagates to strings
            for (int i = 0; i < 20; i++)
                labels.emplace_back("detection-label-number-" + std::to_string(i));
            assert(labels.size() == 20);
            auto *addr = reinterpret_cast<const unsigned char *>(labels.data());
            assert(addr >= stack_buf && addr < stack_buf + sizeof stack_buf);
        }
        std::cout << "   100 frames x 20 strings: upstream (heap) calls = " << upstream.calls << '\n';
        assert(upstream.calls == 0);                        // never touched the real heap
    }

    std::cout << "3. Overflow falls through to the upstream resource\n";
    {
        CountingResource upstream;
        unsigned char tiny[256];
        std::pmr::monotonic_buffer_resource pool(tiny, sizeof tiny, &upstream);
        std::pmr::vector<int> v(&pool);
        for (int i = 0; i < 1000; i++) v.push_back(i);
        std::cout << "   256-byte buffer, 1000 ints: upstream calls = " << upstream.calls
                  << ", bytes = " << upstream.bytes << '\n';
        assert(upstream.calls > 0);
    }   // pool's destructor returns everything to upstream

    std::cout << "4. unsynchronized_pool_resource: size-class pools (like malloc bins) without locks\n";
    {
        CountingResource upstream;
        std::pmr::unsynchronized_pool_resource pools(&upstream);
        {
            std::pmr::vector<std::pmr::string> v(&pools);
            for (int i = 0; i < 200; i++) v.emplace_back(40, 'x');
        }
        std::size_t after_first = upstream.calls;
        {
            std::pmr::vector<std::pmr::string> v(&pools);   // reuses the freed chunks
            for (int i = 0; i < 200; i++) v.emplace_back(40, 'x');
        }
        std::cout << "   upstream calls: first round " << after_first << ", second round +"
                  << upstream.calls - after_first << " (blocks recycled)\n";
        assert(upstream.calls - after_first < after_first);
    }

    std::cout << "All checks passed.\n";
    return 0;
}
