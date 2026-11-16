/*
 * 01_semaphore_class.cpp — a C++17 counting/binary semaphore on
 *                          std::mutex + std::condition_variable, with an
 *                          RAII permit guard
 *
 * Concept : C++17 has no semaphore. C++20 adds std::counting_semaphore<N>
 *           and std::binary_semaphore (acquire, try_acquire,
 *           try_acquire_for, release(n)). This class has the SAME interface,
 *           so code written against it moves to C++20 by changing one
 *           `using`. A Permit guard releases on scope exit, so an exception
 *           in the middle of using a pool slot cannot leak the slot.
 * Build   : make run P=linux-internals/05-semaphores/cpp/01_semaphore_class.cpp
 *           make run P=linux-internals/05-semaphores/cpp/01_semaphore_class.cpp SAN=tsan
 * Read    : ../README.md  (sections 3.9 and 5)
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
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
// steady_clock in real code; system_clock under TSan only (GCC 11's TSan
// does not intercept pthread_cond_clockwait, which libstdc++ uses for
// steady_clock waits — see ../../04-condition-variables/README.md).
#if UNDER_TSAN
using WaitClock = std::chrono::system_clock;
#else
using WaitClock = std::chrono::steady_clock;
#endif

/*
 *   CountingSemaphore<Max>         acquire():  lock; wait(cv, count > 0); count--
 *   +-------------------------+    release(n): lock; count += n; notify n waiters
 *   | mutex m                 |
 *   | condition_variable cv   |    Max = LeastMaxValue: releasing beyond it is a
 *   | ptrdiff_t count         |    bug (C++20 makes it UB; we throw), which is how
 *   +-------------------------+    a BINARY semaphore (Max = 1) catches stray posts.
 */
template <std::ptrdiff_t LeastMaxValue = PTRDIFF_MAX>
class CountingSemaphore {
public:
    explicit CountingSemaphore(std::ptrdiff_t desired) : count_(desired)
    {
        if (desired < 0 || desired > LeastMaxValue)
            throw std::invalid_argument("initial count out of range");
    }
    CountingSemaphore(const CountingSemaphore &) = delete;
    CountingSemaphore &operator=(const CountingSemaphore &) = delete;

    static constexpr std::ptrdiff_t max() noexcept { return LeastMaxValue; }

    void acquire() // P
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [&] { return count_ > 0; });
        --count_;
    }

    bool try_acquire()
    {
        std::lock_guard<std::mutex> g(m_);
        if (count_ == 0)
            return false;
        --count_;
        return true;
    }

    template <typename Rep, typename Period>
    bool try_acquire_for(std::chrono::duration<Rep, Period> d)
    {
        std::unique_lock<std::mutex> lk(m_);
        if (!cv_.wait_until(lk, WaitClock::now() + d, [&] { return count_ > 0; }))
            return false;
        --count_;
        return true;
    }

    void release(std::ptrdiff_t update = 1) // V
    {
        {
            std::lock_guard<std::mutex> g(m_);
            if (update < 0 || count_ > LeastMaxValue - update)
                throw std::logic_error("semaphore released above its maximum");
            count_ += update;
        }
        // n units -> up to n waiters can proceed
        if (update == 1)
            cv_.notify_one();
        else
            cv_.notify_all();
    }

    std::ptrdiff_t value_for_tests() const
    {
        std::lock_guard<std::mutex> g(m_);
        return count_;
    }

private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::ptrdiff_t count_; // guarded by m_
};

using BinarySemaphore = CountingSemaphore<1>; // C++20: std::binary_semaphore

// RAII: acquire in the constructor, release in the destructor.
template <typename Sem>
class Permit {
public:
    explicit Permit(Sem &s) : s_(s) { s_.acquire(); }
    ~Permit() { s_.release(); }
    Permit(const Permit &) = delete;
    Permit &operator=(const Permit &) = delete;

private:
    Sem &s_;
};

// ---------------------------------------------------------------------------
int main()
{
    // ---- 1. resource pool: at most 3 inside, even with exceptions --------
    CountingSemaphore<3> pool(3);
    std::atomic<int> inside{0}, max_inside{0}, exceptions{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; t++)
        ts.emplace_back([&, t] {
            for (int i = 0; i < 10; i++) {
                try {
                    Permit<CountingSemaphore<3>> p(pool);
                    int now = ++inside;
                    int prev = max_inside.load();
                    while (now > prev && !max_inside.compare_exchange_weak(prev, now))
                        ;
                    std::this_thread::sleep_for(200us);
                    --inside;
                    if ((t + i) % 7 == 0)
                        throw std::runtime_error("job failed");
                } catch (const std::runtime_error &) {
                    ++exceptions; // the Permit was released during unwinding
                }
            }
        });
    for (auto &th : ts)
        th.join();
    std::printf("[1] pool of 3, 8 threads x 10 jobs: max inside = %d, %d jobs "
                "threw, permits all returned: value = %td\n",
                max_inside.load(), exceptions.load(), pool.value_for_tests());
    CHECK(max_inside.load() >= 1 && max_inside.load() <= 3);
    CHECK(exceptions.load() > 0);
    CHECK(pool.value_for_tests() == 3);

    // ---- 2. signalling: B waits for A (initial count 0) ------------------
    BinarySemaphore a_done(0);
    int data = 0;
    std::thread b([&] {
        a_done.acquire();
        CHECK(data == 7); // A's write is visible: release -> acquire
    });
    std::thread a([&] {
        std::this_thread::sleep_for(10ms);
        data = 7;
        a_done.release();
    });
    a.join();
    b.join();
    std::printf("[2] binary semaphore as a signal: B ran after A and saw "
                "data = %d\n", data);

    // ---- 3. try_acquire_for and the binary maximum -----------------------
    BinarySemaphore bin(1);
    CHECK(bin.try_acquire());
    auto t0 = std::chrono::steady_clock::now();
    bool got = bin.try_acquire_for(30ms);
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    CHECK(!got && ms >= 29.0);
    bin.release();
    bool threw = false;
    try {
        bin.release(); // a second post on a binary semaphore
    } catch (const std::logic_error &) {
        threw = true;
    }
    std::printf("[3] try_acquire_for(30ms) timed out after %.1f ms; a stray "
                "second release() on a binary semaphore was caught: %s\n",
                ms, threw ? "yes" : "no");
    CHECK(threw);

    // ---- 4. release(n) wakes n waiters (a "start gun" for 4 threads) -----
    CountingSemaphore<> gate(0);
    std::atomic<int> started{0};
    std::vector<std::thread> runners;
    for (int i = 0; i < 4; i++)
        runners.emplace_back([&] {
            gate.acquire();
            ++started;
        });
    std::this_thread::sleep_for(10ms);
    CHECK(started.load() == 0);
    gate.release(4);
    for (auto &r : runners)
        r.join();
    std::printf("[4] release(4) let all 4 waiting threads through\n");
    CHECK(started.load() == 4 && gate.value_for_tests() == 0);

    std::printf("01_semaphore_class: all checks passed\n");
    return 0;
}
