/*
 * 01_periodic_task.cpp — PeriodicTask: a periodic loop with std::chrono
 *                        absolute deadlines, overrun detection and a
 *                        "skip missed periods" policy
 *
 * Concept : The C lesson (c/05) showed that a periodic loop must sleep
 *           until an ABSOLUTE release time. Here that idea becomes a small
 *           class: the release grid is a std::chrono::steady_clock
 *           time_point (on Linux/libstdc++ that IS CLOCK_MONOTONIC), the
 *           sleep is clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME) behind
 *           a type-safe wrapper, and every cycle is checked: if the job
 *           finished after the NEXT release, that is an overrun. Policy on
 *           overrun: count it and skip the missed releases (stay on the
 *           grid) instead of bursting several jobs back to back.
 *           std::this_thread::sleep_until(steady_clock) also works; libstdc++
 *           11 implements it as a loop of relative sleeps, which still does
 *           not drift (the target is absolute) but costs extra syscalls.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/cpp/01_periodic_task.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <utility>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fflush(stdout);                                             \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// libstdc++ on Linux implements steady_clock with CLOCK_MONOTONIC, so its
// time_since_epoch() is directly a CLOCK_MONOTONIC timespec. That is what
// lets us hand a steady_clock::time_point to clock_nanosleep.
static_assert(Clock::is_steady, "steady_clock must not jump");

// Sleep until an absolute steady_clock time point. Returns immediately if
// the time is already past (that is how an overrun "catches up").
static void sleep_until_abs(Clock::time_point tp)
{
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
    timespec ts{static_cast<time_t>(ns / 1'000'000'000), static_cast<long>(ns % 1'000'000'000)};
    int rc;
    // Returns the error number; EINTR -> just sleep again to the SAME time.
    while ((rc = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr)) == EINTR) {
    }
    CHECK(rc == 0);
}

class PeriodicTask {
public:
    struct Stats {
        unsigned jobs = 0;          // jobs executed
        unsigned overruns = 0;      // jobs that ended after the next release
        unsigned skipped = 0;       // releases dropped to get back on the grid
        Clock::duration max_late{}; // worst wakeup lateness
        Clock::duration max_exec{}; // worst job execution time
    };

    // The job gets its cycle index. std::function keeps the class simple; a
    // template parameter would avoid the indirect call in a hot RT loop.
    PeriodicTask(Clock::duration period, std::function<void(unsigned)> job)
        : period_(period), job_(std::move(job))
    {
        CHECK(period_ > Clock::duration::zero());
    }

    // Run until `releases` grid points have passed (jobs + skipped).
    Stats run(unsigned releases)
    {
        Stats s;
        auto next = Clock::now() + period_; // first release one period from now
        unsigned k = 0;
        while (k < releases) {
            sleep_until_abs(next);
            auto woke = Clock::now();
            s.max_late = std::max(s.max_late, woke - next);

            job_(k);
            ++s.jobs;
            ++k;
            auto done = Clock::now();
            s.max_exec = std::max(s.max_exec, done - woke);

            next += period_;
            if (done > next) {
                // Overrun: the job ran past the next release.
                //   grid:  |----|----|----|----|
                //   job :  [=========]           <- ended in the 3rd slot
                //   skip the releases already in the past, resume on the grid
                ++s.overruns;
                while (next < done && k < releases) {
                    next += period_;
                    ++s.skipped;
                    ++k;
                }
            }
        }
        return s;
    }

private:
    Clock::duration period_;
    std::function<void(unsigned)> job_;
};

static void spin_for(Clock::duration d)
{
    auto end = Clock::now() + d;
    while (Clock::now() < end) {
    }
}

static double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

int main()
{
    alarm(10);

    // ---- 1. a well-behaved 2 ms task with 0.3 ms of work ----
    PeriodicTask ok(2ms, [](unsigned) { spin_for(300us); });
    auto t0 = Clock::now();
    auto s1 = ok.run(100);
    auto total = Clock::now() - t0;
    std::printf("1) 2 ms task, 100 releases: %u jobs, %u overruns, total %.1f ms "
                "(ideal 200), worst wakeup %.3f ms late, worst job %.3f ms\n",
                s1.jobs, s1.overruns, ms(total), ms(s1.max_late), ms(s1.max_exec));
    CHECK(s1.jobs + s1.skipped == 100);
    // The loop ends one wakeup latency after the 100th release: no drift.
    // (20 ms = 10 periods: loose enough for a loaded host under TSan.)
    CHECK(total >= 200ms && total < 220ms);

    // ---- 2. a task that sometimes takes 5 ms (2.5 periods) ----
    // Count JOBS (not releases) so "every 10th job" stays exact even when
    // releases are skipped. mutable: the lambda owns its counter.
    PeriodicTask bursty(2ms, [n = 0u](unsigned) mutable { spin_for(++n % 10 == 0 ? 5ms : 200us); });
    t0 = Clock::now();
    auto s2 = bursty.run(100);
    total = Clock::now() - t0;
    std::printf("2) every 10th job takes 5 ms: %u jobs, %u overruns, %u releases skipped,\n"
                "   total %.1f ms (still ~200: the grid is kept), worst job %.3f ms\n",
                s2.jobs, s2.overruns, s2.skipped, ms(total), ms(s2.max_exec));
    CHECK(s2.jobs + s2.skipped == 100);
    const unsigned long_jobs = s2.jobs / 10;
    CHECK(s2.overruns >= long_jobs);    // a 5 ms job always overruns a 2 ms period
    CHECK(s2.skipped >= 2 * long_jobs - 2); // and spans >= 2 releases (last may end the run)
    CHECK(total >= 200ms && total < 230ms);

    // ---- 3. the standard-library way, for comparison ----
    auto next = Clock::now();
    t0 = next;
    for (int i = 0; i < 50; ++i) {
        spin_for(300us);
        next += 2ms;
        std::this_thread::sleep_until(next); // absolute target: no drift either
    }
    total = Clock::now() - t0;
    std::printf("3) std::this_thread::sleep_until loop, 50 x 2 ms: %.1f ms (ideal 100)\n", ms(total));
    CHECK(total >= 100ms && total < 120ms);

    std::printf("01_periodic_task: all checks passed\n");
    return 0;
}
