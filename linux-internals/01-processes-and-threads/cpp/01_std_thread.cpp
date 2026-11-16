/*
 * 01_std_thread.cpp — std::thread: create, join, detach, argument passing
 *                     (copy vs std::ref vs move), RAII joiner, exceptions,
 *                     hardware_concurrency, native_handle
 *
 * Concept : std::thread is a thin RAII-ish wrapper over pthread_create.
 *           Arguments are COPIED into the new thread (use std::ref to share,
 *           std::move to hand over ownership). A joinable std::thread that
 *           is destroyed calls std::terminate(), so every thread must be
 *           joined or detached on every path — wrap it (C++20: std::jthread
 *           does this for you).
 * Build   : make run P=linux-internals/01-processes-and-threads/cpp/01_std_thread.cpp
 * Read    : ../README.md  (section 5, "C vs C++")
 */
#include <pthread.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
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

/* RAII guard: joins in the destructor, so an early return or an exception
 * between "create" and "join" cannot reach std::terminate(). This is what
 * C++20's std::jthread does (plus a stop_token). */
class JoiningThread {
public:
    template <class F, class... Args>
    explicit JoiningThread(F &&f, Args &&...args)
        : t_(std::forward<F>(f), std::forward<Args>(args)...) {}
    JoiningThread(JoiningThread &&) noexcept = default;
    JoiningThread &operator=(JoiningThread &&) = delete;
    ~JoiningThread() {
        if (t_.joinable())
            t_.join();
    }
    std::thread &get() { return t_; }

private:
    std::thread t_;
};

static void demo_basics() {
    std::printf("[1] create + join, results through per-thread slots\n");
    unsigned hc = std::thread::hardware_concurrency(); // may be 0 = unknown
    unsigned n = hc == 0 ? 2 : (hc > 4 ? 4 : hc);
    std::printf("    hardware_concurrency() = %u, using %u threads\n", hc, n);

    std::vector<long> data(100000);
    std::iota(data.begin(), data.end(), 1L);
    std::vector<long> partial(n, 0); // one slot per thread: no sharing
    std::vector<std::thread> pool;
    pool.reserve(n);
    for (unsigned i = 0; i < n; i++) {
        // Capture i BY VALUE. Capturing [&] would bind every lambda to the
        // same loop variable — the C++ twin of passing &i in C.
        pool.emplace_back([i, n, &data, &partial] {
            size_t lo = data.size() * i / n, hi = data.size() * (i + 1) / n;
            partial[i] = std::accumulate(data.begin() + long(lo),
                                         data.begin() + long(hi), 0L);
        });
    }
    for (auto &t : pool)
        t.join(); // join = "happens-before": partial[] is safe to read now
    long total = std::accumulate(partial.begin(), partial.end(), 0L);
    std::printf("    total = %ld\n", total);
    CHECK(total == 100000L * 100001L / 2);
}

static void bump(int &x) { x += 10; }
static void consume(std::unique_ptr<std::string> s, std::string *out) {
    *out = *s + " (moved into thread)";
}

static void demo_arguments() {
    std::printf("\n[2] arguments are copied: std::ref to share, std::move to hand over\n");
    int value = 1;
    // std::thread t(bump, value);   // does NOT compile: the copy is an
    //                               // rvalue and cannot bind to int&.
    std::thread t1(bump, std::ref(value)); // pass a reference explicitly
    t1.join();
    std::printf("    after std::ref: value = %d\n", value);
    CHECK(value == 11);

    auto owned = std::make_unique<std::string>("payload");
    std::string out;
    std::thread t2(consume, std::move(owned), &out); // ownership moves
    t2.join();
    CHECK(owned == nullptr);
    std::printf("    %s\n", out.c_str());
    CHECK(out == "payload (moved into thread)");

    // std::thread itself is move-only: a handle with exactly one owner.
    std::thread a([] {});
    std::thread b = std::move(a);
    CHECK(!a.joinable() && b.joinable());
    b.join();
}

static void demo_raii_and_ids() {
    std::printf("\n[3] RAII joiner, thread ids, native_handle\n");
    std::thread::id worker_id;
    {
        JoiningThread jt([&worker_id] { worker_id = std::this_thread::get_id(); });
        CHECK(jt.get().get_id() != std::this_thread::get_id());
        // native_handle() is the pthread_t: use it for anything std::thread
        // lacks (name, affinity, scheduling policy).
        pthread_setname_np(jt.get().native_handle(), "raii-worker");
    } // joined here, even if something above had thrown
    CHECK(worker_id != std::thread::id{});
    CHECK(worker_id != std::this_thread::get_id());
    std::printf("    worker had a distinct std::thread::id; joined by destructor\n");
}

static void demo_detach() {
    std::printf("\n[4] detach: fire and forget (and why it is dangerous)\n");
    // A detached thread must not touch anything that may die before it.
    // Here it only touches a shared_ptr it holds a copy of, so the flag
    // outlives both this function and the thread.
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread([done] {
        std::this_thread::sleep_for(1ms);
        done->store(true);
    }).detach();
    for (int i = 0; i < 2000 && !done->load(); i++) // bounded wait, max ~2 s
        std::this_thread::sleep_for(1ms);
    std::printf("    detached thread finished: %s\n", done->load() ? "yes" : "NO");
    CHECK(done->load());
}

static void demo_exceptions() {
    std::printf("\n[5] an exception must not escape a thread function\n");
    // If it escapes, std::terminate() kills the WHOLE process. Catch inside
    // and carry it out as std::exception_ptr (std::async does this for you:
    // see 02_async_future_thread_local.cpp).
    std::exception_ptr err;
    std::thread t([&err] {
        try {
            throw std::runtime_error("sensor timeout");
        } catch (...) {
            err = std::current_exception();
        }
    });
    t.join();
    CHECK(err != nullptr);
    try {
        std::rethrow_exception(err);
    } catch (const std::runtime_error &e) {
        std::printf("    rethrown in main: %s\n", e.what());
        CHECK(std::string(e.what()) == "sensor timeout");
    }
}

int main() {
    demo_basics();
    demo_arguments();
    demo_raii_and_ids();
    demo_detach();
    demo_exceptions();
    std::printf("\n01_std_thread: all checks passed\n");
    return 0;
}
