/*
 * 01_mapped_file.cpp — MappedFile: an RAII, move-only read-only file mapping
 *                      with std::string_view / span-like access, next to a
 *                      BufferedReader that does the same job with read()
 *
 * Concept : In C, mmap needs open + fstat + mmap + close + munmap, a size-0
 *           special case (mmap of length 0 is EINVAL) and cleanup on every
 *           error path. A class owns the mapping: constructor maps, the
 *           destructor unmaps, moves transfer ownership, copies are deleted.
 *           The mapped bytes are exposed as std::string_view (for text) and a
 *           tiny span-like ByteView (std::span is C++20), so parsing code
 *           works on the file in place: no copy, no buffer management.
 *           BufferedReader does the same with read() into one reused buffer
 *           and has to handle a line split across two reads — the
 *           bookkeeping mmap saves you. Both parse the same generated file;
 *           results must match.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/cpp/01_mapped_file.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

// A minimal read-only span of bytes (std::span<const std::byte> in C++20).
struct ByteView {
    const std::byte *data = nullptr;
    std::size_t size = 0;
    const std::byte *begin() const { return data; }
    const std::byte *end() const { return data + size; }
};

class MappedFile {
public:
    explicit MappedFile(const std::string &path, bool sequential = true)
    {
        int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            throw_errno("open");
        struct stat st {};
        if (::fstat(fd, &st) != 0) {
            int e = errno;
            ::close(fd);
            throw std::system_error(e, std::generic_category(), "fstat");
        }
        size_ = static_cast<std::size_t>(st.st_size);
        if (size_ > 0) { // mmap(len = 0) is EINVAL: an empty file is an empty view
            void *p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (p == MAP_FAILED) {
                int e = errno;
                ::close(fd);
                throw std::system_error(e, std::generic_category(), "mmap");
            }
            data_ = static_cast<const char *>(p);
            if (sequential)
                ::madvise(p, size_, MADV_SEQUENTIAL); // bigger read-ahead, drop behind
        }
        ::close(fd); // the mapping keeps its own reference to the file
    }
    ~MappedFile()
    {
        if (data_)
            ::munmap(const_cast<char *>(data_), size_);
    }
    MappedFile(MappedFile &&o) noexcept
        : data_(std::exchange(o.data_, nullptr)), size_(std::exchange(o.size_, 0))
    {
    }
    MappedFile &operator=(MappedFile &&o) noexcept
    {
        if (this != &o) {
            if (data_)
                ::munmap(const_cast<char *>(data_), size_);
            data_ = std::exchange(o.data_, nullptr);
            size_ = std::exchange(o.size_, 0);
        }
        return *this;
    }
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;

    // Views are valid only while *this lives (like string_view into a string).
    std::string_view text() const { return {data_ ? data_ : "", size_}; }
    ByteView bytes() const { return {reinterpret_cast<const std::byte *>(data_), size_}; }
    std::size_t size() const { return size_; }

private:
    const char *data_ = nullptr;
    std::size_t size_ = 0;
};

// The read() version: one reused buffer; lines may straddle two reads, so
// the tail of each chunk is carried over to the next.
class BufferedReader {
public:
    explicit BufferedReader(const std::string &path, std::size_t bufsize = 64 * 1024)
        : fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)), buf_(bufsize)
    {
        if (fd_ < 0)
            throw_errno("open");
    }
    ~BufferedReader()
    {
        if (fd_ >= 0)
            ::close(fd_);
    }
    BufferedReader(const BufferedReader &) = delete;
    BufferedReader &operator=(const BufferedReader &) = delete;

    // Calls f(std::string_view line) for each '\n'-terminated line (and a
    // final unterminated one).
    template <typename F>
    void for_each_line(F &&f)
    {
        std::string carry; // a partial line from the previous chunk
        for (;;) {
            ssize_t n = ::read(fd_, buf_.data(), buf_.size());
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw_errno("read");
            if (n == 0)
                break;
            std::string_view chunk(buf_.data(), static_cast<std::size_t>(n));
            std::size_t pos;
            while ((pos = chunk.find('\n')) != std::string_view::npos) {
                if (!carry.empty()) { // finish the straddling line
                    carry.append(chunk.substr(0, pos));
                    f(std::string_view(carry));
                    carry.clear();
                } else {
                    f(chunk.substr(0, pos));
                }
                chunk.remove_prefix(pos + 1);
            }
            carry.append(chunk); // keep the incomplete tail
        }
        if (!carry.empty())
            f(std::string_view(carry));
    }

private:
    int fd_;
    std::vector<char> buf_;
};

template <typename F>
static void for_each_line(std::string_view text, F &&f)
{
    std::size_t pos;
    while ((pos = text.find('\n')) != std::string_view::npos) {
        f(text.substr(0, pos));
        text.remove_prefix(pos + 1);
    }
    if (!text.empty())
        f(text);
}

struct Stats {
    std::size_t lines = 0;
    std::uint64_t sum = 0; // sum of the number after "id="
    bool operator==(const Stats &o) const { return lines == o.lines && sum == o.sum; }
};

static void account(Stats &s, std::string_view line)
{
    ++s.lines;
    auto p = line.find("id=");
    if (p == std::string_view::npos)
        return;
    std::uint64_t v = 0;
    for (std::size_t i = p + 3; i < line.size() && line[i] >= '0' && line[i] <= '9'; ++i)
        v = v * 10 + static_cast<std::uint64_t>(line[i] - '0');
    s.sum += v;
}

int main()
{
    alarm(20);
    char tmpl[64];
    std::snprintf(tmpl, sizeof tmpl, "/tmp/zcpp01-%d-XXXXXX", static_cast<int>(::getpid()));
    int fd = ::mkstemp(tmpl);
    CHECK(fd >= 0);
    const std::string path = tmpl;

    // ~1.1 MiB of text with lines of varying length, so many lines straddle
    // the 64 KiB read() boundary. Expected values computed while writing.
    Stats want;
    {
        std::string out;
        for (unsigned i = 0; i < 40000; ++i) {
            out += "frame id=" + std::to_string(i) + " ts=" + std::to_string(i * 33) + " " +
                   std::string(i % 7, 'x') + '\n';
            ++want.lines;
            want.sum += i;
        }
        out += "trailer without newline id=5"; // last line has no '\n'
        ++want.lines;
        want.sum += 5;
        CHECK(::write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size()));
        ::close(fd);
    }

    using Clk = std::chrono::steady_clock;
    Stats got_map, got_read;
    auto t0 = Clk::now();
    {
        MappedFile mf(path);
        for_each_line(mf.text(), [&](std::string_view l) { account(got_map, l); });
        std::printf("1) MappedFile: %zu bytes as one string_view, %zu lines, id sum %llu\n",
                    mf.size(), got_map.lines, static_cast<unsigned long long>(got_map.sum));

        // ByteView: the same mapping as raw bytes (e.g. to checksum it)
        std::uint64_t h = 1469598103934665603ULL;
        for (std::byte b : mf.bytes())
            h = (h ^ std::to_integer<std::uint64_t>(b)) * 1099511628211ULL;
        std::printf("   ByteView FNV-1a of the file: %016llx\n", static_cast<unsigned long long>(h));

        MappedFile moved = std::move(mf); // ownership moves; mf is now empty
        CHECK(mf.size() == 0 && mf.text().empty());
        CHECK(moved.size() > 0);
    } // munmap happens here, exactly once
    auto t_map = Clk::now() - t0;

    t0 = Clk::now();
    BufferedReader br(path);
    br.for_each_line([&](std::string_view l) { account(got_read, l); });
    auto t_read = Clk::now() - t0;
    std::printf("2) BufferedReader (64 KiB read()s, carry-over for split lines): %zu lines, "
                "id sum %llu\n",
                got_read.lines, static_cast<unsigned long long>(got_read.sum));
    std::printf("   time: mmap %.2f ms, read %.2f ms (printed only)\n",
                std::chrono::duration<double, std::milli>(t_map).count(),
                std::chrono::duration<double, std::milli>(t_read).count());

    CHECK(got_map == want);
    CHECK(got_read == want);

    // Empty file: mmap would fail with EINVAL; MappedFile gives an empty view.
    CHECK(::truncate(path.c_str(), 0) == 0);
    {
        MappedFile empty(path);
        CHECK(empty.size() == 0 && empty.text().empty());
        std::printf("3) empty file -> empty view (no mmap call)\n");
    }
    // Errors become exceptions with errno inside.
    try {
        MappedFile nope("/nonexistent/file");
        CHECK(false);
    } catch (const std::system_error &e) {
        std::printf("4) missing file -> std::system_error: %s\n", e.what());
        CHECK(e.code().value() == ENOENT);
    }
    CHECK(::unlink(path.c_str()) == 0);
    std::printf("01_mapped_file: all checks passed\n");
    return 0;
}
