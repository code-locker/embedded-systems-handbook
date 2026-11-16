/*
 * 01_std_mutex_raii.cpp — std::mutex and its RAII lock holders:
 *                         lock_guard, unique_lock (defer / try / adopt),
 *                         scoped_lock, std::lock, recursive_mutex, timed_mutex
 *
 * Concept : In C++ you almost never call m.lock()/m.unlock() yourself. A lock
 *           holder object locks in its constructor and unlocks in its
 *           destructor, so every return path and every exception releases
 *           the mutex. Pick the smallest tool that does the job:
 *             lock_guard   : lock for the whole scope, nothing else
 *             unique_lock  : movable, can unlock early, defer, try, time out,
 *                            and is what condition_variable needs (lesson 04)
 *             scoped_lock  : C++17, locks SEVERAL mutexes deadlock-free
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/cpp/01_std_mutex_raii.cpp
 *           make run P=linux-internals/03-mutexes-and-deadlock/cpp/01_std_mutex_raii.cpp SAN=tsan
 * Read    : ../README.md  (section 5 "C vs C++")
 */
#include <chrono>
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

// ---------------------------------------------------------------------------
// 1. lock_guard: exception safety for free
// ---------------------------------------------------------------------------
class Counter {
public:
    void add(long n)
    {
        std::lock_guard<std::mutex> g(m_); // C++17 CTAD: std::lock_guard g(m_);
        if (n < 0)
            throw std::invalid_argument("negative"); // g unlocks during unwind
        value_ += n;
    }
    long get() const
    {
        std::lock_guard g(m_); // const method: m_ is `mutable`
        return value_;
    }
    bool is_locked_by_someone() // test helper
    {
        std::unique_lock l(m_, std::try_to_lock);
        return !l.owns_lock();
    }

private:
    mutable std::mutex m_; // a mutex is neither copyable nor movable
    long value_ = 0;       // guarded by m_
};

static void part1_lock_guard()
{
    Counter c;
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&c] {
            for (int k = 0; k < 20000; k++)
                c.add(1);
        });
    for (auto &t : ts)
        t.join();
    bool threw = false;
    try {
        c.add(-1);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    std::printf("[1] lock_guard: 4 x 20000 adds = %ld; exception thrown "
                "inside the lock, mutex still free: %s\n",
                c.get(), c.is_locked_by_someone() ? "NO" : "yes");
    CHECK(c.get() == 80000);
    CHECK(threw);
    CHECK(!c.is_locked_by_someone());
}

// ---------------------------------------------------------------------------
// 2. unique_lock: the flexible one
// ---------------------------------------------------------------------------
static void part2_unique_lock()
{
    std::mutex m;

    // defer_lock: create the holder unlocked, lock later
    std::unique_lock<std::mutex> l1(m, std::defer_lock);
    CHECK(!l1.owns_lock());
    l1.lock();
    CHECK(l1.owns_lock());

    // try_to_lock from another thread while l1 holds m
    bool other_got_it = true;
    std::thread([&] {
        std::unique_lock<std::mutex> l2(m, std::try_to_lock);
        other_got_it = l2.owns_lock();
    }).join();
    CHECK(!other_got_it);

    l1.unlock(); // unlock early: e.g. before a slow I/O call
    CHECK(!l1.owns_lock());

    // adopt_lock: the mutex is ALREADY locked (by a C API, or by std::lock);
    // the holder only takes over the unlocking duty.
    m.lock();
    {
        std::unique_lock<std::mutex> l3(m, std::adopt_lock);
        CHECK(l3.owns_lock());
    } // unlocked here
    CHECK(m.try_lock());
    m.unlock();

    // movable: hand a held lock out of a function
    auto acquire = [&m] { return std::unique_lock<std::mutex>(m); };
    {
        std::unique_lock<std::mutex> l4 = acquire();
        CHECK(l4.owns_lock());
    }
    std::printf("[2] unique_lock: defer_lock, try_to_lock, early unlock, "
                "adopt_lock, move out of a function: ok\n");
}

// ---------------------------------------------------------------------------
// 3. Two mutexes at once: scoped_lock (C++17) and std::lock (C++11)
//
//    Both use a deadlock-AVOIDANCE algorithm: lock one, try_lock the others,
//    on failure release all and start with the one that was busy. So
//    transfer(a, b) and transfer(b, a) in parallel never deadlock, with no
//    global order needed. (Cost: possible retries under heavy contention.)
// ---------------------------------------------------------------------------
struct Account {
    std::mutex m;
    long balance = 1000;
};

static void transfer_scoped(Account &from, Account &to, long amount)
{
    std::scoped_lock lk(from.m, to.m); // both, any order, no deadlock
    if (from.balance >= amount) {
        from.balance -= amount;
        to.balance += amount;
    }
}

static void transfer_std_lock(Account &from, Account &to, long amount)
{
    // C++11 spelling of the same thing
    std::unique_lock<std::mutex> a(from.m, std::defer_lock);
    std::unique_lock<std::mutex> b(to.m, std::defer_lock);
    std::lock(a, b);
    if (from.balance >= amount) {
        from.balance -= amount;
        to.balance += amount;
    }
}

static void part3_multi_lock()
{
    Account x, y;
    std::thread t1([&] {
        for (int i = 0; i < 20000; i++)
            transfer_scoped(x, y, 3);
    });
    std::thread t2([&] {
        for (int i = 0; i < 20000; i++)
            transfer_std_lock(y, x, 2); // OPPOSITE order: would be ABBA in C
    });
    t1.join();
    t2.join();
    std::printf("[3] 20000 x->y (scoped_lock) and 20000 y->x (std::lock) in "
                "parallel: x=%ld y=%ld total=%ld, no deadlock\n",
                x.balance, y.balance, x.balance + y.balance);
    CHECK(x.balance + y.balance == 2000);

    // std::try_lock: returns -1 on success, else the index of the busy one
    std::mutex p, q;
    q.lock();
    int idx = std::try_lock(p, q);
    CHECK(idx == 1);   // q was busy; p was released again automatically
    CHECK(p.try_lock()); // proves p is free
    p.unlock();
    q.unlock();
    std::printf("    std::try_lock(p, q) with q busy -> %d (index of the busy "
                "one), p released again\n", idx);
}

// ---------------------------------------------------------------------------
// 4. recursive_mutex and timed_mutex
// ---------------------------------------------------------------------------
class Tree {
public:
    // public API locks; it calls another public method that locks again
    int sum_to(int n)
    {
        std::lock_guard<std::recursive_mutex> g(m_);
        return n == 0 ? 0 : n + sum_to(n - 1);
    }

private:
    std::recursive_mutex m_;
};

static void part4_recursive_timed()
{
    Tree t;
    int s = t.sum_to(10);
    CHECK(s == 55);
    std::printf("[4] recursive_mutex: sum_to(10) relocked 11 times = %d\n", s);

    std::timed_mutex tm;
    tm.lock();
    bool got = true;
    double waited_ms = 0;
    std::thread([&] {
        auto t0 = std::chrono::steady_clock::now();
        got = tm.try_lock_for(80ms); // relative: measured on steady_clock
        waited_ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        if (got)
            tm.unlock();
    }).join();
    tm.unlock();
    std::printf("    timed_mutex::try_lock_for(80ms) while held -> %s after "
                "%.1f ms\n", got ? "got it" : "timed out", waited_ms);
    CHECK(!got);

    // try_lock_until with a deadline; holder releases in time.
    // Why system_clock here? libstdc++ (GCC 11+) turns a steady_clock wait
    // into pthread_mutex_clocklock(CLOCK_MONOTONIC), which older
    // ThreadSanitizer runtimes do not intercept: a SUCCESSFUL clocklock is
    // invisible to TSan and the later unlock() is reported as "unlock of an
    // unlocked mutex". A system_clock deadline maps to pthread_mutex_
    // timedlock, which TSan knows. In production prefer steady_clock (it
    // never jumps); this is a tooling workaround only.
    tm.lock();
    std::thread waiter([&] {
        got = tm.try_lock_until(std::chrono::system_clock::now() + 2s);
        if (got)
            tm.unlock();
    });
    std::this_thread::sleep_for(20ms);
    tm.unlock();
    waiter.join();
    CHECK(got);
    std::printf("    timed_mutex::try_lock_until(system_clock::now()+2s), released after "
                "20 ms -> got it\n");
}

int main()
{
    part1_lock_guard();
    part2_unique_lock();
    part3_multi_lock();
    part4_recursive_timed();
    std::printf("01_std_mutex_raii: all checks passed\n");
    return 0;
}
