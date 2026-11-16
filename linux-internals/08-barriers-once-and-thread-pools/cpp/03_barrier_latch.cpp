/*
 * 03_barrier_latch.cpp — C++17 versions of C++20's std::latch and
 *                        std::barrier (with a completion function), built
 *                        from std::mutex + std::condition_variable
 *
 * Concept : A LATCH is a one-shot countdown: count_down() N times, then
 *           every wait() returns, forever (e.g. "all 4 camera threads have
 *           opened their device"). A BARRIER is reusable: N threads meet,
 *           an optional completion step runs ONCE while they are all held,
 *           then all continue to the next phase. The generation counter
 *           from c/02_reusable_barrier.c makes the reuse safe. In C++20 use
 *           <latch> and <barrier> directly; the interfaces below match them.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/03_barrier_latch.cpp
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/03_barrier_latch.cpp SAN=tsan
 * Read    : ../README.md  (sections "A reusable barrier", "C vs C++")
 */
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
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

class Latch { // like C++20 std::latch
public:
    explicit Latch(long count) : count_(count) {}
    void count_down(long n = 1)
    {
        std::lock_guard<std::mutex> g(m_);
        count_ -= n;
        if (count_ <= 0)
            cv_.notify_all();
    }
    void wait()
    {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return count_ <= 0; });
    }
    bool try_wait()
    {
        std::lock_guard<std::mutex> g(m_);
        return count_ <= 0;
    }
    void arrive_and_wait()
    {
        count_down();
        wait();
    }

private:
    long count_;
    std::mutex m_;
    std::condition_variable cv_;
};

class Barrier { // like C++20 std::barrier<CompletionFunction>
public:
    explicit Barrier(unsigned count, std::function<void()> completion = {})
        : count_(count), completion_(std::move(completion)) {}

    void arrive_and_wait()
    {
        std::unique_lock<std::mutex> lk(m_);
        const unsigned long my_gen = gen_;
        if (++waiting_ == count_) {
            // Last arriver: everyone else is blocked in wait() below, so the
            // completion step runs alone and its effects are visible to all
            // threads of the next phase (they reacquire m_ after it).
            if (completion_)
                completion_();
            waiting_ = 0;
            ++gen_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lk, [&] { return gen_ != my_gen; }); // generation, not count!
    }

    unsigned long generation()
    {
        std::lock_guard<std::mutex> g(m_);
        return gen_;
    }

private:
    const unsigned count_;
    std::function<void()> completion_;
    unsigned waiting_ = 0;
    unsigned long gen_ = 0;
    std::mutex m_;
    std::condition_variable cv_;
};

int main()
{
    // 1. Latch: workers "open devices", main waits until all are ready.
    const int W = 4;
    Latch ready(W);
    std::atomic<int> opened{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < W; i++)
        ts.emplace_back([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            ++opened;
            ready.count_down();
        });
    ready.wait();
    std::printf("latch released: %d of %d devices opened\n", opened.load(), W);
    CHECK(opened.load() == W); // count_down happens-before wait() returns
    CHECK(ready.try_wait());   // stays open forever (one-shot)
    for (auto &t : ts)
        t.join();
    ts.clear();

    // 2. Barrier with a completion step: a 2-buffer pipeline. In each phase
    //    every worker fills its part of `next` from `cur`; the completion
    //    step (one thread, all others held) swaps the buffers and records
    //    the total. Exactly what std::barrier's completion function is for.
    const int PHASES = 500;
    std::vector<long> a(W, 1), b(W, 0);
    std::vector<long> *cur = &a, *next = &b;
    std::vector<long> totals; // written only by the completion step
    Barrier bar(W, [&] {
        long t = 0;
        for (long v : *next)
            t += v;
        totals.push_back(t);
        std::swap(cur, next);
    });
    for (int id = 0; id < W; id++)
        ts.emplace_back([&, id] {
            for (int p = 0; p < PHASES; p++) {
                // each element: sum of my left neighbour and me, mod 1000
                long left = (*cur)[(id + W - 1) % W];
                (*next)[id] = (left + (*cur)[id]) % 1000;
                bar.arrive_and_wait();
            }
        });
    for (auto &t : ts)
        t.join();

    // Same computation, single-threaded, to verify.
    std::vector<long> ra(W, 1), rb(W, 0);
    std::vector<long> ref_totals;
    for (int p = 0; p < PHASES; p++) {
        for (int id = 0; id < W; id++)
            rb[id] = (ra[(id + W - 1) % W] + ra[id]) % 1000;
        long t = 0;
        for (long v : rb)
            t += v;
        ref_totals.push_back(t);
        std::swap(ra, rb);
    }
    std::printf("barrier: %d phases x %d threads, generation %lu, final total "
                "%ld (reference %ld)\n", PHASES, W, bar.generation(),
                totals.back(), ref_totals.back());
    CHECK(bar.generation() == (unsigned long)PHASES);
    CHECK(totals == ref_totals); // every phase matched
    CHECK(*cur == ra);

    std::printf("03_barrier_latch: all checks passed\n");
    return 0;
}
