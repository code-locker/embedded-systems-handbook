/*
 * 01_spinlock_lockable.cpp — a C++17 spinlock class that satisfies the
 *                            BasicLockable / Lockable requirements, so it
 *                            works with std::lock_guard, std::unique_lock,
 *                            std::scoped_lock and std::condition_variable_any
 *
 * Concept : The standard lock wrappers do not care WHAT the lock is. Any
 *           type with lock() and unlock() is BasicLockable; add try_lock()
 *           and it is Lockable. Write the spinlock once (TTAS + backoff,
 *           acquire/release, own cache line) and get RAII, deadlock-free
 *           multi-locking and condition waits for free.
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/cpp/01_spinlock_lockable.cpp
 *           make run P=linux-internals/06-spinlocks-and-futex/cpp/01_spinlock_lockable.cpp SAN=tsan
 * Read    : ../README.md  (section "C vs C++")
 */
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

#if !(defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__))
#include <sched.h>
#endif

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
    sched_yield();
#endif
}

// alignas(64): two hot spinlocks next to each other in a struct would
// otherwise share a cache line and slow each other down (false sharing).
// std::hardware_destructive_interference_size is the portable spelling in
// C++17, but GCC warns about using it in headers/ABIs, so 64 it is.
class alignas(64) Spinlock {
public:
    Spinlock() = default;
    Spinlock(const Spinlock &) = delete; // a lock has identity: never copy
    Spinlock &operator=(const Spinlock &) = delete;

    void lock() noexcept
    {
        unsigned backoff = 1;
        for (;;) {
            // exchange(true) returns the OLD value: false means we took it.
            if (!locked_.exchange(true, std::memory_order_acquire))
                return;
            // TTAS: wait with plain loads, cache line stays Shared.
            while (locked_.load(std::memory_order_relaxed)) {
                for (unsigned i = 0; i < backoff; i++)
                    cpu_relax();
                if (backoff < 1024)
                    backoff <<= 1;
                else
                    std::this_thread::yield(); // owner probably preempted
            }
        }
    }

    bool try_lock() noexcept
    {
        // Cheap pre-check first: do not steal the line if it is taken.
        return !locked_.load(std::memory_order_relaxed) &&
               !locked_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> locked_{false};
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "a spinlock built on a lock-based atomic would be absurd");
};

// The textbook variant on std::atomic_flag (guaranteed lock-free).
// C++20 adds atomic_flag::test() for TTAS; in C++17 only test_and_set.
class FlagSpinlock {
public:
    void lock() noexcept
    {
        while (flag_.test_and_set(std::memory_order_acquire))
            cpu_relax();
    }
    bool try_lock() noexcept { return !flag_.test_and_set(std::memory_order_acquire); }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// A poor man's concept check (C++17): does T have lock/unlock/try_lock?
template <class T, class = void> struct is_lockable : std::false_type {};
template <class T>
struct is_lockable<T, std::void_t<decltype(std::declval<T &>().lock()),
                                  decltype(std::declval<T &>().unlock()),
                                  decltype(bool(std::declval<T &>().try_lock()))>>
    : std::true_type {};
static_assert(is_lockable<Spinlock>::value, "Spinlock must be Lockable");
static_assert(is_lockable<FlagSpinlock>::value, "FlagSpinlock must be Lockable");

// Generic: the same test for any Lockable type. A template, not a virtual
// interface: zero overhead, the compiler inlines lock()/unlock().
template <class Lock>
long hammer(int nthreads, int iters)
{
    Lock lk;
    long counter = 0;
    std::vector<std::thread> ts;
    for (int t = 0; t < nthreads; t++)
        ts.emplace_back([&] {
            for (int i = 0; i < iters; i++) {
                std::lock_guard<Lock> g(lk); // RAII: unlock even on exception
                ++counter;
            }
        });
    for (auto &t : ts)
        t.join();
    return counter;
}

// Bank transfer: two accounts, each with its own spinlock. Locking two locks
// in "from, to" order deadlocks when another thread transfers "to, from".
// std::scoped_lock uses a try_lock-and-back-off algorithm, so ANY order is
// safe. That is why try_lock (Lockable, not just BasicLockable) matters.
struct Account {
    Spinlock lock;
    long balance = 1000;
};

void transfer(Account &from, Account &to, long amount)
{
    std::scoped_lock both(from.lock, to.lock); // deadlock-free for any order
    from.balance -= amount;
    to.balance += amount;
}

int main()
{
    const unsigned hw = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
    const int nthreads = static_cast<int>(hw);
    const int iters = 30000;

    // 1. Basic semantics with std::unique_lock: try_to_lock, manual unlock.
    Spinlock s;
    {
        std::unique_lock<Spinlock> u(s);
        CHECK(u.owns_lock());
        std::unique_lock<Spinlock> u2(s, std::try_to_lock);
        CHECK(!u2.owns_lock()); // held by u
        u.unlock();
        CHECK(s.try_lock());
        s.unlock();
    }
    std::printf("unique_lock / try_to_lock semantics: ok (sizeof(Spinlock) = %zu)\n",
                sizeof(Spinlock));

    // 2. Counter under contention, both spinlock types.
    long c1 = hammer<Spinlock>(nthreads, iters);
    long c2 = hammer<FlagSpinlock>(nthreads, iters);
    long c3 = hammer<std::mutex>(nthreads, iters); // same code, std::mutex
    std::printf("%d threads x %d: Spinlock %ld, FlagSpinlock %ld, std::mutex %ld\n",
                nthreads, iters, c1, c2, c3);
    CHECK(c1 == long(nthreads) * iters);
    CHECK(c2 == long(nthreads) * iters);
    CHECK(c3 == long(nthreads) * iters);

    // 3. Opposite-order transfers with std::scoped_lock: no deadlock, money
    //    is conserved.
    Account a, b;
    std::thread t1([&] { for (int i = 0; i < 20000; i++) transfer(a, b, 1); });
    std::thread t2([&] { for (int i = 0; i < 20000; i++) transfer(b, a, 1); });
    t1.join();
    t2.join();
    std::printf("scoped_lock transfers both ways: a=%ld b=%ld total=%ld\n",
                a.balance, b.balance, a.balance + b.balance);
    CHECK(a.balance + b.balance == 2000);
    CHECK(a.balance == 1000 && b.balance == 1000); // equal transfers each way

    // 4. condition_variable_any works with any BasicLockable, including a
    //    spinlock (std::condition_variable requires std::unique_lock<std::mutex>).
    //    Purely to show the interface: a sleeping wait on a spinlock is a
    //    strange mix in practice, prefer std::mutex for waiting.
    Spinlock qlock;
    std::condition_variable_any cv;
    bool ready = false; // protected by qlock
    int value = 0;      // protected by qlock
    std::thread consumer([&] {
        std::unique_lock<Spinlock> lk(qlock);
        cv.wait(lk, [&] { return ready; });
        CHECK(value == 7);
    });
    {
        std::lock_guard<Spinlock> g(qlock);
        value = 7;
        ready = true;
    }
    cv.notify_one();
    consumer.join();
    std::printf("condition_variable_any + Spinlock: consumer saw value 7\n");

    std::printf("01_spinlock_lockable: all checks passed\n");
    return 0;
}
