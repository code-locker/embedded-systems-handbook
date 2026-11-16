/*
 * 01_raii_file.cpp — a move-only RAII File that owns one file descriptor,
 *                    with short-I/O-safe read_all/write_all and errors as
 *                    std::system_error; then the same job with std::fstream
 *
 * Concept : An fd is a resource exactly like heap memory: it must be
 *           released once, on every path (early return, exception). A
 *           class that closes it in its destructor makes leaks impossible
 *           (the bug in c/08_fd_leak_bug.c cannot be written). Copying an
 *           fd owner would mean two closes -> copy is deleted; moving
 *           transfers ownership and leaves the source empty (fd = -1), like
 *           std::unique_ptr. std::fstream is the portable, buffered layer
 *           (like FILE*): convenient, but it hides the fd, so no fsync,
 *           no O_CLOEXEC/O_EXCL control, no ioctl, no locks.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/cpp/01_raii_file.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
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

[[noreturn]] static void throw_errno(const std::string &what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

class File {
public:
    File() noexcept = default; // empty: owns nothing

    // Factory instead of a constructor taking flags: the name says what it
    // does, and O_CLOEXEC is added for you (the safe default).
    static File open(const std::string &path, int flags, mode_t mode = 0644)
    {
        int fd = ::open(path.c_str(), flags | O_CLOEXEC, mode);
        if (fd < 0)
            throw_errno("open(" + path + ")");
        return File(fd);
    }

    // Take ownership of an fd that came from elsewhere (pipe, socket...).
    explicit File(int fd) noexcept : fd_(fd) {}

    File(const File &) = delete;            // two owners = double close
    File &operator=(const File &) = delete;

    File(File &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    File &operator=(File &&o) noexcept
    {
        if (this != &o) {
            reset();                          // close what we had
            fd_ = std::exchange(o.fd_, -1);   // steal theirs
        }
        return *this;
    }

    ~File() { reset(); }

    // close() can fail (EIO on NFS / a full device for delayed writes).
    // The destructor must not throw, so offer an explicit close() for
    // callers who care, and let the destructor ignore the error.
    void close()
    {
        int fd = std::exchange(fd_, -1);
        if (fd >= 0 && ::close(fd) != 0)
            throw_errno("close");
    }
    void reset() noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }
    [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    // Loops over short writes and EINTR (see c/02_open_read_write.c).
    void write_all(const void *buf, size_t n)
    {
        const char *p = static_cast<const char *>(buf);
        while (n > 0) {
            ssize_t w = ::write(fd_, p, n);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                throw_errno("write");
            }
            p += w;
            n -= static_cast<size_t>(w);
        }
    }
    void write_all(const std::string &s) { write_all(s.data(), s.size()); }

    // Reads until EOF.
    std::string read_all()
    {
        std::string out;
        char buf[4096];
        for (;;) {
            ssize_t r = ::read(fd_, buf, sizeof buf);
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                throw_errno("read");
            }
            if (r == 0)
                return out;
            out.append(buf, static_cast<size_t>(r));
        }
    }

    off_t seek(off_t off, int whence)
    {
        off_t r = ::lseek(fd_, off, whence);
        if (r < 0)
            throw_errno("lseek");
        return r;
    }
    off_t size() const
    {
        struct stat sb {};
        if (::fstat(fd_, &sb) != 0)
            throw_errno("fstat");
        return sb.st_size;
    }
    void sync()
    {
        if (::fsync(fd_) != 0)
            throw_errno("fsync");
    }

private:
    int fd_ = -1;
};

static_assert(!std::is_copy_constructible_v<File>);
static_assert(std::is_nothrow_move_constructible_v<File>);

static bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

int main()
{
    ::alarm(10);
    char tmpl[64];
    std::snprintf(tmpl, sizeof tmpl, "/tmp/raii_%d_XXXXXX",
                  static_cast<int>(::getpid()));
    int tfd = ::mkstemp(tmpl);
    CHECK(tfd >= 0);
    ::close(tfd);
    const std::string path = tmpl;

    // ---- 1. scope = lifetime ----
    int raw = -1;
    {
        File f = File::open(path, O_WRONLY | O_TRUNC);
        raw = f.get();
        f.write_all("line one\nline two\n");
        f.sync();
        CHECK(f.size() == 18);
        CHECK(fd_is_open(raw));
    } // ~File closes here
    CHECK(!fd_is_open(raw));
    std::printf("1) File went out of scope -> fd %d is closed "
                "(fcntl says EBADF)\n", raw);

    // ---- 2. move = transfer ownership ----
    File a = File::open(path, O_RDONLY);
    int afd = a.get();
    File b = std::move(a);                 // a is now empty
    CHECK(!a && b && b.get() == afd);
    std::vector<File> pool;
    pool.push_back(std::move(b));          // vector growth moves, never copies
    pool.push_back(File::open(path, O_RDONLY));
    pool.push_back(File::open(path, O_RDONLY));
    std::string text = pool[0].read_all();
    CHECK(text == "line one\nline two\n");
    std::printf("2) moved fd %d: a -> b -> vector; 3 Files in the vector, "
                "read %zu bytes from the first\n", afd, text.size());
    int last = pool.back().get();
    pool.clear();                          // closes all three
    CHECK(!fd_is_open(afd) && !fd_is_open(last));

    // ---- 3. errors become exceptions with errno inside ----
    try {
        File::open("/nonexistent/x", O_RDONLY);
        CHECK(false);
    } catch (const std::system_error &e) {
        std::printf("3) open failed -> std::system_error: \"%s\" (code %d)\n",
                    e.what(), e.code().value());
        CHECK(e.code() == std::errc::no_such_file_or_directory);
    }
    // ... and an exception cannot leak an fd: f is destroyed while the
    // exception propagates (stack unwinding).
    int leaked_fd = -1;
    try {
        File f = File::open(path, O_RDONLY);
        leaked_fd = f.get();
        throw std::runtime_error("parser failed");
    } catch (const std::runtime_error &) {
    }
    CHECK(!fd_is_open(leaked_fd));
    std::printf("   exception thrown while a File was open: fd %d still got "
                "closed\n", leaked_fd);

    // ---- 4. the same with std::fstream ----
    {
        std::ofstream out(path, std::ios::trunc); // buffered, like FILE*
        CHECK(out.is_open());
        for (int i = 0; i < 3; i++)
            out << "sample " << i << '\n';
    } // flushed + closed by the destructor (also RAII)
    std::ifstream in(path);
    std::string line;
    int lines = 0;
    while (std::getline(in, line))
        lines++;
    CHECK(lines == 3);
    std::printf("4) std::ofstream wrote 3 lines, std::ifstream read %d back\n",
                lines);

    // fstream reports errors with state bits, not exceptions, by default.
    std::ifstream missing("/nonexistent/x");
    CHECK(!missing.is_open() && missing.fail());
    std::printf("   std::ifstream on a missing file: no exception, just "
                "fail() == true (errno is not reliably set)\n");

    // Mixing: the raw File sees what fstream wrote (same file, same page
    // cache) — but only AFTER fstream flushed its buffer.
    std::string all = File::open(path, O_RDONLY).read_all();
    CHECK(all == "sample 0\nsample 1\nsample 2\n");
    std::printf("   raw File::read_all() sees the fstream output (%zu "
                "bytes)\n", all.size());

    CHECK(::unlink(path.c_str()) == 0);
    std::printf("01_raii_file: all checks passed\n");
    return 0;
}
