/*
 * 02_blocking_queue.cpp — BlockingQueue<T>: a bounded MPMC queue template
 *                         with close(), timed pop and move-only items
 *
 * Concept : The C version (../c/04_mpmc_bounded_queue.c) as a reusable
 *           class: std::mutex + two std::condition_variable + std::deque.
 *             push(T)               blocks while full; false if closed
 *             pop() -> optional<T>  blocks while empty; nullopt = closed
 *                                   and drained (end of stream)
 *             try_pop_for(d)        like pop() but gives up after d
 *             close()               wakes everybody, refuses new items
 *           Items are MOVED in and out, so std::unique_ptr works: the queue
 *           owns each item while it sits inside, then hands ownership on.
 * Build   : make run P=linux-internals/04-condition-variables/cpp/02_blocking_queue.cpp
 *           make run P=linux-internals/04-condition-variables/cpp/02_blocking_queue.cpp SAN=tsan
 * Read    : ../README.md  (section 3.7 "Bounded buffer")
 */
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

using namespace std::chrono_literals;

#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif
// steady_clock in real code; system_clock under TSan only, because GCC 11's
// TSan does not intercept pthread_cond_clockwait (see 01_condition_variable).
#if UNDER_TSAN
using WaitClock = std::chrono::system_clock;
#else
using WaitClock = std::chrono::steady_clock;
#endif

template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : cap_(capacity) {}
    BlockingQueue(const BlockingQueue &) = delete; // owns a mutex: not copyable
    BlockingQueue &operator=(const BlockingQueue &) = delete;

    // O(1) amortised. Takes the item by value so callers can move into it.
    bool push(T item)
    {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return q_.size() < cap_ || closed_; });
        if (closed_)
            return false; // item is destroyed here; caller keeps nothing
        q_.push_back(std::move(item));
        lk.unlock();
        not_empty_.notify_one();
        return true;
    }

    std::optional<T> pop()
    {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return !q_.empty() || closed_; });
        return take(lk);
    }

    template <typename Rep, typename Period>
    std::optional<T> try_pop_for(std::chrono::duration<Rep, Period> d)
    {
        std::unique_lock<std::mutex> lk(m_);
        if (!not_empty_.wait_until(lk, WaitClock::now() + d,
                                   [&] { return !q_.empty() || closed_; }))
            return std::nullopt; // timed out, still empty
        return take(lk);
    }

    void close()
    {
        {
            std::lock_guard<std::mutex> g(m_);
            closed_ = true;
        }
        not_full_.notify_all(); // every blocked producer must see it
        not_empty_.notify_all(); // every blocked consumer must see it
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> g(m_);
        return q_.size();
    }

private:
    // Called with m_ held and the predicate true.
    std::optional<T> take(std::unique_lock<std::mutex> &lk)
    {
        if (q_.empty())
            return std::nullopt; // closed and drained
        T item = std::move(q_.front());
        q_.pop_front();
        lk.unlock();
        not_full_.notify_one();
        return item;
    }

    mutable std::mutex m_;
    std::condition_variable not_full_, not_empty_;
    std::deque<T> q_; // guarded by m_
    const std::size_t cap_;
    bool closed_ = false; // guarded by m_
};

int main()
{
    // ---- 1. MPMC with move-only items ----------------------------------
    constexpr int kProducers = 3, kConsumers = 3, kPerProducer = 2000;
    constexpr int kTotal = kProducers * kPerProducer;
    BlockingQueue<std::unique_ptr<int>> q(16);
    std::vector<long long> sums(kConsumers, 0);
    std::vector<int> counts(kConsumers, 0);
    std::vector<int> seen(kTotal, 0); // each slot written by one consumer

    std::vector<std::thread> consumers, producers;
    for (int c = 0; c < kConsumers; c++)
        consumers.emplace_back([&, c] {
            while (auto item = q.pop()) { // nullopt ends the loop
                int v = **item;
                seen[v]++;
                sums[c] += v;
                counts[c]++;
            } // the unique_ptr frees the int here: ownership ended with us
        });
    for (int p = 0; p < kProducers; p++)
        producers.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; i++)
                CHECK(q.push(std::make_unique<int>(p * kPerProducer + i)));
        });
    for (auto &t : producers)
        t.join();
    q.close();
    for (auto &t : consumers)
        t.join();

    long long sum = 0;
    int count = 0;
    for (int c = 0; c < kConsumers; c++) {
        sum += sums[c];
        count += counts[c];
    }
    const long long want = (long long)kTotal * (kTotal - 1) / 2;
    std::printf("[1] %d producers -> BlockingQueue<unique_ptr<int>>(16) -> %d "
                "consumers: %d items, sum %lld (expected %lld)\n",
                kProducers, kConsumers, count, sum, want);
    CHECK(count == kTotal && sum == want);
    for (int v : seen)
        CHECK(v == 1);

    // ---- 2. timed pop and close semantics ------------------------------
    BlockingQueue<int> q2(2);
    auto t0 = std::chrono::steady_clock::now();
    auto r = q2.try_pop_for(40ms);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    std::printf("[2] try_pop_for(40ms) on an empty queue -> %s after %.1f ms\n",
                r ? "item" : "nullopt", ms);
    CHECK(!r && ms >= 39.0);

    CHECK(q2.push(1) && q2.push(2)); // now full
    std::thread blocked([&] { CHECK(!q2.push(3)); }); // sleeps: full
    std::this_thread::sleep_for(20ms);
    q2.close(); // wakes the blocked producer, its push fails
    blocked.join();
    auto a = q2.pop(), b = q2.pop(), c = q2.pop();
    std::printf("    full queue + blocked producer, then close(): producer "
                "released, drained %d %d, then %s\n",
                a.value_or(-1), b.value_or(-1), c ? "item" : "nullopt");
    CHECK(a == 1 && b == 2 && !c);
    CHECK(!q2.push(4));
    CHECK(q2.size() == 0);

    std::printf("02_blocking_queue: all checks passed\n");
    return 0;
}
