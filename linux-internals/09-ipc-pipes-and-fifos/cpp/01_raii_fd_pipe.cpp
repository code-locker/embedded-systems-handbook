/*
 * 01_raii_fd_pipe.cpp — RAII wrappers for file descriptors and pipes:
 *                       a move-only Fd and a Pipe made of two Fds
 *
 * Concept : Every early return, exception or forgotten close() in C leaks a
 *           file descriptor — and with pipes a leaked WRITE end is worse
 *           than a leak: the reader never sees EOF. Put the fd in an object
 *           whose destructor closes it, make it move-only (two owners would
 *           close twice), and the problem disappears. This is what
 *           std::fstream does for FILE-like handles, unique_ptr for memory,
 *           and what every serious C++ code base has for fds.
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/cpp/01_raii_fd_pipe.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <system_error>
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

// ---------------------------------------------------------------------------
// Fd: owns one file descriptor. Move-only, like std::unique_ptr.
//
//   Fd a{open(...)};      a owns fd 5
//   Fd b = std::move(a);  b owns fd 5, a owns nothing (-1)
//   } // scope ends       b's destructor closes fd 5, a's does nothing
// ---------------------------------------------------------------------------
class Fd {
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(const Fd &) = delete;            // copying would mean double close()
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&o) noexcept : fd_(o.release()) {}
    Fd &operator=(Fd &&o) noexcept
    {
        if (this != &o)
            reset(o.release());
        return *this;
    }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    // Give up ownership without closing (e.g. after dup2 onto fd 1).
    int release() noexcept { return std::exchange(fd_, -1); }

    // Close the current fd (if any), then own `fd`. close() errors are
    // ignored on purpose: on Linux the fd is gone even if close() fails, and
    // retrying could close somebody else's newly opened fd.
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

// Throwing helpers: system errors become std::system_error with errno.
[[noreturn]] static void throw_errno(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

static void write_all(const Fd &fd, const void *buf, size_t len)
{
    auto *p = static_cast<const char *>(buf);
    while (len > 0) {
        ssize_t n = ::write(fd.get(), p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw_errno("write");
        }
        p += n;
        len -= static_cast<size_t>(n);
    }
}

static std::string read_to_eof(const Fd &fd)
{
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd.get(), buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            throw_errno("read");
        }
        if (n == 0)
            return out;
        out.append(buf, static_cast<size_t>(n));
    }
}

// ---------------------------------------------------------------------------
// Pipe: two Fds. O_CLOEXEC by default so exec'd programs never inherit
// ends they do not know about (a classic source of "reader never gets EOF").
// ---------------------------------------------------------------------------
struct Pipe {
    Fd read_end;
    Fd write_end;

    static Pipe create(int flags = O_CLOEXEC)
    {
        int fds[2];
        if (::pipe2(fds, flags) != 0)
            throw_errno("pipe2");
        return Pipe{Fd{fds[0]}, Fd{fds[1]}};
    }
};

// Count open fds of this process via /proc/self/fd (the directory fd itself
// is counted too, consistently on every call).
static int count_open_fds()
{
    DIR *d = ::opendir("/proc/self/fd");
    CHECK(d != nullptr);
    int n = 0;
    while (struct dirent *e = ::readdir(d))
        if (e->d_name[0] != '.')
            n++;
    ::closedir(d);
    return n;
}

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
    const int fds_at_start = count_open_fds();
    std::printf("open fds at start: %d\n", fds_at_start);

    // ---- 1. child -> parent through a Pipe, ends closed by scope ----
    {
        Pipe p = Pipe::create();
        std::printf("1) pipe: read fd %d, write fd %d\n", p.read_end.get(),
                    p.write_end.get());
        std::fflush(stdout);
        pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            ::alarm(5);
            p.read_end.reset(); // the child does not read
            const std::string msg = "greetings from the child\n";
            write_all(p.write_end, msg.data(), msg.size());
            // _exit: no destructors run in the child, but the kernel closes
            // every fd on process exit anyway.
            _exit(0);
        }
        p.write_end.reset(); // THE line that makes EOF possible
        std::string got = read_to_eof(p.read_end);
        wait_ok(pid);
        std::printf("   parent read: %s", got.c_str());
        CHECK(got == "greetings from the child\n");
    } // p.read_end closed here
    CHECK(count_open_fds() == fds_at_start);
    std::printf("   after the scope: open fds back to %d (nothing leaked)\n",
                fds_at_start);

    // ---- 2. exception in the middle: still no leak ----
    try {
        Pipe a = Pipe::create();
        Pipe b = Pipe::create();
        CHECK(count_open_fds() == fds_at_start + 4);
        throw std::runtime_error("something failed half-way");
    } catch (const std::exception &e) {
        std::printf("2) caught \"%s\"; 4 fds were open inside the try\n",
                    e.what());
    }
    CHECK(count_open_fds() == fds_at_start);
    std::printf("   after unwinding: open fds back to %d\n", fds_at_start);

    // ---- 3. move semantics: ownership transfer, single close ----
    Pipe p = Pipe::create();
    int raw = p.read_end.get();
    Fd moved = std::move(p.read_end);
    CHECK(!p.read_end);          // moved-from is empty
    CHECK(moved.get() == raw);   // new owner has it
    moved.reset();               // closes once
    CHECK(::fcntl(raw, F_GETFD) == -1 && errno == EBADF);
    std::printf("3) moved fd %d to a new owner; closed exactly once\n", raw);

    // ---- 4. errors are exceptions with errno inside ----
    struct sigaction sa {};
    sa.sa_handler = SIG_IGN;
    CHECK(::sigaction(SIGPIPE, &sa, nullptr) == 0);
    try {
        write_all(p.write_end, "x", 1); // no reader left -> EPIPE
        CHECK(false);
    } catch (const std::system_error &e) {
        std::printf("4) write to reader-less pipe threw: %s\n", e.what());
        CHECK(e.code() == std::errc::broken_pipe);
    }
    p.write_end.reset();
    CHECK(count_open_fds() == fds_at_start);

    std::printf("01_raii_fd_pipe: all checks passed\n");
    return 0;
}
