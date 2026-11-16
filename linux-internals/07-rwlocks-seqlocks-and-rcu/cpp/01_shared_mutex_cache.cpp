/*
 * 01_shared_mutex_cache.cpp — C++17 std::shared_mutex with std::shared_lock
 *                             (readers) and std::unique_lock (writers): a
 *                             thread-safe configuration cache
 *
 * Concept : std::shared_mutex is the standard reader-writer lock (on Linux
 *           libstdc++ wraps pthread_rwlock_t). shared_lock = "read lock"
 *           RAII guard, unique_lock / lock_guard = "write lock". The cache
 *           shows the three patterns you write over and over:
 *             get()            : shared lock, look up, return a COPY
 *             set()            : exclusive lock, modify
 *             get_or_compute() : shared lookup; on a miss DROP the shared
 *                                lock, take the exclusive one, CHECK AGAIN
 *                                (no upgrade exists: see the README)
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/01_shared_mutex_cache.cpp
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/01_shared_mutex_cache.cpp SAN=tsan
 * Read    : ../README.md  (section "C vs C++")
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
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

class ConfigCache {
public:
    // Returns a copy: a reference would outlive the lock (dangling the
    // moment a writer rehashes / erases). Copy out under the lock.
    std::optional<std::string> get(const std::string &key) const
    {
        std::shared_lock<std::shared_mutex> rd(m_); // many readers at once
        auto it = map_.find(key);
        if (it == map_.end())
            return std::nullopt;
        return it->second;
    }

    void set(const std::string &key, std::string value)
    {
        std::unique_lock<std::shared_mutex> wr(m_); // exclusive
        map_[key] = std::move(value);
        ++version_;
    }

    // Lazy fill. `compute` is expensive and must run at most once per key.
    template <class F>
    std::string get_or_compute(const std::string &key, F compute)
    {
        {
            std::shared_lock<std::shared_mutex> rd(m_);
            auto it = map_.find(key);
            if (it != map_.end())
                return it->second; // fast path: hit under the shared lock
        } // shared lock released here: we CANNOT upgrade it in place
        std::unique_lock<std::shared_mutex> wr(m_);
        auto it = map_.find(key); // re-check: someone may have filled it
        if (it != map_.end())     // between our two locks
            return it->second;
        std::string v = compute(key);
        map_.emplace(key, v);
        ++version_;
        return v;
    }

    // Several keys in ONE critical section: a consistent snapshot. Two
    // separate get() calls are two critical sections, and a writer may run
    // in between (the pair you get back may never have existed together).
    std::pair<std::string, std::string> get_pair(const std::string &k1,
                                                 const std::string &k2) const
    {
        std::shared_lock<std::shared_mutex> rd(m_);
        return {map_.at(k1), map_.at(k2)};
    }

    void set_pair(const std::string &k1, std::string v1, const std::string &k2,
                  std::string v2)
    {
        std::lock_guard<std::shared_mutex> wr(m_); // lock_guard works too
        map_[k1] = std::move(v1);
        map_[k2] = std::move(v2);
        ++version_;
    }

    // Hold the shared lock and call f() inside it (for the concurrency demo).
    template <class F>
    void with_read_lock(F f) const
    {
        std::shared_lock<std::shared_mutex> rd(m_);
        f();
    }

    unsigned long version() const
    {
        std::shared_lock<std::shared_mutex> rd(m_);
        return version_;
    }

private:
    mutable std::shared_mutex m_; // mutable: const readers still lock it
    std::map<std::string, std::string> map_;
    unsigned long version_ = 0;
};

int main()
{
    ConfigCache cache;
    cache.set("video.width", "1920");
    cache.set("video.height", "1080");
    CHECK(cache.get("video.width") == std::string("1920"));
    CHECK(!cache.get("audio.rate"));

    // 1. Readers really run concurrently: each waits INSIDE its shared lock
    //    until all 4 are inside. With a std::mutex this could never happen.
    std::atomic<int> inside{0}, max_inside{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([&] {
            cache.with_read_lock([&] {
                int now = ++inside;
                int m = max_inside.load();
                while (now > m && !max_inside.compare_exchange_weak(m, now)) {
                }
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                // wait on max_inside: `inside` drops when the first reader leaves
                while (max_inside.load() < 4 && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::yield();
                --inside;
            });
        });
    for (auto &t : ts)
        t.join();
    ts.clear();
    std::printf("max concurrent readers inside shared_lock: %d of 4\n", max_inside.load());
    CHECK(max_inside.load() == 4);

    // 2. get_or_compute from 6 threads on the same 3 keys: each value must
    //    be computed exactly once (the re-check under the unique lock).
    std::atomic<int> computations{0};
    for (int i = 0; i < 6; i++)
        ts.emplace_back([&] {
            for (int k = 0; k < 3; k++) {
                std::string key = "codec." + std::to_string(k);
                std::string v = cache.get_or_compute(key, [&](const std::string &kk) {
                    ++computations;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    return kk + "=h264";
                });
                CHECK(v == key + "=h264");
            }
        });
    for (auto &t : ts)
        t.join();
    ts.clear();
    std::printf("get_or_compute: 6 threads x 3 keys -> %d computations\n",
                computations.load());
    CHECK(computations.load() == 3);

    // 3. Stress: 3 readers, 1 writer keeping a + b == 100.
    //    get_pair() reads both under one shared lock: always consistent.
    //    Two separate get() calls may straddle a write: counted, printed,
    //    not asserted (it depends on timing) - that is the pitfall.
    cache.set_pair("a", "100", "b", "0");
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0}, bad{0}, torn_separate{0};
    for (int i = 0; i < 3; i++)
        ts.emplace_back([&] {
            while (!stop.load()) {
                auto [a, b] = cache.get_pair("a", "b");
                if (std::stoi(a) + std::stoi(b) != 100)
                    ++bad;
                int a2 = std::stoi(*cache.get("a")); // critical section 1
                int b2 = std::stoi(*cache.get("b")); // critical section 2
                if (a2 + b2 != 100)
                    ++torn_separate;
                ++reads;
            }
        });
    for (int i = 1; i <= 3000; i++)
        cache.set_pair("a", std::to_string(100 - i % 100), "b", std::to_string(i % 100));
    stop = true;
    for (auto &t : ts)
        t.join();
    std::printf("stress: %ld reads during 3000 writes; get_pair inconsistent: %ld, "
                "two separate get() inconsistent: %ld (timing-dependent)\n",
                reads.load(), bad.load(), torn_separate.load());
    CHECK(bad.load() == 0);

    std::printf("01_shared_mutex_cache: all checks passed\n");
    return 0;
}
