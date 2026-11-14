/*
 * 05_spsc_ring.cpp — lock-free SPSC ring buffer with std::atomic + std::thread
 *
 * Concept : The same algorithm as c/08_spsc_lockfree_ring.c as a template
 *           SpscRing<T, N>. Producer owns `tail`, consumer owns `head`;
 *           release/acquire on those indices publishes the slot contents.
 *           Plus one optimisation used by real libraries (rigtorp,
 *           folly::ProducerConsumerQueue, DPDK rte_ring): each side caches
 *           the OTHER side's index, so it only touches the shared cache line
 *           when the cached value says "maybe full/empty".
 * Build   : make run P=dsa/07-stacks-and-queues/cpp/05_spsc_ring.cpp
 * Read    : ../README.md  (section "Lock-free SPSC ring buffer")
 */
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

// 64 bytes = cache line on Cortex-A72 and x86. (std::hardware_destructive_
// interference_size exists, but GCC warns that its value is ABI-unstable.)
constexpr std::size_t kCacheLine = 64;

template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");
    static constexpr std::size_t kMask = N - 1;

public:
    // Producer thread only. O(1), wait-free.
    template <typename U>
    bool try_push(U&& v)
    {
        const std::size_t t = tail_.load(std::memory_order_relaxed);  // mine
        if (t - head_cache_ == N) {
            // Cached head says full: refresh it from the real atomic.
            // ACQUIRE: pairs with the consumer's release store of head, so
            // the consumer has finished reading that slot before we reuse it.
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == N) return false;  // really full
        }
        slots_[t & kMask] = std::forward<U>(v);           // (1) write data
        tail_.store(t + 1, std::memory_order_release);    // (2) publish it
        return true;
    }

    // Consumer thread only. O(1), wait-free.
    std::optional<T> try_pop()
    {
        const std::size_t h = head_.load(std::memory_order_relaxed);  // mine
        if (h == tail_cache_) {
            // ACQUIRE: if we see tail = h+1, we also see slot h's data (1).
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return std::nullopt;  // really empty
        }
        T v = std::move(slots_[h & kMask]);
        head_.store(h + 1, std::memory_order_release);  // slot may be reused
        return v;
    }

    static constexpr std::size_t capacity() { return N; }

private:
    // Layout: producer-written data and consumer-written data on separate
    // cache lines, so the two cores do not ping-pong one line (false sharing).
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer writes
    std::size_t head_cache_ = 0;                           // producer-private
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer writes
    std::size_t tail_cache_ = 0;                           // consumer-private
    alignas(kCacheLine) std::array<T, N> slots_{};
};

// What NOT to do, explained (not run):
//
//   tail_.store(t + 1, std::memory_order_relaxed);   // BUG
//
// Relaxed only guarantees atomicity of tail itself, not ordering with the
// slot write before it. On ARM the consumer may see the new tail and read
// the OLD slot contents. It "works" on x86 in testing and fails on the
// board — the worst kind of bug. ThreadSanitizer (-fsanitize=thread) flags
// the resulting data race on slots_.

int main()
{
    static_assert(std::atomic<std::size_t>::is_always_lock_free,
                  "size_t atomics must be lock-free for this to be lock-free");

    // single-threaded sanity
    {
        SpscRing<int, 4> r;
        for (int i = 0; i < 4; ++i) assert(r.try_push(i));
        assert(!r.try_push(99));  // full at exactly N (free-running indices)
        for (int i = 0; i < 4; ++i) assert(*r.try_pop() == i);
        assert(!r.try_pop());
    }

    // Two threads, integers: order and sum must be exact.
    constexpr std::uint64_t kItems = 200000;
    auto ring = std::make_unique<SpscRing<std::uint64_t, 1024>>();  // ~8 KiB: heap
    std::uint64_t sum = 0, count = 0;
    bool in_order = true;
    {
        std::thread consumer([&] {
            std::uint64_t expect = 1;
            while (expect <= kItems) {
                if (auto v = ring->try_pop()) {
                    in_order &= (*v == expect);
                    sum += *v;
                    ++count;
                    ++expect;
                } else {
                    std::this_thread::yield();
                }
            }
        });
        std::thread producer([&] {
            for (std::uint64_t i = 1; i <= kItems; ++i)
                while (!ring->try_push(i)) std::this_thread::yield();
        });
        producer.join();
        consumer.join();  // join = synchronisation: safe to read sum/count now
    }
    std::cout << "ints:    " << count << " items, sum " << sum << ", in order "
              << (in_order ? "yes" : "NO") << '\n';
    assert(count == kItems);
    assert(sum == kItems * (kItems + 1) / 2);
    assert(in_order);

    // Two threads, std::string: non-trivial type, moved through the ring.
    // If ordering were wrong, the consumer could see a half-built string.
    auto sring = std::make_unique<SpscRing<std::string, 64>>();
    constexpr int kMsgs = 20000;
    int good = 0;
    {
        std::thread consumer([&] {
            for (int i = 0; i < kMsgs;) {
                if (auto s = sring->try_pop()) {
                    good += (*s == "msg-" + std::to_string(i));
                    ++i;
                } else {
                    std::this_thread::yield();
                }
            }
        });
        std::thread producer([&] {
            for (int i = 0; i < kMsgs; ++i) {
                std::string m = "msg-" + std::to_string(i);
                while (!sring->try_push(std::move(m))) std::this_thread::yield();
                // after a failed try_push, m was NOT moved from (forward only
                // happens on the success path), so retrying is safe.
            }
        });
        producer.join();
        consumer.join();
    }
    std::cout << "strings: " << good << " of " << kMsgs << " intact\n";
    assert(good == kMsgs);

    std::cout << "05_spsc_ring: all checks passed\n";
    return 0;
}
