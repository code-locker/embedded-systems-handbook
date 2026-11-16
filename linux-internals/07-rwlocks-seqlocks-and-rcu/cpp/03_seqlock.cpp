/*
 * 03_seqlock.cpp — a generic Seqlock<T> for any trivially copyable T,
 *                  storing T as an array of std::atomic<word> so that the
 *                  racy reader copy is still well-defined C++
 *
 * Concept : Same protocol as c/03_seqlock.c (odd = writing, readers retry).
 *           Making it a template raises the C++ question: the reader must
 *           copy T WHILE a writer may be changing it, which is a data race
 *           on a plain T. Solution: keep T's bytes in std::atomic<uint64_t>
 *           words (lock-free, so on ARMv8 these are ordinary loads and
 *           stores plus LDAR/STLR for the acquire/release ones), and
 *           memcpy between T and words. static_assert keeps it honest.
 *           Example payload: a camera frame timestamp {sec, nsec, frame}.
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/03_seqlock.cpp
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/03_seqlock.cpp SAN=tsan
 * Read    : ../README.md  (section "Seqlock")
 */
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

template <class T>
class Seqlock {
    static_assert(std::is_trivially_copyable_v<T>, "Seqlock<T> copies T with memcpy");
    using Word = std::uint64_t;
    static constexpr std::size_t kWords = (sizeof(T) + sizeof(Word) - 1) / sizeof(Word);
    static_assert(std::atomic<Word>::is_always_lock_free, "need lock-free 64-bit atomics");

public:
    explicit Seqlock(const T &init = T{}) { store_words(init); }

    // Writers are serialised by a mutex (the seqlock itself only protects
    // readers from writers). Returns nothing, never blocks readers.
    void write(const T &v)
    {
        std::lock_guard<std::mutex> g(wlock_);
        unsigned s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed); // odd: writing
        store_words(v);                              // release stores
        seq_.store(s + 2, std::memory_order_release); // even: published
    }

    // Returns a consistent copy. `retries` counts how often it had to retry.
    T read(unsigned long *retries = nullptr) const
    {
        for (unsigned long n = 0;; n++) {
            unsigned s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) {
                if (n % 1024 == 1023)
                    std::this_thread::yield(); // writer may be preempted
                continue;
            }
            std::array<Word, kWords> buf;
            for (std::size_t i = 0; i < kWords; i++)
                buf[i] = data_[i].load(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s1) {
                if (retries)
                    *retries += n;
                T out;
                std::memcpy(&out, buf.data(), sizeof(T));
                return out;
            }
        }
    }

    unsigned sequence() const { return seq_.load(); }

private:
    void store_words(const T &v)
    {
        std::array<Word, kWords> buf{};
        std::memcpy(buf.data(), &v, sizeof(T));
        for (std::size_t i = 0; i < kWords; i++)
            data_[i].store(buf[i], std::memory_order_release);
    }

    std::atomic<unsigned> seq_{0};
    std::array<std::atomic<Word>, kWords> data_{};
    std::mutex wlock_;
};

// A camera timestamp: the classic "two fields that must match" case.
// A torn read could pair a new `sec` with an old `nsec` (time goes back
// by almost a second) or a frame number with the wrong time.
struct FrameTime {
    std::int64_t sec;
    std::int64_t nsec;  // 0 .. 999'999'999
    std::uint32_t frame;
};

static FrameTime time_of_frame(std::uint32_t f)
{
    // 30 fps: frame f at f * 33'333'333 ns
    std::int64_t ns = std::int64_t(f) * 33'333'333;
    return {ns / 1'000'000'000, ns % 1'000'000'000, f};
}

int main()
{
    Seqlock<FrameTime> clock(time_of_frame(0));
    FrameTime t0 = clock.read();
    CHECK(t0.frame == 0 && t0.sec == 0 && t0.nsec == 0);
    std::printf("sizeof(FrameTime) = %zu -> stored in %zu atomic words\n",
                sizeof(FrameTime), (sizeof(FrameTime) + 7) / 8);

    const std::uint32_t kFrames = 30000;
    std::atomic<bool> done{false};
    std::atomic<long> reads{0}, torn{0}, backwards{0};
    std::atomic<unsigned long> retries{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; r++)
        readers.emplace_back([&] {
            std::uint32_t last = 0;
            unsigned long my_retries = 0;
            for (;;) {
                bool fin = done.load();
                FrameTime t = clock.read(&my_retries);
                FrameTime expect = time_of_frame(t.frame);
                if (t.sec != expect.sec || t.nsec != expect.nsec)
                    ++torn; // fields from two different writes
                if (t.frame < last)
                    ++backwards;
                last = t.frame;
                ++reads;
                if (fin)
                    break;
            }
            retries += my_retries;
        });
    std::thread writer([&] {
        for (std::uint32_t f = 1; f <= kFrames; f++)
            clock.write(time_of_frame(f)); // the "capture interrupt"
    });
    writer.join();
    done = true;
    for (auto &t : readers)
        t.join();

    FrameTime last = clock.read();
    std::printf("%u frames written, %ld reads, %lu retries (timing-dependent), "
                "%ld torn, %ld went backwards\n", kFrames, reads.load(),
                retries.load(), torn.load(), backwards.load());
    CHECK(torn.load() == 0);
    CHECK(backwards.load() == 0);
    CHECK(last.frame == kFrames);
    CHECK(clock.sequence() == 2 * kFrames);

    std::printf("03_seqlock: all checks passed\n");
    return 0;
}
