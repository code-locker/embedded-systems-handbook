/*
 * 08_hashed_timer_wheel.cpp — single-level hashed timing wheel with "rounds"
 *                             (Varghese & Lauck scheme 6; Netty HashedWheelTimer)
 *
 * Concept : The C version used several wheels with cascading. The other
 *           classic design keeps ONE wheel of S slots: a timer due in d ticks
 *           goes into slot (now+d) % S with rounds = d / S. Each tick visits
 *           one slot: rounds==0 entries fire, others decrement. Add is O(1);
 *           a tick costs O(entries in that slot). Cancellation is lazy (flag
 *           checked when the slot is visited) — cheap and lock-friendly.
 *           Checked against a std::priority_queue (min-heap) reference.
 * Build   : make run P=dsa/19-system-design-structures/cpp/08_hashed_timer_wheel.cpp
 * Read    : ../README.md  (section "Timer wheel")
 */
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <queue>
#include <random>
#include <utility>
#include <vector>

/*
 *   S = 8 slots, now = 3, add timer with d = 13  ->  expires 16
 *   slot = 16 % 8 = 0, rounds = 13 / 8 = 1
 *
 *        slot:  0    1    2    3    4    5    6    7
 *             [r=1]           ^now
 *   tick 8 visits slot 0: rounds 1 -> 0 (not yet)
 *   tick 16 visits slot 0: rounds 0 -> FIRE      (8 ticks = one revolution later)
 */
class TimerWheel {
public:
    class Handle {
    public:
        Handle() = default;
        void cancel() { if (auto s = flag_.lock()) *s = true; }
    private:
        friend class TimerWheel;
        explicit Handle(std::weak_ptr<bool> f) : flag_(std::move(f)) {}
        std::weak_ptr<bool> flag_;     // weak: a handle kept after firing is harmless
    };

    explicit TimerWheel(std::size_t slots) : slots_(slots) {}

    std::uint64_t now() const { return now_; }

    // O(1). delay 0 means "on the next tick processed".
    Handle add(std::uint64_t delay, std::function<void()> fn) {
        std::uint64_t expires = now_ + delay;
        auto flag = std::make_shared<bool>(false);
        slots_[expires % slots_.size()].push_back(Entry{delay / slots_.size(), std::move(fn), flag});
        ++pending_;
        return Handle(flag);
    }

    // Process ticks now_ .. target inclusive.
    void advance_to(std::uint64_t target) {
        while (now_ <= target) {
            std::uint64_t cur = now_++;
            auto& slot = slots_[cur % slots_.size()];
            // Detach the slot first: callbacks may add timers that hash to this
            // very slot; they must NOT be seen in this pass.
            std::list<Entry> work;
            work.splice(work.end(), slot);
            for (auto it = work.begin(); it != work.end();) {
                if (*it->cancelled) { it = work.erase(it); --pending_; continue; }
                if (it->rounds > 0) { --it->rounds; ++it; continue; }
                auto fn = std::move(it->fn);
                it = work.erase(it);
                --pending_;
                fire_tick_ = cur;
                fn();
            }
            slot.splice(slot.end(), work);       // survivors go back
        }
    }

    std::uint64_t firing_tick() const { return fire_tick_; }
    std::size_t pending() const { return pending_; }

private:
    struct Entry {
        std::uint64_t rounds;
        std::function<void()> fn;
        std::shared_ptr<bool> cancelled;
    };
    std::vector<std::list<Entry>> slots_;
    std::uint64_t now_ = 0, fire_tick_ = 0;
    std::size_t pending_ = 0;
};

int main() {
    std::cout << "== TCP-style retransmit timer with backoff (re-arms from callback) ==\n";
    TimerWheel w(8);
    std::vector<std::uint64_t> rto_fires;
    std::uint64_t rto = 3;
    std::function<void()> retransmit = [&] {
        rto_fires.push_back(w.firing_tick());
        if (rto_fires.size() < 4) { rto *= 2; w.add(rto, retransmit); }
    };
    // delay counted from now_ (= next tick), so fire tick = now_ + delay.
    w.add(rto, retransmit);
    w.advance_to(100);
    for (auto t : rto_fires) std::cout << "  retransmit at tick " << t << "\n";
    // fires at 3; re-armed during tick 3 with now_=4 -> 4+6=10; then 11+12=23; then 24+24=48
    assert((rto_fires == std::vector<std::uint64_t>{3, 10, 23, 48}));

    std::cout << "\n== cancel before expiry (the common case for timeouts) ==\n";
    bool fired = false;
    auto h = w.add(20, [&] { fired = true; });
    w.advance_to(w.now() + 10);
    h.cancel();
    w.advance_to(w.now() + 50);
    assert(!fired && w.pending() == 0);
    std::cout << "cancelled timer never fired; lazily removed when its slot was visited\n";

    std::cout << "\n== 20000 random timers vs a min-heap reference ==\n";
    TimerWheel big(256);
    std::mt19937 rng(99);
    using Ev = std::pair<std::uint64_t, int>;               // (tick, id)
    std::vector<Ev> got;
    std::priority_queue<Ev, std::vector<Ev>, std::greater<Ev>> ref;
    std::vector<TimerWheel::Handle> handles;
    std::vector<std::uint64_t> due;
    for (int id = 0; id < 20000; ++id) {
        if (id % 2000 == 0) big.advance_to(big.now() + rng() % 300);   // interleave
        std::uint64_t d = (rng() % 4 == 0) ? rng() % 20000 : rng() % 600;
        due.push_back(big.now() + d);
        handles.push_back(big.add(d, [&got, &big, id] { got.emplace_back(big.firing_tick(), id); }));
    }
    std::vector<char> cancelled(handles.size(), 0);
    for (int k = 0; k < 3000; ++k) {                         // cancel random timers up front
        std::size_t i = rng() % handles.size();
        // only "valid" cancels: those not yet fired at this point
        if (due[i] >= big.now()) { handles[i].cancel(); cancelled[i] = 1; }
    }
    for (std::size_t i = 0; i < due.size(); ++i)
        if (!cancelled[i]) ref.emplace(due[i], static_cast<int>(i));
    big.advance_to(big.now() + 21000);
    std::vector<Ev> want;
    while (!ref.empty()) { want.push_back(ref.top()); ref.pop(); }
    std::sort(got.begin(), got.end());                       // order within a tick is unspecified
    std::cout << "wheel fired " << got.size() << ", heap expects " << want.size() << "\n";
    assert(got == want);                                     // same timers, same exact ticks
    assert(big.pending() == 0);
    std::cout << "OK\n";
    return 0;
}
