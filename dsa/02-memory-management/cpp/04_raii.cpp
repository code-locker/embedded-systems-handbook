/*
 * 04_raii.cpp — Resource Acquisition Is Initialisation: the destructor is your free()
 *
 * Concept : Tie every resource (memory, file descriptor, mutex, mapping, GPU
 *           buffer) to the lifetime of a stack object. Constructor acquires,
 *           destructor releases. The compiler guarantees destructors run when
 *           the scope ends — on return, on break, and on exceptions — so
 *           "forgot to free on the error path" disappears as a bug class.
 * Build   : make run P=dsa/02-memory-management/cpp/04_raii.cpp
 * Read    : ../README.md  (section "3.12 RAII")
 */
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

// --- 1. A file-descriptor owner (move-only, like unique_ptr) -----------------
class Fd {
public:
    explicit Fd(int fd = -1) : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(const Fd &) = delete;                         // one owner only
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd &operator=(Fd &&o) noexcept { if (this != &o) { reset(); fd_ = std::exchange(o.fd_, -1); } return *this; }
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }
    void reset() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private:
    int fd_;
};

// --- 2. An mmap owner ---------------------------------------------------------
class Mapping {
public:
    explicit Mapping(std::size_t len)
        : len_(len), p_(::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))
    {
        if (p_ == MAP_FAILED) throw std::runtime_error("mmap failed");   // no object -> no destructor
    }
    ~Mapping() { ::munmap(p_, len_); }
    Mapping(const Mapping &) = delete;
    Mapping &operator=(const Mapping &) = delete;
    unsigned char *data() { return static_cast<unsigned char *>(p_); }
private:
    std::size_t len_;
    void *p_;
};

static bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

// --- 3. Early returns and exceptions cannot leak ------------------------------
static int g_cleanups = 0;
struct Tracker { ~Tracker() { ++g_cleanups; } };

static int process(bool fail_early, bool throw_midway)
{
    Tracker a;                                   // cleaned up on EVERY path below
    auto buf = std::make_unique<int[]>(1024);    // heap memory owned by a stack object
    if (fail_early)
        return -1;                               // a's and buf's destructors run here
    Tracker b;
    if (throw_midway)
        throw std::runtime_error("decode error"); // ...and here, during unwinding
    buf[0] = 1;
    return 0;
}

int main()
{
    int raw_fd = -1;
    {
        Fd fd(::open("/dev/null", O_WRONLY));
        assert(fd);
        raw_fd = fd.get();
        assert(::write(fd.get(), "x", 1) == 1);
        Fd moved = std::move(fd);                // ownership transferred
        assert(!fd && moved.get() == raw_fd);
        assert(fd_is_open(raw_fd));
    }                                            // closed exactly once, here
    assert(!fd_is_open(raw_fd));
    std::cout << "Fd closed automatically at end of scope\n";

    {
        Mapping m(1 << 20);
        m.data()[0] = 42;
        assert(m.data()[0] == 42);
    }                                            // munmap here

    // unique_ptr with a custom deleter wraps any C API "handle + free function".
    {
        std::unique_ptr<FILE, int (*)(FILE *)> f(std::fopen("/dev/null", "w"), &std::fclose);
        assert(f);
        std::fprintf(f.get(), "hello");
    }                                            // fclose here

    // std::lock_guard: the mutex is released even if the body throws.
    std::mutex mtx;
    try {
        std::lock_guard<std::mutex> lock(mtx);
        throw std::runtime_error("oops");
    } catch (const std::exception &) {
    }
    assert(mtx.try_lock());                      // not left locked
    mtx.unlock();

    g_cleanups = 0;
    assert(process(true, false) == -1);
    assert(g_cleanups == 1);
    g_cleanups = 0;
    try { process(false, true); } catch (const std::runtime_error &) {}
    assert(g_cleanups == 2);                     // a and b both destroyed during unwinding
    g_cleanups = 0;
    assert(process(false, false) == 0 && g_cleanups == 2);
    std::cout << "early return / exception / normal path: every destructor ran\n";

    std::cout << "All checks passed.\n";
    return 0;
}
