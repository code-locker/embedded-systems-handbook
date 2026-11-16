/*
 * 02_thread_pool.cpp — a C++17 thread pool: std::function tasks, results
 *                      through std::future via std::packaged_task, exceptions
 *                      carried back to the caller, and RAII shutdown in the
 *                      destructor
 *
 * Concept : Same machinery as c/04_thread_pool.c (queue + mutex + condvar +
 *           N workers), but C++ removes three chores:
 *             - any callable with any arguments becomes a task (templates +
 *               std::function<void()>),
 *             - submit() returns std::future<R>: the caller gets the result
 *               (or the exception the task threw) with .get(),
 *             - the destructor drains the queue and joins the workers, so a
 *               pool can never be "forgotten" with threads still running.
 *           Then a parallel_for / parallel sum built on top of it.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/02_thread_pool.cpp
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/02_thread_pool.cpp SAN=tsan
 * Read    : ../README.md  (section "Thread pool")
 */
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

class ThreadPool {
public:
    explicit ThreadPool(unsigned n)
    {
        for (unsigned i = 0; i < n; i++)
            workers_.emplace_back([this] { run(); });
    }

    // RAII: finish everything queued, then join. No task is lost, no thread
    // outlives the pool object.
    ~ThreadPool()
    {
        {
            std::lock_guard<std::mutex> g(m_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto &t : workers_)
            t.join();
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;

    // submit(f, args...) -> std::future<result of f(args...)>
    template <class F, class... Args>
    auto submit(F &&f, Args &&...args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using R = std::invoke_result_t<F, Args...>;
        // packaged_task is move-only but std::function needs a copyable
        // callable, hence the shared_ptr wrapper.
        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<R> fut = task->get_future();
        {
            std::lock_guard<std::mutex> g(m_);
            if (stopping_)
                throw std::runtime_error("submit on a stopping ThreadPool");
            q_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    unsigned size() const { return unsigned(workers_.size()); }

private:
    void run()
    {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [this] { return stopping_ || !q_.empty(); });
                if (q_.empty()) // stopping_ and nothing left: exit
                    return;
                job = std::move(q_.front());
                q_.pop_front();
            }
            // Run outside the lock. packaged_task catches exceptions and
            // stores them in the future, so a throwing task cannot kill
            // the worker thread (an uncaught exception would std::terminate).
            job();
        }
    }

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> q_; // under m_
    bool stopping_ = false;               // under m_
    std::mutex m_;
    std::condition_variable cv_;
};

// parallel_for on top of the pool: split [0, n) into one chunk per worker,
// wait for all futures. f(begin, end) must touch only its own range.
template <class F>
void parallel_for(ThreadPool &pool, std::size_t n, F f)
{
    const std::size_t parts = pool.size();
    std::vector<std::future<void>> fs;
    for (std::size_t p = 0; p < parts; p++) {
        std::size_t b = p * n / parts, e = (p + 1) * n / parts;
        fs.push_back(pool.submit(f, b, e));
    }
    for (auto &fu : fs)
        fu.get(); // waits; rethrows if a chunk threw
}

static std::uint64_t slow_square(std::uint64_t x)
{
    std::this_thread::sleep_for(std::chrono::microseconds(200));
    return x * x;
}

int main()
{
    const unsigned nw = std::max(2u, std::min(4u, std::thread::hardware_concurrency()));
    std::atomic<int> ran_after_destroy{0};
    {
        ThreadPool pool(nw);

        // 1. Futures carry results back.
        std::vector<std::future<std::uint64_t>> fs;
        for (std::uint64_t i = 0; i < 100; i++)
            fs.push_back(pool.submit(slow_square, i));
        std::uint64_t sum = 0;
        for (auto &f : fs)
            sum += f.get();
        std::printf("100 tasks on %u workers: sum of squares %llu\n", nw,
                    (unsigned long long)sum);
        CHECK(sum == 99ULL * 100 * 199 / 6);

        // 2. Exceptions travel through the future too.
        auto bad = pool.submit([]() -> int { throw std::runtime_error("sensor offline"); });
        try {
            bad.get();
            CHECK(!"expected an exception");
        } catch (const std::runtime_error &e) {
            std::printf("task exception delivered to caller: \"%s\"\n", e.what());
        }
        // ...and the pool still works afterwards.
        CHECK(pool.submit([] { return 7; }).get() == 7);

        // 3. parallel_for: parallel sum of an array, verified.
        std::vector<std::uint32_t> data(1 << 20);
        std::iota(data.begin(), data.end(), 1u);
        std::vector<std::uint64_t> partial(pool.size(), 0);
        std::atomic<unsigned> slot{0};
        parallel_for(pool, data.size(), [&](std::size_t b, std::size_t e) {
            std::uint64_t s = 0;
            for (std::size_t i = b; i < e; i++)
                s += data[i];
            partial[slot++] = s; // each chunk writes its own slot
        });
        std::uint64_t total = std::accumulate(partial.begin(), partial.end(), std::uint64_t{0});
        const std::uint64_t n = data.size();
        std::printf("parallel_for sum of 1..%llu = %llu\n", (unsigned long long)n,
                    (unsigned long long)total);
        CHECK(total == n * (n + 1) / 2);

        // 4. Queue fire-and-forget work, then let the destructor drain it.
        for (int i = 0; i < 500; i++)
            pool.submit([&] { ++ran_after_destroy; }); // future discarded: fine
    } // ~ThreadPool: all 500 still run before the workers are joined
    std::printf("tasks still queued at destruction that ran anyway: %d of 500\n",
                ran_after_destroy.load());
    CHECK(ran_after_destroy.load() == 500);

    std::printf("02_thread_pool: all checks passed\n");
    return 0;
}
