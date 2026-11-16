/*
 * 01_shared_memory_raii.cpp — SharedMemory<T>: an RAII class template that
 *                             maps one T in POSIX shared memory, built with
 *                             placement new; std::atomic counters and a
 *                             release/acquire "ready" flag across processes
 *
 * Concept : The C sequence shm_open/ftruncate/mmap/close + munmap/shm_unlink
 *           has five ways to leak. A class owns it: the constructor maps,
 *           the destructor unmaps, and the CREATOR also destroys the object
 *           and unlinks the name. Raw bytes from mmap are not yet a T in
 *           C++ terms — the creator constructs it with PLACEMENT NEW
 *           (`new (addr) T{}`): no allocation, just "run T's constructor at
 *           this address". std::atomic members work across processes only
 *           if they are lock-free (checked with static_assert).
 * Build   : make run P=linux-internals/11-ipc-shared-memory/cpp/01_shared_memory_raii.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fflush(stdout);                                             \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

[[noreturn]] static void throw_errno(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

template <typename T>
class SharedMemory {
    // A T in shared memory is seen by processes with different address
    // spaces: it must be a flat block of bytes. Standard layout keeps the
    // layout predictable; it must not own heap memory (no std::string,
    // std::vector...), which a trait cannot check — that is on you.
    static_assert(std::is_standard_layout_v<T>,
                  "SharedMemory<T>: T must be standard-layout");

public:
    // Create the object and construct a T in it (placement new).
    template <typename... Args>
    static SharedMemory create(const std::string &name, Args &&...args)
    {
        int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
            throw_errno("shm_open(create)");
        if (::ftruncate(fd, sizeof(T)) != 0) {
            int e = errno;
            ::close(fd);
            ::shm_unlink(name.c_str());
            throw std::system_error(e, std::generic_category(), "ftruncate");
        }
        void *mem = map(fd, name, true);
        T *obj = new (mem) T(std::forward<Args>(args)...); // placement new
        return SharedMemory(obj, name, true);
    }

    // Open an object some other process created (and constructed).
    static SharedMemory open(const std::string &name)
    {
        int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0)
            throw_errno("shm_open");
        struct stat sb {};
        if (::fstat(fd, &sb) != 0 || sb.st_size != static_cast<off_t>(sizeof(T))) {
            ::close(fd);
            throw std::runtime_error("shared object has the wrong size");
        }
        return SharedMemory(static_cast<T *>(map(fd, name, false)), name,
                            false);
    }

    SharedMemory(SharedMemory &&o) noexcept
        : obj_(std::exchange(o.obj_, nullptr)), name_(std::move(o.name_)),
          owner_(std::exchange(o.owner_, false))
    {
    }
    SharedMemory(const SharedMemory &) = delete;
    SharedMemory &operator=(const SharedMemory &) = delete;
    SharedMemory &operator=(SharedMemory &&) = delete; // keep it simple

    ~SharedMemory()
    {
        if (!obj_)
            return;
        if (owner_)
            obj_->~T(); // placement new is paired with an explicit dtor call
        ::munmap(obj_, sizeof(T));
        if (owner_)
            ::shm_unlink(name_.c_str());
    }

    T *operator->() const noexcept { return obj_; }
    T &operator*() const noexcept { return *obj_; }
    const std::string &name() const noexcept { return name_; }

private:
    SharedMemory(T *obj, std::string name, bool owner)
        : obj_(obj), name_(std::move(name)), owner_(owner)
    {
    }

    static void *map(int fd, const std::string &name, bool unlink_on_fail)
    {
        void *mem = ::mmap(nullptr, sizeof(T), PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
        int e = errno;
        ::close(fd); // the mapping keeps the object alive
        if (mem == MAP_FAILED) {
            if (unlink_on_fail)
                ::shm_unlink(name.c_str());
            throw std::system_error(e, std::generic_category(), "mmap");
        }
        return mem;
    }

    T *obj_ = nullptr;
    std::string name_;
    bool owner_ = false;
};

// ---- what we share ----
struct Telemetry {
    // Each hot atomic on its own cache line: four processes hammering
    // `hits` must not also slow down whoever polls `ready`.
    alignas(64) std::atomic<uint64_t> hits{0};
    alignas(64) std::atomic<uint32_t> ready{0};
    uint64_t payload[8]{}; // plain data, published via `ready`
};

// Cross-process atomics must be lock-free: a non-lock-free std::atomic is
// implemented with a lock that lives in each process's private memory.
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<Telemetry>);

static void wait_ok(pid_t pid)
{
    int st = 0;
    pid_t r;
    do
        r = ::waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

int main()
{
    ::alarm(10);
    constexpr int kProcs = 4;
    constexpr uint64_t kHits = 100000;
    const std::string name = "/shm_raii_" + std::to_string(::getpid());
    {
        auto shm = SharedMemory<Telemetry>::create(name);
        std::printf("created %s: sizeof(Telemetry) = %zu (hits@+%zu, "
                    "ready@+%zu)\n", name.c_str(), sizeof(Telemetry),
                    offsetof(Telemetry, hits), offsetof(Telemetry, ready));

        // ---- 1. N processes, one atomic counter ----
        pid_t kids[kProcs];
        std::fflush(stdout);
        for (int p = 0; p < kProcs; p++) {
            kids[p] = ::fork();
            CHECK(kids[p] >= 0);
            if (kids[p] == 0) {
                ::alarm(5);
                // A child opens by name like any other program. (_exit at
                // the end: the fork-copy of `shm` is the OWNER and must not
                // run its destructor here — that would unlink the name.)
                auto mine = SharedMemory<Telemetry>::open(name);
                for (uint64_t i = 0; i < kHits; i++)
                    mine->hits.fetch_add(1, std::memory_order_relaxed);
                _exit(0);
            }
        }
        for (pid_t k : kids)
            wait_ok(k);
        uint64_t hits = shm->hits.load();
        std::printf("1) %d processes x %llu fetch_add -> hits = %llu "
                    "(exact)\n", kProcs,
                    static_cast<unsigned long long>(kHits),
                    static_cast<unsigned long long>(hits));
        CHECK(hits == kProcs * kHits);

        // ---- 2. publish plain data with a release/acquire flag ----
        //   child: payload[i] = ...;  ready.store(1, release)
        //   parent: while (!ready.load(acquire)) ;  read payload
        // The release store "carries" the payload writes with it; on the
        // A72 that is an STLR, the acquire load an LDAR.
        std::fflush(stdout);
        pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            ::alarm(5);
            auto mine = SharedMemory<Telemetry>::open(name);
            for (int i = 0; i < 8; i++)
                mine->payload[i] = 1000u + static_cast<uint64_t>(i);
            mine->ready.store(1, std::memory_order_release);
            _exit(0);
        }
        auto start = std::chrono::steady_clock::now();
        while (shm->ready.load(std::memory_order_acquire) == 0) {
            ::sched_yield();
            CHECK(std::chrono::steady_clock::now() - start <
                  std::chrono::seconds(3));
        }
        for (int i = 0; i < 8; i++)
            CHECK(shm->payload[i] == 1000u + static_cast<uint64_t>(i));
        wait_ok(pid);
        std::printf("2) ready flag (release/acquire) published payload "
                    "%llu..%llu correctly\n",
                    static_cast<unsigned long long>(shm->payload[0]),
                    static_cast<unsigned long long>(shm->payload[7]));
    } // shm (owner): ~Telemetry(), munmap, shm_unlink

    errno = 0;
    CHECK(::shm_open(name.c_str(), O_RDONLY, 0) == -1 && errno == ENOENT);
    std::printf("3) owner destroyed: object unlinked from /dev/shm\n");

    std::printf("01_shared_memory_raii: all checks passed\n");
    return 0;
}
