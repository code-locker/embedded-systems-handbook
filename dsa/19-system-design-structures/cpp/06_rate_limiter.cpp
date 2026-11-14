/*
 * 06_rate_limiter.cpp — token bucket with std::chrono and an injectable clock,
 *                       a per-client limiter (map of buckets), and a sliding-window log
 *
 * Concept : Production limiters are (1) keyed — one bucket per client IP or
 *           API key, like nginx limit_req or a cloud API gateway — and
 *           (2) thread-safe. Making the clock a template parameter lets the
 *           same code run on std::chrono::steady_clock in production and on
 *           a fake clock in tests, so tests are exact and instant.
 * Build   : make run P=dsa/19-system-design-structures/cpp/06_rate_limiter.cpp
 * Read    : ../README.md  (section "Rate limiter: token bucket")
 */
#include <cassert>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;
using Nanos = std::chrono::nanoseconds;

// A clock we drive by hand. Same interface shape as steady_clock::now().
struct FakeClock {
    static inline Nanos t{0};
    static Nanos now() { return t; }
};
struct RealClock {
    static Nanos now() {
        return std::chrono::duration_cast<Nanos>(std::chrono::steady_clock::now().time_since_epoch());
    }
};

template <typename Clock>
class TokenBucket {
public:
    // per_token: time to earn one token (1/rate). burst: bucket size.
    TokenBucket(Nanos per_token, std::int64_t burst)
        : per_token_(per_token), cap_(per_token * burst), credit_(cap_), last_(Clock::now()) {}

    bool allow(std::int64_t cost = 1) {
        std::lock_guard<std::mutex> lk(m_);       // refill + take must be atomic together
        Nanos now = Clock::now();
        credit_ = std::min(cap_, credit_ + (now - last_));
        last_ = now;
        Nanos need = per_token_ * cost;
        if (credit_ < need) return false;
        credit_ -= need;
        return true;
    }

    // How long until `cost` tokens are available (for a Retry-After header).
    Nanos wait_time(std::int64_t cost = 1) {
        std::lock_guard<std::mutex> lk(m_);
        Nanos have = std::min(cap_, credit_ + (Clock::now() - last_));
        Nanos need = per_token_ * cost;
        return have >= need ? Nanos{0} : need - have;
    }

private:
    std::mutex m_;
    Nanos per_token_, cap_, credit_, last_;
};

// One bucket per client. unordered_map node addresses are stable, so a
// reference to a bucket stays valid while other clients are inserted.
template <typename Clock>
class KeyedLimiter {
public:
    KeyedLimiter(Nanos per_token, std::int64_t burst) : per_token_(per_token), burst_(burst) {}
    bool allow(const std::string& client) {
        TokenBucket<Clock>* b;
        {
            std::lock_guard<std::mutex> lk(m_);   // protects the map only
            auto it = buckets_.try_emplace(client, per_token_, burst_).first;
            b = &it->second;
        }
        return b->allow();                        // bucket has its own lock
    }
    std::size_t clients() const { return buckets_.size(); }
private:
    std::mutex m_;
    Nanos per_token_;
    std::int64_t burst_;
    std::unordered_map<std::string, TokenBucket<Clock>> buckets_;
};

// Sliding-window LOG: exact "at most N in any window W", but O(N) memory per
// client — the price of exactness.
class SlidingWindowLog {
public:
    SlidingWindowLog(Nanos window, std::size_t limit) : w_(window), limit_(limit) {}
    bool allow(Nanos now) {
        while (!log_.empty() && now - log_.front() >= w_) log_.pop_front();
        if (log_.size() >= limit_) return false;
        log_.push_back(now);
        return true;
    }
private:
    Nanos w_;
    std::size_t limit_;
    std::deque<Nanos> log_;
};

int main() {
    std::cout << "== token bucket, 5 req/s, burst 3, fake clock ==\n";
    FakeClock::t = 0ns;
    TokenBucket<FakeClock> tb(200ms, 3);
    int ok = 0;
    for (int i = 0; i < 5; ++i) ok += tb.allow();
    std::cout << "5 requests at t=0: " << ok << " allowed\n";
    assert(ok == 3);
    std::cout << "retry after " << std::chrono::duration_cast<std::chrono::milliseconds>(tb.wait_time()).count() << " ms\n";
    assert(tb.wait_time() == 200ms);
    FakeClock::t = 150ms;
    assert(!tb.allow() && tb.wait_time() == 50ms);
    FakeClock::t = 200ms;
    assert(tb.allow());

    std::cout << "\n== per-client limiting: a noisy client cannot starve others ==\n";
    FakeClock::t = 0ns;
    KeyedLimiter<FakeClock> kl(100ms, 2);
    int noisy = 0, quiet = 0;
    for (int ms = 0; ms < 1000; ms += 10) {          // noisy: 100 req/s
        FakeClock::t = Nanos(std::chrono::milliseconds(ms));
        noisy += kl.allow("10.0.0.66");
        if (ms % 250 == 0) quiet += kl.allow("10.0.0.7");   // quiet: 4 req/s
    }
    std::cout << "noisy allowed " << noisy << "/100, quiet allowed " << quiet << "/4, clients="
              << kl.clients() << "\n";
    assert(noisy == 2 + 9 && quiet == 4);           // burst 2 + refills at 100..900ms

    std::cout << "\n== thread safety: 8 threads x 1000 requests at a frozen instant ==\n";
    FakeClock::t = 0ns;
    TokenBucket<FakeClock> shared(1ms, 500);
    std::vector<std::thread> th;
    std::vector<int> per(8, 0);
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] { for (int i = 0; i < 1000; ++i) per[static_cast<std::size_t>(t)] += shared.allow(); });
    for (auto& x : th) x.join();
    int total = 0;
    for (int p : per) total += p;
    std::cout << "allowed " << total << " (exactly the burst: no token was double-spent)\n";
    assert(total == 500);

    std::cout << "\n== token bucket vs sliding-window log on random traffic ==\n";
    // Bucket(burst B, rate B/W) and log(limit B per W) agree on the long-run
    // rate but differ in shape: the log forbids B+1 in ANY window W, the bucket
    // allows B + (refill during W). Check the bucket's bound and the log's.
    std::mt19937 rng(3);
    FakeClock::t = 0ns;
    TokenBucket<FakeClock> b2(100ms, 10);
    SlidingWindowLog lg(1s, 10);
    std::vector<Nanos> ta, tl;
    for (int i = 0; i < 3000; ++i) {
        FakeClock::t += Nanos(std::chrono::milliseconds(rng() % 20));
        if (b2.allow()) ta.push_back(FakeClock::t);
        if (lg.allow(FakeClock::t)) tl.push_back(FakeClock::t);
    }
    for (std::size_t i = 0; i < tl.size(); ++i)            // log: <=10 in any 1 s
        for (std::size_t j = i; j < tl.size() && tl[j] - tl[i] < 1s; ++j) assert(j - i + 1 <= 10);
    std::size_t worst = 0;
    for (std::size_t i = 0; i < ta.size(); ++i)            // bucket: <= 10 + 10 per 1 s
        for (std::size_t j = i; j < ta.size() && ta[j] - ta[i] < 1s; ++j) worst = std::max(worst, j - i + 1);
    std::cout << "bucket allowed " << ta.size() << ", log allowed " << tl.size()
              << "; worst 1 s window: bucket " << worst << " (bound 20), log <= 10\n";
    assert(worst <= 20);

    std::cout << "\n== real clock sanity (steady_clock) ==\n";
    TokenBucket<RealClock> real(1s, 2);
    assert(real.allow() && real.allow() && !real.allow());   // < 1 s elapses here
    std::cout << "burst 2 then reject\n";
    std::cout << "OK\n";
    return 0;
}
