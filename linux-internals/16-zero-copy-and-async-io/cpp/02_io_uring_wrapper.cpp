/*
 * 02_io_uring_wrapper.cpp — IoUring: a small RAII, move-only io_uring class
 *                           over the raw syscalls (no liburing), used to
 *                           read a file with many requests in flight
 *
 * Concept : c/07 showed the raw protocol: setup, three mmaps, SQE/CQE
 *           rings with acquire/release indices. Here it becomes a class:
 *             - IoUring::create() returns std::optional (io_uring may be
 *               disabled: sysctl kernel.io_uring_disabled, seccomp, old
 *               kernel) and the error code, instead of throwing;
 *             - the destructor unmaps the rings and closes the fd;
 *             - prep_read() returns false when the SQ is full, submit()
 *               publishes and enters, for_each_completion(f) drains the CQ
 *               with a callback.
 *           std::atomic_ref is C++20, so the shared ring indices are
 *           accessed with the GCC __atomic builtins, exactly as in C.
 *           The demo reads a file in 64 KiB pieces with up to 16 reads in
 *           flight, including a short read at EOF, and checks every byte.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/cpp/02_io_uring_wrapper.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
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

class IoUring {
public:
    static std::optional<IoUring> create(unsigned entries, int *err)
    {
        io_uring_params p{};
        int fd = static_cast<int>(::syscall(__NR_io_uring_setup, entries, &p));
        if (fd < 0) {
            *err = errno;
            return std::nullopt;
        }
        IoUring r;
        r.fd_ = fd;
        r.entries_ = p.sq_entries;
        r.sq_sz_ = p.sq_off.array + p.sq_entries * sizeof(unsigned);
        r.cq_sz_ = p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe);
        const bool single = p.features & IORING_FEAT_SINGLE_MMAP;
        if (single)
            r.sq_sz_ = r.cq_sz_ = std::max(r.sq_sz_, r.cq_sz_);
        r.sq_ = r.map(r.sq_sz_, IORING_OFF_SQ_RING);
        r.cq_ = single ? r.sq_ : r.map(r.cq_sz_, IORING_OFF_CQ_RING);
        r.sqes_sz_ = p.sq_entries * sizeof(io_uring_sqe);
        r.sqes_ = static_cast<io_uring_sqe *>(r.map(r.sqes_sz_, IORING_OFF_SQES));
        if (!r.sq_ || !r.cq_ || !r.sqes_) {
            *err = errno;
            return std::nullopt; // r's destructor releases what was mapped
        }
        auto *sq = static_cast<char *>(r.sq_);
        auto *cq = static_cast<char *>(r.cq_);
        r.sq_head_ = reinterpret_cast<unsigned *>(sq + p.sq_off.head);
        r.sq_tail_ = reinterpret_cast<unsigned *>(sq + p.sq_off.tail);
        r.sq_mask_ = *reinterpret_cast<unsigned *>(sq + p.sq_off.ring_mask);
        r.sq_array_ = reinterpret_cast<unsigned *>(sq + p.sq_off.array);
        r.cq_head_ = reinterpret_cast<unsigned *>(cq + p.cq_off.head);
        r.cq_tail_ = reinterpret_cast<unsigned *>(cq + p.cq_off.tail);
        r.cq_mask_ = *reinterpret_cast<unsigned *>(cq + p.cq_off.ring_mask);
        r.cqes_ = reinterpret_cast<io_uring_cqe *>(cq + p.cq_off.cqes);
        *err = 0;
        return std::optional<IoUring>(std::move(r));
    }

    IoUring(IoUring &&o) noexcept { steal(o); }
    IoUring &operator=(IoUring &&o) noexcept
    {
        if (this != &o) {
            release();
            steal(o);
        }
        return *this;
    }
    IoUring(const IoUring &) = delete;
    IoUring &operator=(const IoUring &) = delete;
    ~IoUring() { release(); }

    unsigned entries() const { return entries_; }

    // Queue a read; false if the SQ is full (submit/drain first).
    bool prep_read(int fd, void *buf, unsigned len, std::uint64_t off, std::uint64_t user_data)
    {
        io_uring_sqe *s = next_sqe();
        if (!s)
            return false;
        s->opcode = IORING_OP_READ;
        s->fd = fd;
        s->addr = reinterpret_cast<std::uintptr_t>(buf);
        s->len = len;
        s->off = off;
        s->user_data = user_data;
        publish();
        return true;
    }

    // Submit everything queued since the last call and wait for at least
    // `wait_nr` completions. Returns the number submitted, or -errno.
    int submit(unsigned wait_nr = 0)
    {
        unsigned n = pending_;
        int r;
        do
            r = static_cast<int>(::syscall(__NR_io_uring_enter, fd_, n, wait_nr,
                                           wait_nr ? IORING_ENTER_GETEVENTS : 0u, nullptr, 0));
        while (r < 0 && errno == EINTR);
        if (r < 0)
            return -errno;
        pending_ -= static_cast<unsigned>(r);
        ++enters_;
        return r;
    }

    // f(user_data, res) for every completion available now; returns count.
    template <typename F>
    unsigned for_each_completion(F &&f)
    {
        unsigned n = 0;
        unsigned head = *cq_head_;
        const unsigned tail = __atomic_load_n(cq_tail_, __ATOMIC_ACQUIRE);
        for (; head != tail; ++head, ++n) {
            const io_uring_cqe &c = cqes_[head & cq_mask_];
            f(c.user_data, c.res);
        }
        __atomic_store_n(cq_head_, head, __ATOMIC_RELEASE); // free the slots
        return n;
    }

    unsigned long enters() const { return enters_; }

private:
    IoUring() = default;

    void *map(std::size_t sz, off_t off)
    {
        void *p = ::mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd_, off);
        return p == MAP_FAILED ? nullptr : p;
    }
    io_uring_sqe *next_sqe()
    {
        unsigned tail = *sq_tail_;
        if (tail - __atomic_load_n(sq_head_, __ATOMIC_ACQUIRE) >= entries_)
            return nullptr;
        unsigned idx = tail & sq_mask_;
        sq_array_[idx] = idx;
        std::memset(&sqes_[idx], 0, sizeof(io_uring_sqe));
        return &sqes_[idx];
    }
    void publish()
    {
        __atomic_store_n(sq_tail_, *sq_tail_ + 1, __ATOMIC_RELEASE);
        ++pending_;
    }
    void release()
    {
        if (sqes_)
            ::munmap(sqes_, sqes_sz_);
        if (cq_ && cq_ != sq_)
            ::munmap(cq_, cq_sz_);
        if (sq_)
            ::munmap(sq_, sq_sz_);
        if (fd_ >= 0)
            ::close(fd_);
        sq_ = cq_ = nullptr;
        sqes_ = nullptr;
        fd_ = -1;
    }
    void steal(IoUring &o)
    {
        fd_ = std::exchange(o.fd_, -1);
        sq_ = std::exchange(o.sq_, nullptr);
        cq_ = std::exchange(o.cq_, nullptr);
        sqes_ = std::exchange(o.sqes_, nullptr);
        sq_sz_ = o.sq_sz_;
        cq_sz_ = o.cq_sz_;
        sqes_sz_ = o.sqes_sz_;
        entries_ = o.entries_;
        sq_head_ = o.sq_head_;
        sq_tail_ = o.sq_tail_;
        sq_array_ = o.sq_array_;
        sq_mask_ = o.sq_mask_;
        cq_head_ = o.cq_head_;
        cq_tail_ = o.cq_tail_;
        cq_mask_ = o.cq_mask_;
        cqes_ = o.cqes_;
        pending_ = o.pending_;
        enters_ = o.enters_;
    }

    int fd_ = -1;
    void *sq_ = nullptr, *cq_ = nullptr;
    io_uring_sqe *sqes_ = nullptr;
    std::size_t sq_sz_ = 0, cq_sz_ = 0, sqes_sz_ = 0;
    unsigned entries_ = 0;
    unsigned *sq_head_ = nullptr, *sq_tail_ = nullptr, *sq_array_ = nullptr;
    unsigned sq_mask_ = 0;
    unsigned *cq_head_ = nullptr, *cq_tail_ = nullptr;
    unsigned cq_mask_ = 0;
    io_uring_cqe *cqes_ = nullptr;
    unsigned pending_ = 0;       // queued but not yet submitted
    unsigned long enters_ = 0;
};

int main()
{
    alarm(20);
    // A file whose size is NOT a multiple of the block size: the last read
    // is short (res < len), which real code must handle.
    constexpr std::size_t kSize = 3 * 1024 * 1024 + 1234;
    constexpr unsigned kBlk = 64 * 1024;
    constexpr unsigned kDepth = 16;

    char tmpl[64];
    std::snprintf(tmpl, sizeof tmpl, "/tmp/zcpp02-%d-XXXXXX", static_cast<int>(::getpid()));
    int fd = ::mkstemp(tmpl);
    CHECK(fd >= 0);
    CHECK(::unlink(tmpl) == 0);
    std::vector<unsigned char> src(kSize);
    for (std::size_t i = 0; i < kSize; ++i)
        src[i] = static_cast<unsigned char>(i * 7 + (i >> 10));
    CHECK(::pwrite(fd, src.data(), kSize, 0) == static_cast<ssize_t>(kSize));

    int err = 0;
    auto ring = IoUring::create(kDepth, &err);
    if (!ring) {
        std::printf("io_uring unavailable: %s (kernel.io_uring_disabled / seccomp / kernel < 5.6)."
                    " Skipping.\n", std::strerror(err));
        ::close(fd);
        std::printf("02_io_uring_wrapper: all checks passed\n");
        return 0;
    }
    std::printf("1) IoUring::create(%u): %u SQ entries\n", kDepth, ring->entries());

    std::vector<unsigned char> dst(kSize, 0);
    const unsigned nblocks = static_cast<unsigned>((kSize + kBlk - 1) / kBlk);
    unsigned next = 0, inflight = 0, done = 0;
    std::size_t bytes = 0;
    int short_reads = 0;
    while (done < nblocks) {
        // keep the ring full
        while (next < nblocks && inflight < kDepth &&
               ring->prep_read(fd, dst.data() + std::size_t(next) * kBlk, kBlk,
                               std::uint64_t(next) * kBlk, next)) {
            ++next;
            ++inflight;
        }
        CHECK(ring->submit(1) >= 0);
        ring->for_each_completion([&](std::uint64_t blk, int res) {
            CHECK(res >= 0); // -errno on failure
            const std::size_t expect = std::min<std::size_t>(kBlk, kSize - blk * kBlk);
            CHECK(static_cast<std::size_t>(res) == expect);
            if (static_cast<unsigned>(res) < kBlk)
                ++short_reads; // the EOF block
            bytes += static_cast<std::size_t>(res);
            --inflight;
            ++done;
        });
    }
    CHECK(bytes == kSize);
    CHECK(short_reads == 1);
    CHECK(dst == src);
    std::printf("2) read %zu bytes as %u x %u KiB requests, depth %u: %lu io_uring_enter calls,\n"
                "   %d short read at EOF, every byte verified\n",
                bytes, nblocks, kBlk / 1024, kDepth, ring->enters(), short_reads);

    IoUring moved = std::move(*ring); // move-only: the ring now lives in `moved`
    ring.reset();                     // destroys the empty shell: nothing to release
    CHECK(moved.entries() == kDepth);
    std::printf("3) moved into another object; the destructor unmaps and closes once\n");
    ::close(fd);
    std::printf("02_io_uring_wrapper: all checks passed\n");
    return 0;
}
