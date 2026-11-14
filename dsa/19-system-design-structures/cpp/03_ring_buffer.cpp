/*
 * 03_ring_buffer.cpp — BoundedQueue<T> (mutex + condition_variable) and lock-free SPSC ring
 *
 * Concept : Same ring as the C version, as a reusable template. C++ adds:
 *           RAII locks (std::unique_lock), predicate waits
 *           (cv.wait(lock, pred) loops for you, handling spurious wake-ups),
 *           move-only payloads (std::unique_ptr frames flow through the
 *           queue with no copies), and std::atomic for the SPSC variant.
 * Build   : make run P=dsa/19-system-design-structures/cpp/03_ring_buffer.cpp
 * Read    : ../README.md  (section "Ring buffer")
 */
#include <array>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t cap) : slots_(cap) {}

    // Blocks while full. Returns false if the queue was closed.
    bool push(T item) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return count_ < slots_.size() || closed_; });
        if (closed_) return false;
        slots_[tail_] = std::move(item);
        tail_ = (tail_ + 1) % slots_.size();
        ++count_;
        lk.unlock();               // notify without holding the lock: the woken
        not_empty_.notify_one();   // thread can grab it immediately
        return true;
    }

    // Blocks while empty; std::nullopt once closed AND drained.
    std::optional<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return count_ > 0 || closed_; });
        if (count_ == 0) return std::nullopt;
        std::optional<T> out(std::move(*slots_[head_]));
        slots_[head_].reset();     // destroy moved-from object now, not at overwrite
        head_ = (head_ + 1) % slots_.size();
        --count_;
        lk.unlock();
        not_full_.notify_one();
        return out;
    }

    void close() {
        { std::lock_guard<std::mutex> lk(m_); closed_ = true; }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

private:
    std::vector<std::optional<T>> slots_;   // optional: T need not be default-constructible
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    bool closed_ = false;
    std::mutex m_;
    std::condition_variable not_full_, not_empty_;
};

// Single-producer single-consumer, N must be a power of two.
// head_ and tail_ on separate cache lines: otherwise producer and consumer
// keep stealing the same line from each other ("false sharing").
template <typename T, std::size_t N>
class SpscRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
public:
    bool try_push(const T& v) {
        auto t = tail_.load(std::memory_order_relaxed);
        if (t - head_.load(std::memory_order_acquire) == N) return false;
        buf_[t & (N - 1)] = v;
        tail_.store(t + 1, std::memory_order_release);   // publish the data
        return true;
    }
    bool try_pop(T& out) {
        auto h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire)) return false;
        out = buf_[h & (N - 1)];
        head_.store(h + 1, std::memory_order_release);   // give the slot back
        return true;
    }
private:
    std::array<T, N> buf_{};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

struct Frame {                      // e.g. a video frame handed from capture to encoder
    int seq;
    std::vector<std::uint8_t> pixels;
};

int main() {
    std::cout << "== video pipeline: 1 capture thread -> queue(4) -> 2 encoder threads ==\n";
    constexpr int kFrames = 2000;
    BoundedQueue<std::unique_ptr<Frame>> q(4);    // move-only payload
    std::vector<std::vector<int>> got(2);

    std::thread capture([&] {
        for (int i = 0; i < kFrames; ++i) {
            auto f = std::make_unique<Frame>(Frame{i, std::vector<std::uint8_t>(64, static_cast<std::uint8_t>(i))});
            bool ok = q.push(std::move(f));
            assert(ok);
            (void)ok;
        }
        q.close();                  // signals "end of stream" (like GStreamer EOS)
    });
    std::vector<std::thread> enc;
    for (int e = 0; e < 2; ++e)
        enc.emplace_back([&, e] {
            while (auto f = q.pop()) {
                const Frame& fr = **f;
                for (auto px : fr.pixels) assert(px == static_cast<std::uint8_t>(fr.seq));
                got[static_cast<std::size_t>(e)].push_back(fr.seq);
            }
        });
    capture.join();
    for (auto& t : enc) t.join();

    std::vector<int> seen(kFrames, 0);
    for (std::size_t e = 0; e < 2; ++e) {
        std::cout << "encoder " << e << " got " << got[e].size() << " frames\n";
        for (std::size_t i = 1; i < got[e].size(); ++i)
            assert(got[e][i] > got[e][i - 1]);     // each consumer sees FIFO order
        for (int s : got[e]) ++seen[static_cast<std::size_t>(s)];
    }
    for (int s : seen) assert(s == 1);
    std::cout << "every frame encoded exactly once, per-consumer order preserved\n";

    std::cout << "\n== push after close is refused ==\n";
    BoundedQueue<int> q2(2);
    q2.close();
    assert(!q2.push(1) && !q2.pop());
    std::cout << "ok\n";

    std::cout << "\n== lock-free SPSC, 300000 items ==\n";
    static SpscRing<std::uint32_t, 256> ring;
    constexpr std::uint32_t kItems = 300000;
    std::thread prod([&] {
        for (std::uint32_t i = 0; i < kItems;)
            if (ring.try_push(i)) ++i; else std::this_thread::yield();
    });
    for (std::uint32_t expect = 0; expect < kItems;) {
        std::uint32_t v;
        if (ring.try_pop(v)) { assert(v == expect); ++expect; }
        else std::this_thread::yield();
    }
    prod.join();
    std::cout << "received 0.." << kItems - 1 << " in order\n";
    std::cout << "OK\n";
    return 0;
}
