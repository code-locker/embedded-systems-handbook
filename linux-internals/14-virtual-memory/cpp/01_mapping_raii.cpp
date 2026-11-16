/*
 * 01_mapping_raii.cpp — Mapping: a move-only RAII owner of one mmap()
 *                       region (anonymous or file-backed) with typed
 *                       access, protect/advise/sync, and proof via mincore()
 *                       that the destructor really unmapped it
 *
 * Concept : A mapping is a resource like an fd or heap block: mmap() gives
 *           it, munmap() must take it back exactly once, with the same
 *           length. The C bug in ../c/07_use_after_munmap_bug.c (a pointer
 *           kept after munmap) and the leak (forgetting munmap on an error
 *           path) both disappear when one object owns (addr, len): the
 *           destructor unmaps, copy is deleted, a move leaves the source
 *           empty. The file descriptor is closed right after mmap(): the
 *           mapping keeps the file alive on its own.
 * Build   : make run P=linux-internals/14-virtual-memory/cpp/01_mapping_raii.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <system_error>
#include <type_traits>
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

[[noreturn]] static void throw_errno(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

class Mapping {
public:
    Mapping() noexcept = default;

    static Mapping anonymous(size_t len, int prot = PROT_READ | PROT_WRITE,
                             int flags = MAP_PRIVATE)
    {
        void *p = ::mmap(nullptr, len, prot, flags | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED)
            throw_errno("mmap(anonymous)");
        return Mapping(p, len);
    }

    // Map `len` bytes of a file (grown with ftruncate if needed).
    static Mapping file(const std::string &path, size_t len, bool shared)
    {
        int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0)
            throw_errno("open");
        if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
            int e = errno;
            ::close(fd);
            throw std::system_error(e, std::generic_category(), "ftruncate");
        }
        void *p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE,
                         shared ? MAP_SHARED : MAP_PRIVATE, fd, 0);
        int e = errno;
        ::close(fd); // the mapping holds its own reference to the file
        if (p == MAP_FAILED)
            throw std::system_error(e, std::generic_category(), "mmap(file)");
        return Mapping(p, len);
    }

    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    Mapping(Mapping &&o) noexcept
        : addr_(std::exchange(o.addr_, nullptr)), len_(std::exchange(o.len_, 0))
    {
    }
    Mapping &operator=(Mapping &&o) noexcept
    {
        if (this != &o) {
            reset();
            addr_ = std::exchange(o.addr_, nullptr);
            len_ = std::exchange(o.len_, 0);
        }
        return *this;
    }
    ~Mapping() { reset(); }

    void reset() noexcept
    {
        if (addr_)
            ::munmap(addr_, len_); // same length as mmap: always correct
        addr_ = nullptr;
        len_ = 0;
    }

    std::byte *data() const noexcept { return static_cast<std::byte *>(addr_); }
    size_t size() const noexcept { return len_; }
    explicit operator bool() const noexcept { return addr_ != nullptr; }

    // Typed view. mmap memory is page-aligned, so any alignment <= page
    // holds; only trivially copyable types make sense in raw pages.
    template <typename T>
    T *as() const
    {
        static_assert(std::is_trivially_copyable_v<T>,
                      "only trivially copyable types belong in raw pages");
        return reinterpret_cast<T *>(addr_);
    }
    template <typename T>
    size_t count() const noexcept { return len_ / sizeof(T); }

    void protect(int prot)
    {
        if (::mprotect(addr_, len_, prot) != 0)
            throw_errno("mprotect");
    }
    void advise(int advice)
    {
        if (::madvise(addr_, len_, advice) != 0)
            throw_errno("madvise");
    }
    void sync()
    {
        if (::msync(addr_, len_, MS_SYNC) != 0)
            throw_errno("msync");
    }

private:
    Mapping(void *a, size_t l) noexcept : addr_(a), len_(l) {}
    void *addr_ = nullptr;
    size_t len_ = 0;
};

static_assert(!std::is_copy_constructible_v<Mapping>);
static_assert(std::is_nothrow_move_constructible_v<Mapping>);

// mincore() fails with ENOMEM if any page of the range is not mapped:
// a crash-free way to ask "is this address range still mapped?".
static bool is_mapped(const void *addr, size_t len)
{
    std::vector<unsigned char> vec(
        (len + static_cast<size_t>(::sysconf(_SC_PAGESIZE)) - 1) /
        static_cast<size_t>(::sysconf(_SC_PAGESIZE)));
    return ::mincore(const_cast<void *>(addr), len, vec.data()) == 0;
}

int main()
{
    ::alarm(10);
    const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));

    // ---- 1. anonymous mapping, typed access, destructor unmaps ----
    void *where = nullptr;
    {
        auto m = Mapping::anonymous(16 * page);
        auto *v = m.as<uint32_t>();
        std::iota(v, v + m.count<uint32_t>(), 0u); // pages fault in here
        uint64_t sum = std::accumulate(v, v + m.count<uint32_t>(), 0ull);
        const uint64_t n = m.count<uint32_t>();
        CHECK(sum == n * (n - 1) / 2);
        where = m.data();
        CHECK(is_mapped(where, m.size()));
        std::printf("1) Mapping::anonymous(64 KiB) at %p: %llu uint32 "
                    "values, sum ok\n", where,
                    static_cast<unsigned long long>(n));
    }
    CHECK(!is_mapped(where, 16 * page));
    std::printf("   after the scope: mincore() says %p is no longer mapped\n",
                where);

    // ---- 2. move: ownership travels, the source becomes empty ----
    std::vector<Mapping> pool;
    auto a = Mapping::anonymous(page);
    void *aaddr = a.data();
    pool.push_back(std::move(a));
    for (int i = 0; i < 3; i++)
        pool.push_back(Mapping::anonymous(page)); // vector regrows: moves
    CHECK(!a && pool[0].data() == aaddr);
    std::printf("2) moved a mapping into a vector of %zu; the moved-from "
                "object is empty (data() == nullptr)\n", pool.size());
    void *last = pool.back().data();
    pool.clear(); // four munmaps
    CHECK(!is_mapped(aaddr, page) && !is_mapped(last, page));

    // ---- 3. file mapping: shared writes reach the file ----
    char tmpl[64];
    std::snprintf(tmpl, sizeof tmpl, "/tmp/mapping_%d_XXXXXX",
                  static_cast<int>(::getpid()));
    int tfd = ::mkstemp(tmpl);
    CHECK(tfd >= 0);
    ::close(tfd);
    {
        auto f = Mapping::file(tmpl, page, /*shared=*/true);
        const char msg[] = "written through a Mapping";
        std::memcpy(f.data(), msg, sizeof msg);
        f.sync(); // durable (see lesson 00, fsync)
        char back[sizeof msg] = {};
        int fd = ::open(tmpl, O_RDONLY | O_CLOEXEC);
        CHECK(fd >= 0 && ::read(fd, back, sizeof back) ==
                             static_cast<ssize_t>(sizeof back));
        ::close(fd);
        CHECK(std::strcmp(back, msg) == 0);
        std::printf("3) Mapping::file(shared): read() of the file returns "
                    "\"%s\"\n", back);

        auto p = Mapping::file(tmpl, page, /*shared=*/false);
        std::memcpy(p.data(), "PRIVATE", 7); // copy-on-write, file untouched
        CHECK(std::memcmp(f.data(), msg, 7) == 0);
        std::printf("   Mapping::file(private) write: the shared view still "
                    "shows \"%.7s\"\n",
                    reinterpret_cast<const char *>(f.data()));
    }
    CHECK(::unlink(tmpl) == 0);

    // ---- 4. errors are exceptions ----
    try {
        auto bad = Mapping::anonymous(0); // length 0 -> EINVAL
        CHECK(false);
    } catch (const std::system_error &e) {
        std::printf("4) Mapping::anonymous(0) -> std::system_error: %s\n",
                    e.what());
        CHECK(e.code() == std::errc::invalid_argument);
    }

    std::printf("01_mapping_raii: all checks passed\n");
    return 0;
}
