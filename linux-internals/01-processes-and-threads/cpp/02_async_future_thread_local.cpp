/*
 * 02_async_future_thread_local.cpp — std::async / std::future / std::promise /
 *                                    std::packaged_task, and thread_local
 *
 * Concept : A future is a one-shot channel for ONE result (or exception)
 *           from another thread. std::async runs a function and hands you
 *           its future; launch::async forces a new thread, launch::deferred
 *           runs it lazily in the thread that calls get(). thread_local
 *           objects are constructed per thread on first use and destroyed
 *           when that thread exits.
 * Build   : make run P=linux-internals/01-processes-and-threads/cpp/02_async_future_thread_local.cpp
 * Read    : ../README.md  (section 5, "C vs C++")
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string>
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

static long sum_to(long n) {
    long s = 0;
    for (long i = 1; i <= n; i++)
        s += i;
    return s;
}

static void demo_async() {
    std::printf("[1] std::async: launch::async vs launch::deferred\n");
    auto caller = std::this_thread::get_id();

    // launch::async: guaranteed new thread, starts now.
    auto fa = std::async(std::launch::async, [caller] {
        return std::make_pair(sum_to(1000), std::this_thread::get_id() != caller);
    });
    // launch::deferred: nothing runs until get()/wait(), then it runs HERE.
    auto fd = std::async(std::launch::deferred, [caller] {
        return std::make_pair(sum_to(1000), std::this_thread::get_id() == caller);
    });
    // Default policy (async|deferred) lets the library choose — so it may
    // never run in parallel. Say what you mean.
    CHECK(fd.wait_for(0s) == std::future_status::deferred);

    auto [ra, other_thread] = fa.get(); // blocks until ready
    auto [rd, same_thread] = fd.get();
    std::printf("    async: %ld on another thread: %d; deferred: %ld on caller: %d\n",
                ra, other_thread, rd, same_thread);
    CHECK(ra == 500500 && rd == 500500 && other_thread && same_thread);

    // Pitfall: the future returned by std::async(launch::async, ...) BLOCKS
    // in its destructor until the task finishes. So
    //     std::async(std::launch::async, f);   // temporary future
    // is NOT fire-and-forget: it runs f synchronously, statement by statement.
}

static void demo_exception_through_future() {
    std::printf("\n[2] exceptions travel through the future\n");
    auto f = std::async(std::launch::async, []() -> int {
        throw std::runtime_error("calibration failed");
    });
    try {
        (void)f.get();
        CHECK(false);
    } catch (const std::runtime_error &e) {
        std::printf("    get() rethrew: %s\n", e.what());
    }
}

static void demo_promise_and_task() {
    std::printf("\n[3] std::promise (set by hand) and std::packaged_task\n");
    std::promise<std::string> p;
    std::future<std::string> f = p.get_future();
    std::thread producer([&p] {
        std::this_thread::sleep_for(2ms);
        p.set_value("frame #1 ready"); // wakes whoever waits on f
    });
    // wait_for with a timeout: never block forever on a result that may
    // never come (e.g. the producer crashed).
    CHECK(f.wait_for(5s) == std::future_status::ready);
    std::printf("    promise delivered: %s\n", f.get().c_str());
    producer.join();

    // packaged_task = function + promise: call it anywhere, get via future.
    std::packaged_task<long(long)> task(sum_to);
    std::future<long> tf = task.get_future();
    std::thread runner(std::move(task), 100L);
    std::printf("    packaged_task result: %ld\n", tf.get());
    runner.join();
}

/* ---- thread_local ---------------------------------------------------- */
static std::atomic<int> g_ctor{0}, g_dtor{0};

struct PerThreadStats {
    int calls = 0;
    PerThreadStats() { g_ctor++; }  // runs on first use IN EACH THREAD
    ~PerThreadStats() { g_dtor++; } // runs when THAT thread exits
};

static thread_local PerThreadStats tl_stats;

static int record_call() { return ++tl_stats.calls; }

static void demo_thread_local() {
    std::printf("\n[4] thread_local: constructed per thread, destroyed at thread exit\n");
    constexpr int kThreads = 3;
    std::vector<std::thread> ts;
    std::vector<int> last(kThreads);
    for (int t = 0; t < kThreads; t++)
        ts.emplace_back([t, &last] {
            int r = 0;
            for (int i = 0; i < 100 * (t + 1); i++)
                r = record_call();
            last[size_t(t)] = r;
        });
    for (auto &t : ts)
        t.join();
    for (int t = 0; t < kThreads; t++) {
        std::printf("    thread %d counted %d calls\n", t, last[size_t(t)]);
        CHECK(last[size_t(t)] == 100 * (t + 1)); // each saw only its own
    }
    std::printf("    constructors %d, destructors %d (main has not used it yet)\n",
                g_ctor.load(), g_dtor.load());
    CHECK(g_ctor == kThreads && g_dtor == kThreads);
    CHECK(record_call() == 1); // main's own fresh copy
}

int main() {
    demo_async();
    demo_exception_through_future();
    demo_promise_and_task();
    demo_thread_local();
    std::printf("\n02_async_future_thread_local: all checks passed\n");
    return 0;
}
