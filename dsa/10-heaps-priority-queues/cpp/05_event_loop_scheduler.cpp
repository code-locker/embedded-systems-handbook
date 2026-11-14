/*
 * 05_event_loop_scheduler.cpp — a single-threaded event loop with timers built
 *                               on std::priority_queue and LAZY cancellation
 *
 * Concept : The C version (../c/06_timer_queue.c) cancels in O(log n) by
 *           tracking each timer's heap index. std::priority_queue cannot
 *           remove from the middle, so the common C++ trick is lazy deletion:
 *           cancel just marks the timer dead; the loop discards dead entries
 *           when they reach the top. Same idea as Python's asyncio and many
 *           game engines. The shape mirrors GStreamer's clock waits: a
 *           gst_clock_id_wait_async() is "call me at time T", and
 *           gst_clock_id_unschedule() is cancel. Simulated clock: no sleeping.
 * Build   : make run P=dsa/10-heaps-priority-queues/cpp/05_event_loop_scheduler.cpp
 * Read    : ../README.md  (section "Timer queues and schedulers")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using Time = std::uint64_t;   // simulated nanoseconds
using TimerId = std::uint64_t;

class EventLoop {
public:
    using Callback = std::function<void(EventLoop&, TimerId)>;

    // O(log n). Returns a handle the caller can cancel with.
    TimerId call_at(Time when, Callback cb)
    {
        TimerId id = next_id_++;
        live_.emplace(id, std::move(cb));          // callbacks stored by id: heap stays small
        heap_.push(Entry{when, id});
        return id;
    }
    TimerId call_later(Time delay, Callback cb) { return call_at(now_ + delay, std::move(cb)); }

    // O(1) average: just forget the callback. The heap entry becomes a tombstone.
    bool cancel(TimerId id) { return live_.erase(id) == 1; }

    // O(1) amortised: drop tombstones at the top, then peek.
    // This is the "poll timeout" an epoll-based loop would compute.
    std::optional<Time> next_deadline()
    {
        purge_top();
        if (heap_.empty()) return std::nullopt;
        return heap_.top().when;
    }

    // Run until no timers remain or the clock passes 'until'. Each iteration:
    // compute the deadline, "sleep" (jump the clock), fire everything due.
    void run(Time until)
    {
        while (auto d = next_deadline()) {
            if (*d > until) break;
            now_ = *d;                              // real code: epoll_wait(timeout)
            while (!heap_.empty() && heap_.top().when <= now_) {
                Entry e = heap_.top();
                heap_.pop();                        // O(log n)
                auto it = live_.find(e.id);
                if (it == live_.end()) { ++tombstones_skipped_; continue; }
                Callback cb = std::move(it->second);
                live_.erase(it);                    // erase BEFORE calling: cb may re-arm
                cb(*this, e.id);
            }
        }
        now_ = std::max(now_, until);
    }

    Time now() const { return now_; }
    std::size_t pending() const { return live_.size(); }
    std::size_t heap_entries() const { return heap_.size(); }
    std::size_t tombstones_skipped() const { return tombstones_skipped_; }

private:
    struct Entry {
        Time when;
        TimerId id;       // ids grow monotonically, so (when, id) is FIFO for ties
        bool operator>(const Entry& o) const { return when != o.when ? when > o.when : id > o.id; }
    };

    void purge_top()
    {
        while (!heap_.empty() && !live_.count(heap_.top().id)) {
            heap_.pop();
            ++tombstones_skipped_;
        }
    }

    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> heap_;  // min-heap
    std::unordered_map<TimerId, Callback> live_;
    Time now_ = 0;
    TimerId next_id_ = 1;
    std::size_t tombstones_skipped_ = 0;
};

int main()
{
    // ---- demo: a pipeline-like scenario -------------------------------------
    EventLoop loop;
    std::vector<std::string> log;
    auto say = [&](std::string s) { return [&log, s](EventLoop& l, TimerId) {
        std::cout << "  t=" << l.now() << "  " << s << '\n';
        log.push_back(s);
    }; };

    // A periodic "render a frame every 33 ms" that re-arms itself.
    int frames = 0;
    std::function<void(EventLoop&, TimerId)> frame = [&](EventLoop& l, TimerId) {
        std::cout << "  t=" << l.now() << "  frame " << frames << '\n';
        if (++frames < 4) l.call_later(33, frame);
    };
    loop.call_at(0, frame);
    loop.call_at(50, say("buffer at running-time 50 ready"));
    TimerId seek_wait = loop.call_at(70, say("THIS MUST NOT PRINT"));
    loop.call_at(70, say("audio tick"));
    loop.call_at(60, [&](EventLoop& l, TimerId) {
        std::cout << "  t=" << l.now() << "  seek: unschedule pending wait\n";
        assert(l.cancel(seek_wait));
    });
    std::cout << "event loop:\n";
    loop.run(1000);
    assert(frames == 4);
    assert((log == std::vector<std::string>{"buffer at running-time 50 ready", "audio tick"}));
    assert(loop.tombstones_skipped() == 1);
    assert(loop.pending() == 0 && loop.heap_entries() == 0);

    // ---- randomised check against a brute-force reference --------------------
    std::mt19937_64 rng(31337);
    for (int round = 0; round < 30; ++round) {
        EventLoop el;
        std::vector<TimerId> fired;
        struct Ref { Time when; TimerId id; bool alive; };
        std::vector<Ref> ref;
        Time horizon = 0;
        for (int step = 0; step < 400; ++step) {
            unsigned op = static_cast<unsigned>(rng() % 10);
            if (op < 6) {
                Time when = el.now() + rng() % 500;
                TimerId id = el.call_at(when, [&fired](EventLoop&, TimerId tid) { fired.push_back(tid); });
                ref.push_back({when, id, true});
            } else if (op < 8 && !ref.empty()) {
                Ref& r = ref[rng() % ref.size()];
                bool was_pending = r.alive;            // not yet fired, not cancelled
                bool ok = el.cancel(r.id);
                assert(ok == was_pending);      // cancelling a fired/cancelled timer is a no-op
                r.alive = false;
            } else {
                horizon = el.now() + rng() % 200;
                // expected: alive timers with when in (now, horizon], by (when, id)
                std::vector<Ref> due;
                for (auto& r : ref)
                    if (r.alive && r.when <= horizon) { due.push_back(r); r.alive = false; }
                std::sort(due.begin(), due.end(), [](const Ref& a, const Ref& b) {
                    return a.when != b.when ? a.when < b.when : a.id < b.id;
                });
                fired.clear();
                el.run(horizon);
                assert(fired.size() == due.size());
                for (std::size_t i = 0; i < due.size(); ++i) assert(fired[i] == due[i].id);
            }
        }
    }
    std::cout << "30 random rounds of add/cancel/run match the brute-force reference\n";
    std::cout << "all checks passed\n";
    return 0;
}
