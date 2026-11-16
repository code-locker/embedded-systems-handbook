/*
 * 01_condition_variable.cpp — std::condition_variable: predicate wait,
 *                             wait_for / wait_until, notify_one / notify_all,
 *                             and condition_variable_any with a custom lock
 *
 * Concept : Same rules as pthread_cond_t, nicer API:
 *             cv.wait(lk, pred)            == while (!pred()) cv.wait(lk);
 *             cv.wait_for(lk, 50ms, pred)  -> bool: pred() at return
 *             cv.wait_until(lk, tp, pred)  -> bool: pred() at return
 *           The lock must be a std::unique_lock<std::mutex> (wait() has to
 *           unlock and relock it). condition_variable_any accepts ANY
 *           lockable (shared_lock, your own lock type) at some extra cost.
 * Build   : make run P=linux-internals/04-condition-variables/cpp/01_condition_variable.cpp
 *           make run P=linux-internals/04-condition-variables/cpp/01_condition_variable.cpp SAN=tsan
 * Read    : ../README.md  (section 5 "C vs C++")
 */
#include <atomic>
#include <chrono>
#include <condition_variable>
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

using namespace std::chrono_literals;
using Ms = std::chrono::duration<double, std::milli>;

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

/*
 * Which clock for deadlines? In real code: std::chrono::steady_clock (it
 * never jumps). libstdc++ (GCC 10+) implements a steady_clock wait with
 * pthread_cond_clockwait(CLOCK_MONOTONIC). The ThreadSanitizer runtime of
 * GCC 11 does not intercept that call, so when ANOTHER thread wakes us it
 * cannot see the unlock/relock inside the wait and reports false "double
 * lock" and data-race warnings. system_clock waits use pthread_cond_
 * timedwait, which TSan understands. So: steady_clock normally, system_clock
 * only in TSan builds. (Timeout-only waits are fine either way.)
 */
#if UNDER_TSAN
using WaitClock = std::chrono::system_clock;
#else
using WaitClock = std::chrono::steady_clock;
#endif

// ---------------------------------------------------------------------------
// 1. wait with a predicate
// ---------------------------------------------------------------------------
static void part1_predicate_wait()
{
    std::mutex m;
    std::condition_variable cv;
    bool ready = false; // guarded by m
    int value = 0;      // guarded by m

    std::thread producer([&] {
        std::this_thread::sleep_for(20ms);
        {
            std::lock_guard<std::mutex> g(m);
            value = 7;
            ready = true;
        }
        // Notifying AFTER unlocking avoids waking the waiter only for it to
        // block on the mutex we still hold ("hurry up and wait"). Both are
        // correct; changing `ready` must happen under the lock either way.
        cv.notify_one();
    });

    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&] { return ready; }); // the while-loop is inside
    std::printf("[1] wait(lk, pred): ready, value=%d\n", value);
    CHECK(value == 7);
    lk.unlock();
    producer.join();
}

// ---------------------------------------------------------------------------
// 2. wait_for (timeout) and wait_until (deadline)
// ---------------------------------------------------------------------------
static void part2_timed()
{
    std::mutex m;
    std::condition_variable cv;
    bool flag = false;

    // Nobody will notify: wait_for returns false = "pred still false".
    auto t0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(m);
    bool got = cv.wait_for(lk, 60ms, [&] { return flag; });
    lk.unlock();
    double waited = Ms(std::chrono::steady_clock::now() - t0).count();
    std::printf("[2] wait_for(60ms) nobody notifies -> %s after %.1f ms\n",
                got ? "true" : "false (timeout)", waited);
    CHECK(!got);
    CHECK(waited >= 59.0);

    // Somebody notifies after ~20 ms; deadline 2 s away.
    std::thread setter([&] {
        std::this_thread::sleep_for(20ms);
        {
            std::lock_guard<std::mutex> g(m);
            flag = true;
        }
        cv.notify_one();
    });
    lk.lock();
    auto deadline = WaitClock::now() + 2s; // computed ONCE
    got = cv.wait_until(lk, deadline, [&] { return flag; });
    lk.unlock();
    setter.join();
    std::printf("    wait_until(now+2s) notified after ~20 ms -> %s\n",
                got ? "true" : "false");
    CHECK(got);

    // The overload WITHOUT a predicate returns std::cv_status and can wake
    // spuriously: you must loop and re-check yourself. Prefer the predicate
    // overloads; they do exactly that loop for you.
}

// ---------------------------------------------------------------------------
// 3. notify_all: a "generation" barrier — workers wait for the next round
// ---------------------------------------------------------------------------
static void part3_notify_all()
{
    std::mutex m;
    std::condition_variable cv;
    int generation = 0; // guarded by m
    int arrived = 0;    // guarded by m
    const int n = 4;

    std::vector<std::thread> ts;
    for (int i = 0; i < n; i++)
        ts.emplace_back([&] {
            std::unique_lock<std::mutex> lk(m);
            int my_gen = generation;
            arrived++;
            cv.notify_all(); // tell main "one more arrived"
            cv.wait(lk, [&] { return generation != my_gen; });
        });

    {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return arrived == n; });
        generation++; // the change every waiter is waiting for
    }
    cv.notify_all(); // notify_one here would release only one worker
    for (auto &t : ts)
        t.join();
    std::printf("[3] notify_all released all %d workers waiting for the "
                "next generation\n", n);
    // Note: main and the workers share ONE cv for two different predicates
    // (arrived == n, generation changed). That is legal but REQUIRES
    // notify_all: notify_one could wake the wrong kind of waiter.
}

// ---------------------------------------------------------------------------
// 4. condition_variable_any with a custom lock type
// ---------------------------------------------------------------------------
class CountingMutex { // any BasicLockable works: lock() + unlock()
public:
    void lock()
    {
        m_.lock();
        locks_.fetch_add(1, std::memory_order_relaxed);
    }
    void unlock() { m_.unlock(); }
    long locks() const { return locks_.load(std::memory_order_relaxed); }

private:
    std::mutex m_;
    std::atomic<long> locks_{0};
};

static void part4_cv_any()
{
    CountingMutex cm;
    std::condition_variable_any cv; // internally: its own mutex + a cv
    int items = 0;                  // guarded by cm

    std::thread producer([&] {
        for (int i = 0; i < 3; i++) {
            std::this_thread::sleep_for(5ms);
            {
                std::lock_guard<CountingMutex> g(cm);
                items++;
            }
            cv.notify_one();
        }
    });
    {
        std::unique_lock<CountingMutex> lk(cm);
        cv.wait(lk, [&] { return items == 3; });
    }
    producer.join();
    std::printf("[4] condition_variable_any with a custom lock: items=3, "
                "the lock was taken %ld times (each wake-up relocks it)\n",
                cm.locks());
    CHECK(cm.locks() >= 4); // 3 producer locks + at least 1 waiter lock
}

int main()
{
    part1_predicate_wait();
    part2_timed();
    part3_notify_all();
    part4_cv_any();
    std::printf("01_condition_variable: all checks passed\n");
    return 0;
}
