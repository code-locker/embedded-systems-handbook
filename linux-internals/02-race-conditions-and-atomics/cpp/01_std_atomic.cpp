/*
 * 01_std_atomic.cpp — std::atomic<T>: RMW operators, is_lock_free,
 *                     compare_exchange loops, memory_order, publishing an
 *                     object through std::atomic<T*>, atomic_flag spinlock
 *                     with std::lock_guard, stop flags
 *
 * Concept : std::atomic<T> is the C++ face of the same hardware atomics as
 *           C11 <stdatomic.h>; the default order of every operation is
 *           seq_cst. `++a` / `a += n` / `a.fetch_add(n)` are single atomic
 *           RMWs; `a = a + 1` is an atomic LOAD followed by a separate
 *           atomic STORE — a race condition. Big types may be implemented
 *           with a hidden lock: ask is_always_lock_free.
 * Build   : make run P=linux-internals/02-race-conditions-and-atomics/cpp/01_std_atomic.cpp
 * Read    : ../README.md  (section 5, "C vs C++")
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
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
constexpr int kThreads = 4;
constexpr int kIters = 100000;

template <class F>
static void run_threads(int n, F f) {
    std::vector<std::thread> ts;
    for (int i = 0; i < n; i++)
        ts.emplace_back(f, i);
    for (auto &t : ts)
        t.join();
}

/* ---- 1. atomic RMW vs load-then-store -------------------------------- */
static void demo_rmw() {
    std::printf("[1] ++a is one atomic RMW; a = a + 1 is two separate atomic ops\n");
    std::atomic<int> good{0}, bad{0};
    run_threads(kThreads, [&](int) {
        for (int i = 0; i < kIters; i++) {
            ++good;                          // fetch_add(1, seq_cst)
            bad.store(bad.load() + 1,        // load ... (another thread may
                      std::memory_order_relaxed); // store here) ... store
        }
    });
    std::printf("    ++good -> %d (exact)   bad = bad + 1 -> %d (lost %d, timing-dependent)\n",
                good.load(), bad.load(), kThreads * kIters - bad.load());
    CHECK(good == kThreads * kIters);
    CHECK(bad <= kThreads * kIters);
    // No data race on `bad` (every access is atomic), so TSan is silent —
    // the lost updates are a RACE CONDITION, a logic bug.
}

/* ---- 2. lock-freedom ---------------------------------------------------- */
struct Pair8 { int32_t a, b; };        // 8 bytes: one register
struct Pair16 { int64_t a, b; };       // 16 bytes: needs a double-width CAS
struct Big { char bytes[24]; };         // never lock-free

static void demo_lock_free() {
    std::printf("\n[2] is_always_lock_free (compile time) / is_lock_free (run time)\n");
    std::atomic<int> i{0};
    std::atomic<long long> ll{0};
    std::atomic<Pair8> p8{Pair8{1, 2}};
    std::printf("    atomic<int>       : always %d, now %d\n",
                std::atomic<int>::is_always_lock_free, i.is_lock_free());
    std::printf("    atomic<long long> : always %d, now %d\n",
                std::atomic<long long>::is_always_lock_free, ll.is_lock_free());
    std::printf("    atomic<Pair8>     : always %d, now %d\n",
                std::atomic<Pair8>::is_always_lock_free, p8.is_lock_free());
    // For 16+ byte types GCC routes operations through libatomic, which may
    // use a hidden lock table; we only ask the compile-time question here
    // (the run-time query itself would need -latomic).
    std::printf("    atomic<Pair16>    : always %d  (x86-64 cmpxchg16b / ARMv8.1 CASP"
                " exist, but GCC does not promise them)\n",
                std::atomic<Pair16>::is_always_lock_free);
    std::printf("    atomic<Big>       : always %d  (a hidden lock: NOT safe in a"
                " signal handler or between processes)\n",
                std::atomic<Big>::is_always_lock_free);
    static_assert(std::atomic<int>::is_always_lock_free, "int must be lock-free");
    static_assert(!std::atomic<Big>::is_always_lock_free, "24 bytes cannot be");
    CHECK(p8.load().b == 2);
}

/* ---- 3. compare_exchange loop: generic fetch_max / fetch_update -------- */
template <class T, class F>
static T fetch_update(std::atomic<T> &a, F f) {
    T old = a.load(std::memory_order_relaxed);
    // compare_exchange_weak(expected, desired):
    //   if a == expected: a = desired, return true
    //   else: expected = a (refreshed!), return false -> loop recomputes
    while (!a.compare_exchange_weak(old, f(old), std::memory_order_acq_rel,
                                    std::memory_order_relaxed)) {
    }
    return old;
}

static void demo_cas() {
    std::printf("\n[3] compare_exchange_weak loop: saturating add (never above 1000)\n");
    std::atomic<int> level{0};
    std::atomic<int> accepted{0};
    run_threads(kThreads, [&](int) {
        for (int i = 0; i < 1000; i++) {
            int before = fetch_update(level, [](int v) { return v + 3 > 1000 ? v : v + 3; });
            if (before + 3 <= 1000)
                accepted.fetch_add(1, std::memory_order_relaxed);
        }
    });
    std::printf("    level = %d after %d accepted adds of 3 (cap 1000)\n", level.load(),
                accepted.load());
    CHECK(level == 999 && accepted == 333);
}

/* ---- 4. publish an object: release store of a pointer ------------------ */
struct Config {
    std::string device;
    int rate_hz;
    std::vector<int> taps;
};

static void demo_publish() {
    std::printf("\n[4] publish a fully built object through atomic<Config*>\n");
    std::atomic<Config *> current{nullptr};
    std::unique_ptr<Config> owner; // main owns it; readers only borrow

    std::thread reader([&] {
        Config *c;
        // acquire: once we see the pointer we see everything written into
        // the object before the release store — no torn half-built Config.
        for (int i = 0; (c = current.load(std::memory_order_acquire)) == nullptr; i++) {
            CHECK(i < 5000000); // bounded spin (never hang)
            std::this_thread::yield();
        }
        CHECK(c->device == "/dev/video0" && c->rate_hz == 30 && c->taps.size() == 3);
        std::printf("    reader saw %s @ %d Hz with %zu taps\n", c->device.c_str(),
                    c->rate_hz, c->taps.size());
    });
    std::this_thread::sleep_for(1ms);
    owner = std::make_unique<Config>(Config{"/dev/video0", 30, {1, 2, 1}});
    current.store(owner.get(), std::memory_order_release); // publish
    reader.join();
    // Replacing the config while readers may still hold the old pointer
    // needs a reclamation scheme: RCU (lesson 07), hazard pointers, or
    // std::atomic<std::shared_ptr<T>> (C++20).
}

/* ---- 5. atomic_flag spinlock usable with std::lock_guard --------------- */
class SpinLock {
public:
    void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire))
            std::this_thread::yield(); // C++20: flag_.wait(true) sleeps instead
    }
    void unlock() noexcept { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

static void demo_spinlock() {
    std::printf("\n[5] atomic_flag spinlock + std::lock_guard (BasicLockable)\n");
    SpinLock lock;
    std::vector<int> log; // NOT thread-safe on its own
    run_threads(kThreads, [&](int id) {
        for (int i = 0; i < 2000; i++) {
            std::lock_guard<SpinLock> g(lock); // RAII: unlocks on every path
            log.push_back(id);
        }
    });
    std::printf("    %zu entries pushed under the spinlock\n", log.size());
    CHECK(log.size() == size_t(kThreads) * 2000);
}

/* ---- 6. stop flag ----------------------------------------------------- */
static void demo_stop_flag() {
    std::printf("\n[6] atomic<bool> stop flag (a plain bool may be hoisted out of the loop)\n");
    std::atomic<bool> stop{false};
    std::atomic<long> work{0};
    std::thread worker([&] {
        // With a plain `bool stop`, the compiler may read it ONCE and spin
        // forever (data race = UB, it may assume nobody else writes it).
        while (!stop.load(std::memory_order_relaxed))
            work.fetch_add(1, std::memory_order_relaxed);
    });
    std::this_thread::sleep_for(2ms);
    stop.store(true, std::memory_order_relaxed); // relaxed is enough: no
    worker.join();                               // data rides on the flag
    std::printf("    worker stopped after %ld iterations (timing-dependent)\n", work.load());
    CHECK(work.load() > 0);
}

int main() {
    demo_rmw();
    demo_lock_free();
    demo_cas();
    demo_publish();
    demo_spinlock();
    demo_stop_flag();
    // C++20 additions, mentioned only (this repo builds C++17):
    //   std::atomic_ref<T>         atomic ops on a plain object you don't own
    //   a.wait(old) / notify_one() futex-style blocking on an atomic
    //   std::atomic<std::shared_ptr<T>>, std::atomic<float>::fetch_add
    std::printf("\n01_std_atomic: all checks passed\n");
    return 0;
}
