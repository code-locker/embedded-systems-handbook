/*
 * 02_ticket_lock.cpp — a fair ticket spinlock with std::atomic, used through
 *                      std::lock_guard, and a check that it really hands
 *                      the lock out in FIFO order
 *
 * Concept : Same algorithm as c/03_ticket_lock.c, as a C++ class. `next`
 *           and `owner` are 16-bit halves here (like Linux's old
 *           arch_spinlock_t on ARM: one 32-bit word { u16 owner; u16 next; }),
 *           which shows that wrap-around is harmless as long as there are
 *           fewer than 65536 waiters.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/cpp/02_ticket_lock.cpp
 *           make run P=linux-internals/06-spinlocks-and-futex/cpp/02_ticket_lock.cpp SAN=tsan
 * Read    : ../README.md  (section "Ticket lock")
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

inline void cpu_relax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

class TicketLock {
public:
    void lock() noexcept
    {
        const std::uint16_t my = next_.fetch_add(1, std::memory_order_relaxed);
        unsigned spins = 0;
        while (owner_.load(std::memory_order_acquire) != my) {
            cpu_relax();
            if (++spins % 1024 == 0)
                std::this_thread::yield(); // a waiter ahead may be preempted
        }
    }

    bool try_lock() noexcept
    {
        std::uint16_t o = owner_.load(std::memory_order_relaxed);
        std::uint16_t expect = o;
        // Take a ticket only if it is the one being served right now.
        return next_.compare_exchange_strong(expect, std::uint16_t(o + 1),
                                             std::memory_order_acquire,
                                             std::memory_order_relaxed);
    }

    void unlock() noexcept
    {
        // Only the owner writes owner_: load + store, no RMW needed.
        std::uint16_t o = owner_.load(std::memory_order_relaxed);
        owner_.store(std::uint16_t(o + 1), std::memory_order_release);
    }

    // For the tests below only. A racy snapshot, never use it for logic.
    std::uint16_t tickets_taken() const noexcept { return next_.load(); }

    // Test hook: start both counters at an arbitrary value (lock must be free).
    void reset_to(std::uint16_t v) noexcept
    {
        next_.store(v);
        owner_.store(v);
    }

private:
    std::atomic<std::uint16_t> next_{0};
    std::atomic<std::uint16_t> owner_{0};
};

int main()
{
    TicketLock lk;

    // 1. FIFO, deterministically: main holds the lock, then waiters 0..3
    //    take tickets ONE AT A TIME (we wait until each has its ticket
    //    before starting the next). They must be served in exactly that
    //    order, whatever the scheduler does.
    std::vector<int> served; // protected by lk
    lk.lock();
    const std::uint16_t base = lk.tickets_taken();
    std::vector<std::thread> waiters;
    for (int i = 0; i < 4; i++) {
        waiters.emplace_back([&, i] {
            std::lock_guard<TicketLock> g(lk);
            served.push_back(i);
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (lk.tickets_taken() != std::uint16_t(base + i + 1)) {
            CHECK(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
    }
    lk.unlock();
    for (auto &t : waiters)
        t.join();
    std::printf("served order:");
    for (int v : served)
        std::printf(" %d", v);
    std::printf("  (tickets taken in order 0 1 2 3)\n");
    CHECK((served == std::vector<int>{0, 1, 2, 3}));

    // 2. Contention + 16-bit wrap-around: start near 65535 and do enough
    //    acquisitions to wrap several times.
    lk.reset_to(65530);
    const int nthreads = int(std::max(2u, std::min(4u, std::thread::hardware_concurrency())));
    const int iters = 30000; // nthreads*iters >> 65536: wraps
    long counter = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; t++)
        ts.emplace_back([&] {
            for (int i = 0; i < iters; i++) {
                std::lock_guard<TicketLock> g(lk);
                ++counter;
            }
        });
    for (auto &t : ts)
        t.join();
    std::printf("%d threads x %d with 16-bit tickets wrapping: counter %ld\n",
                nthreads, iters, counter);
    CHECK(counter == long(nthreads) * iters);
    CHECK(lk.try_lock()); // free again after all that
    lk.unlock();

    std::printf("02_ticket_lock: all checks passed\n");
    return 0;
}
