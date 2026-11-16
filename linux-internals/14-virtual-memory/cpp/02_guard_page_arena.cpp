/*
 * 02_guard_page_arena.cpp — a page-aligned bump arena fenced by PROT_NONE
 *                           guard pages, a std::pmr::memory_resource on top
 *                           of it, and an "electric fence" allocation whose
 *                           last byte touches a guard page; overflows are
 *                           caught by the MMU (SIGSEGV) and reported
 *
 * Concept : Embedded and real-time code often allocates from a fixed arena
 *           set up at start-up (no malloc in the control loop, no
 *           fragmentation, freed all at once). Put a PROT_NONE page on each
 *           side and an overflow off either end faults on the very first
 *           byte instead of silently corrupting the neighbour:
 *
 *             [ guard ---p ][ usable arena rw-p ...... ][ guard ---p ]
 *                 ^ underflow faults                       ^ overflow faults
 *
 *           Placing ONE allocation so it ends exactly at a guard page
 *           ("electric fence", efence/DUMA, glibc's MALLOC_CHECK_ cousins)
 *           catches an off-by-one at the exact byte, at the cost of at
 *           least two pages per allocation. ASan does this at byte
 *           granularity with shadow memory instead; guard pages need no
 *           special build and also work in release builds on the board.
 * Build   : make run P=linux-internals/14-virtual-memory/cpp/02_guard_page_arena.cpp
 * Read    : ../README.md  (sections "3.7 mprotect, guard pages and
 *           SIGSEGV" and "5. C vs C++")
 */
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory_resource>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fflush(stdout);                                             \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n",     \
                         #cond, __FILE__, __LINE__, errno);                  \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

static size_t page_size() { return static_cast<size_t>(::sysconf(_SC_PAGESIZE)); }
static size_t round_up(size_t n, size_t a) { return (n + a - 1) / a * a; }

// ---- the arena ----
class GuardedArena {
public:
    explicit GuardedArena(size_t capacity)
        : page_(page_size()), cap_(round_up(capacity, page_))
    {
        total_ = cap_ + 2 * page_;
        // Reserve everything PROT_NONE, then open up the middle: the two
        // guards can never be "forgotten".
        void *p = ::mmap(nullptr, total_, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap");
        base_ = static_cast<std::byte *>(p);
        if (::mprotect(base_ + page_, cap_, PROT_READ | PROT_WRITE) != 0) {
            int e = errno;
            ::munmap(base_, total_);
            throw std::system_error(e, std::generic_category(), "mprotect");
        }
    }
    GuardedArena(const GuardedArena &) = delete;
    GuardedArena &operator=(const GuardedArena &) = delete;
    ~GuardedArena() { ::munmap(base_, total_); }

    // O(1) bump allocation. Returns nullptr when full (no exceptions in
    // the hot path; the pmr adapter below turns it into bad_alloc).
    void *allocate(size_t n, size_t align = alignof(std::max_align_t)) noexcept
    {
        uintptr_t cur = reinterpret_cast<uintptr_t>(begin()) + used_;
        uintptr_t aligned = (cur + align - 1) & ~(uintptr_t)(align - 1);
        size_t new_used = aligned - reinterpret_cast<uintptr_t>(begin()) + n;
        if (new_used > cap_)
            return nullptr;
        used_ = new_used;
        return reinterpret_cast<void *>(aligned);
    }
    void reset() noexcept { used_ = 0; } // frees everything at once: O(1)

    std::byte *begin() const noexcept { return base_ + page_; }
    std::byte *end() const noexcept { return base_ + page_ + cap_; }
    std::byte *guard_lo() const noexcept { return base_; }
    std::byte *guard_hi() const noexcept { return end(); }
    size_t capacity() const noexcept { return cap_; }
    size_t used() const noexcept { return used_; }

private:
    size_t page_, cap_, total_ = 0, used_ = 0;
    std::byte *base_ = nullptr;
};

// ---- std::pmr adapter: any pmr container can live in the arena ----
class ArenaResource : public std::pmr::memory_resource {
public:
    explicit ArenaResource(GuardedArena &a) : arena_(a) {}

private:
    void *do_allocate(size_t n, size_t align) override
    {
        void *p = arena_.allocate(n, align);
        if (!p)
            throw std::bad_alloc();
        return p;
    }
    void do_deallocate(void *, size_t, size_t) override {} // freed by reset()
    bool do_is_equal(const memory_resource &o) const noexcept override
    {
        return this == &o;
    }
    GuardedArena &arena_;
};

// ---- an "electric fence" allocation: [ ..pad.. | n bytes ][ guard ] ----
class FencedBuffer {
public:
    explicit FencedBuffer(size_t n) : n_(n)
    {
        size_t pg = page_size();
        len_ = round_up(n, pg) + pg;
        void *p = ::mmap(nullptr, len_, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            throw std::system_error(errno, std::generic_category(), "mmap");
        map_ = static_cast<std::byte *>(p);
        guard_ = map_ + len_ - pg;
        if (::mprotect(guard_, pg, PROT_NONE) != 0) {
            int e = errno;
            ::munmap(map_, len_);
            throw std::system_error(e, std::generic_category(), "mprotect");
        }
        data_ = guard_ - n; // last byte of the buffer is right before guard
    }
    FencedBuffer(const FencedBuffer &) = delete;
    FencedBuffer &operator=(const FencedBuffer &) = delete;
    ~FencedBuffer() { ::munmap(map_, len_); }
    char *data() const noexcept { return reinterpret_cast<char *>(data_); }
    size_t size() const noexcept { return n_; }
    std::byte *guard() const noexcept { return guard_; }

private:
    size_t n_, len_ = 0;
    std::byte *map_ = nullptr, *guard_ = nullptr, *data_ = nullptr;
};

// ---- turning a fault into a return value ----
// The handler jumps back to the sigsetjmp in try_store(). Only trivially
// destructible things may live between the two (siglongjmp runs no
// destructors) — here: a pointer and a char.
static sigjmp_buf g_env;
static void *volatile g_fault_addr;

extern "C" void on_segv(int, siginfo_t *si, void *)
{
    g_fault_addr = si->si_addr;
    siglongjmp(g_env, 1);
}

static bool try_store(volatile char *p, char v)
{
    if (sigsetjmp(g_env, 1) == 0) {
        *p = v;
        return true;
    }
    return false; // faulted; g_fault_addr says where
}

// Write bytes 0, 1, 2, ... from p until a store faults; return the index.
static size_t overflow_until_fault(char *p, size_t limit)
{
    for (size_t i = 0; i < limit; i++)
        if (!try_store(p + i, 'O'))
            return i;
    return limit;
}

int main()
{
    ::alarm(10);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    CHECK(::sigaction(SIGSEGV, &sa, nullptr) == 0);
    const size_t pg = page_size();

    // ---- 1. bump allocation ----
    GuardedArena arena(4 * pg);
    auto *a = static_cast<char *>(arena.allocate(10, 1));
    auto *d = static_cast<double *>(arena.allocate(sizeof(double) * 4,
                                                   alignof(double)));
    auto *v = arena.allocate(100, 64); // cache-line aligned
    CHECK(a && d && v);
    CHECK(reinterpret_cast<uintptr_t>(d) % alignof(double) == 0);
    CHECK(reinterpret_cast<uintptr_t>(v) % 64 == 0);
    std::printf("1) arena %zu KiB at %p: alloc(10,1) @+%td, alloc(32,8) @+%td, "
                "alloc(100,64) @+%td; used %zu bytes\n", arena.capacity() / 1024,
                static_cast<void *>(arena.begin()),
                reinterpret_cast<std::byte *>(a) - arena.begin(),
                reinterpret_cast<std::byte *>(d) - arena.begin(),
                static_cast<std::byte *>(v) - arena.begin(), arena.used());
    CHECK(arena.allocate(arena.capacity(), 1) == nullptr); // does not fit
    std::printf("   alloc(capacity) when partly used -> nullptr (full)\n");

    // ---- 2. overflow and underflow hit the guards ----
    arena.reset();
    char *all = static_cast<char *>(arena.allocate(arena.capacity(), 1));
    CHECK(all == reinterpret_cast<char *>(arena.begin()));
    size_t idx = overflow_until_fault(all, arena.capacity() + pg);
    std::printf("2) writing past the arena: %zu bytes ok, then SIGSEGV at %p "
                "(= upper guard %p)\n", idx, g_fault_addr,
                static_cast<void *>(arena.guard_hi()));
    CHECK(idx == arena.capacity());
    CHECK(g_fault_addr == arena.guard_hi());
    CHECK(!try_store(all - 1, 'U')); // one byte before the arena
    std::printf("   writing 1 byte BEFORE the arena: SIGSEGV at %p (lower "
                "guard page %p..)\n", g_fault_addr,
                static_cast<void *>(arena.guard_lo()));
    CHECK(g_fault_addr == all - 1);

    // ---- 3. pmr containers in the arena ----
    arena.reset();
    {
        ArenaResource res(arena);
        std::pmr::vector<int> vec(&res);
        for (int i = 0; i < 500; i++)
            vec.push_back(i); // every regrowth bump-allocates in the arena
        CHECK(reinterpret_cast<std::byte *>(vec.data()) >= arena.begin() &&
              reinterpret_cast<std::byte *>(vec.data()) < arena.end());
        std::printf("3) std::pmr::vector<int> with 500 ints lives in the "
                    "arena; arena used %zu of %zu bytes (old buffers are "
                    "not reused)\n", arena.used(), arena.capacity());
        bool threw = false;
        try {
            for (int i = 0; i < 100000; i++)
                vec.push_back(i);
        } catch (const std::bad_alloc &) {
            threw = true;
        }
        CHECK(threw);
        std::printf("   growing it further -> std::bad_alloc (arena full), "
                    "never an overflow into the guard\n");
    } // vector destroyed before the arena is reset/destroyed
    arena.reset();

    // ---- 4. electric fence: off-by-one caught at the exact byte ----
    FencedBuffer buf(100);
    std::memset(buf.data(), 'x', buf.size()); // all 100 bytes are fine
    size_t at = overflow_until_fault(buf.data(), 200);
    std::printf("4) FencedBuffer(100): bytes 0..99 written, byte %zu faults "
                "at %p (= guard %p)\n", at, g_fault_addr,
                static_cast<void *>(buf.guard()));
    CHECK(at == 100 && g_fault_addr == buf.guard());
    std::printf("   cost: %zu KiB of address space for 100 bytes; fine for "
                "debugging, not for production\n", 2 * pg / 1024);

    ::signal(SIGSEGV, SIG_DFL);
    std::printf("02_guard_page_arena: all checks passed\n");
    return 0;
}
