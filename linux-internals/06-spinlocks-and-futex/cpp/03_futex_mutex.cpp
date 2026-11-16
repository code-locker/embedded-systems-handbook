/*
 * 03_futex_mutex.cpp — Drepper's 3-state futex mutex as a C++ class, made
 *                      Lockable, then used with std::unique_lock and
 *                      std::condition_variable_any in a small work queue
 *
 * Concept : This is roughly what std::mutex is on Linux: std::mutex wraps
 *           pthread_mutex_t, which is this algorithm on a futex word. The
 *           C++ class adds RAII-friendly lock()/try_lock()/unlock() and a
 *           spin-then-sleep phase like PTHREAD_MUTEX_ADAPTIVE_NP.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/cpp/03_futex_mutex.cpp
 *           make run P=linux-internals/06-spinlocks-and-futex/cpp/03_futex_mutex.cpp SAN=tsan
 * Read    : ../README.md  (sections "A futex mutex from scratch", "Adaptive mutexes")
 */
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
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

class FutexMutex {
public:
    FutexMutex() = default;
    FutexMutex(const FutexMutex &) = delete;
    FutexMutex &operator=(const FutexMutex &) = delete;

    void lock() noexcept
    {
        int c = kFree;
        if (state_.compare_exchange_strong(c, kLocked, std::memory_order_acquire,
                                           std::memory_order_relaxed))
            return; // fast path: no syscall

        // Adaptive phase: the owner is probably on another core and about
        // to unlock. Spin briefly before paying for a sleep + wake.
        for (int i = 0; i < kSpin; i++) {
            if (state_.load(std::memory_order_relaxed) == kFree) {
                c = kFree;
                if (state_.compare_exchange_strong(c, kLocked,
                                                   std::memory_order_acquire,
                                                   std::memory_order_relaxed))
                    return;
            }
        }

        // Slow path (Drepper mutex 3): mark "contended" and sleep.
        if (c != kContended)
            c = state_.exchange(kContended, std::memory_order_acquire);
        while (c != kFree) {
            futex(FUTEX_WAIT_PRIVATE, kContended);
            c = state_.exchange(kContended, std::memory_order_acquire);
        }
    }

    bool try_lock() noexcept
    {
        int c = kFree;
        return state_.compare_exchange_strong(c, kLocked, std::memory_order_acquire,
                                              std::memory_order_relaxed);
    }

    void unlock() noexcept
    {
        if (state_.fetch_sub(1, std::memory_order_release) != kLocked) {
            state_.store(kFree, std::memory_order_release);
            futex(FUTEX_WAKE_PRIVATE, 1);
            wakes_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    long wake_syscalls() const noexcept { return wakes_.load(); }

private:
    static constexpr int kFree = 0, kLocked = 1, kContended = 2;
    static constexpr int kSpin = 100; // like glibc's default mutex_spin_count

    long futex(int op, int val) noexcept
    {
        // The kernel needs the address of a plain 32-bit int. On Linux
        // std::atomic<int> is exactly that (checked below).
        return syscall(SYS_futex, reinterpret_cast<int *>(&state_), op, val,
                       nullptr, nullptr, 0);
    }

    std::atomic<int> state_{kFree};
    std::atomic<long> wakes_{0}; // statistics only
    static_assert(sizeof(std::atomic<int>) == sizeof(int), "futex word must be 32 bit");
    static_assert(std::atomic<int>::is_always_lock_free, "futex word must be lock-free");
};

// A tiny MPMC work queue: FutexMutex + condition_variable_any.
template <class T>
class WorkQueue {
public:
    void push(T v)
    {
        {
            std::lock_guard<FutexMutex> g(m_);
            q_.push_back(std::move(v));
        }
        cv_.notify_one(); // notify outside the lock: woken thread won't block on m_
    }
    void close()
    {
        {
            std::lock_guard<FutexMutex> g(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }
    // Returns false when the queue is closed and empty.
    bool pop(T &out)
    {
        std::unique_lock<FutexMutex> lk(m_);
        cv_.wait(lk, [&] { return !q_.empty() || closed_; }); // predicate loop
        if (q_.empty())
            return false;
        out = std::move(q_.front());
        q_.pop_front();
        return true;
    }
    FutexMutex &mutex() { return m_; }

private:
    FutexMutex m_;
    std::condition_variable_any cv_;
    std::deque<T> q_;     // protected by m_
    bool closed_ = false; // protected by m_
};

int main()
{
    // 1. Counter under contention.
    FutexMutex m;
    long counter = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&] {
            for (int i = 0; i < 30000; i++) {
                std::lock_guard<FutexMutex> g(m);
                ++counter;
            }
        });
    for (auto &t : ts)
        t.join();
    std::printf("4 threads x 30000: counter %ld, unlock needed FUTEX_WAKE %ld "
                "times (timing-dependent)\n", counter, m.wake_syscalls());
    CHECK(counter == 4L * 30000);
    CHECK(m.try_lock());
    m.unlock();

    // 2. Work queue: 2 producers push 1..N each, 3 consumers sum.
    WorkQueue<int> q;
    const int N = 5000;
    std::atomic<long> sum{0};
    std::atomic<int> items{0};
    std::vector<std::thread> consumers, producers;
    for (int c = 0; c < 3; c++)
        consumers.emplace_back([&] {
            int v;
            while (q.pop(v)) {
                sum.fetch_add(v, std::memory_order_relaxed);
                items.fetch_add(1, std::memory_order_relaxed);
            }
        });
    for (int p = 0; p < 2; p++)
        producers.emplace_back([&] {
            for (int i = 1; i <= N; i++)
                q.push(i);
        });
    for (auto &t : producers)
        t.join();
    q.close();
    for (auto &t : consumers)
        t.join();
    const long want = 2L * N * (N + 1) / 2;
    std::printf("work queue: %d items, sum %ld (expected %ld)\n", items.load(),
                sum.load(), want);
    CHECK(items.load() == 2 * N);
    CHECK(sum.load() == want);

    std::printf("03_futex_mutex: all checks passed\n");
    return 0;
}
