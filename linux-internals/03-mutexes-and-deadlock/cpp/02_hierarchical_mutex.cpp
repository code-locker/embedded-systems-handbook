/*
 * 02_hierarchical_mutex.cpp — enforce a lock order at run time: a mutex
 *                             wrapper that refuses to be locked "upwards"
 *
 * Concept : Lock ordering prevents deadlock only if EVERYONE follows it, and
 *           people forget. Give every mutex a level; a thread may only lock
 *           a mutex whose level is LOWER than the lowest one it already
 *           holds. A violation throws immediately, on the first run, with a
 *           stack trace — instead of deadlocking once a month on a customer
 *           board. (Idea from Anthony Williams, "C++ Concurrency in Action".)
 *           The Linux kernel does the same at debug time with LOCKDEP:
 *           it learns the order of lock CLASSES and warns "possible circular
 *           locking dependency detected" the first time an inversion occurs.
 *           The class satisfies the Lockable requirements, so lock_guard,
 *           unique_lock and scoped_lock all work with it.
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/cpp/02_hierarchical_mutex.cpp
 *           make run P=linux-internals/03-mutexes-and-deadlock/cpp/02_hierarchical_mutex.cpp SAN=tsan
 * Read    : ../README.md  (section 3.9 "Avoiding deadlock")
 */
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
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

/*
 *   level 1000  app_state      lock order: always DOWNWARDS
 *   level  500  device_list         1000 -> 500 -> 100   ok
 *   level  100  log_buffer          100  -> 500          throws
 *
 *   per thread: current_level (thread_local), starts at ULONG_MAX
 *   lock(m):  require m.level < current_level
 *             save previous level in m, current_level = m.level
 *   unlock(m): current_level = saved previous level
 */
class HierarchicalMutex {
public:
    explicit HierarchicalMutex(unsigned long level) : level_(level) {}

    void lock()
    {
        check_order();
        m_.lock();
        enter();
    }
    void unlock()
    {
        if (this_thread_level_ != level_)
            throw std::logic_error("hierarchy violated: unlock out of order");
        this_thread_level_ = previous_level_;
        m_.unlock();
    }
    bool try_lock()
    {
        check_order();
        if (!m_.try_lock())
            return false;
        enter();
        return true;
    }
    unsigned long level() const { return level_; }

private:
    void check_order() const
    {
        if (this_thread_level_ <= level_)
            throw std::logic_error("hierarchy violated: locking upwards");
    }
    void enter()
    {
        // previous_level_ is only touched while m_ is held: no race.
        previous_level_ = this_thread_level_;
        this_thread_level_ = level_;
    }

    std::mutex m_;
    const unsigned long level_;
    unsigned long previous_level_ = 0;
    static thread_local unsigned long this_thread_level_;
};
thread_local unsigned long HierarchicalMutex::this_thread_level_ = ULONG_MAX;

static HierarchicalMutex app_state(1000);
static HierarchicalMutex device_list(500);
static HierarchicalMutex log_buffer(100);
static long devices = 0, log_lines = 0; // guarded by device_list / log_buffer

// Correct: top-down.
static void add_device_and_log()
{
    std::lock_guard<HierarchicalMutex> a(app_state);
    std::lock_guard<HierarchicalMutex> d(device_list);
    devices++;
    std::lock_guard<HierarchicalMutex> l(log_buffer);
    log_lines++;
}

// Wrong: a logging callback that, while holding the log lock, walks the
// device list. With plain mutexes this deadlocks against add_device_and_log
// only when the timing is unlucky. Here it fails the first time it runs.
static bool log_then_touch_devices()
{
    std::lock_guard<HierarchicalMutex> l(log_buffer);
    try {
        std::lock_guard<HierarchicalMutex> d(device_list); // 100 -> 500 !
        devices++;
    } catch (const std::logic_error &e) {
        std::printf("    caught: %s (holding level 100, asked for 500)\n",
                    e.what());
        return false;
    }
    return true;
}

int main()
{
    std::printf("[1] 4 threads lock app_state(1000) -> device_list(500) -> "
                "log_buffer(100)\n");
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; i++)
        ts.emplace_back([] {
            for (int k = 0; k < 5000; k++)
                add_device_and_log();
        });
    for (auto &t : ts)
        t.join();
    std::printf("    devices=%ld log_lines=%ld\n", devices, log_lines);
    CHECK(devices == 20000 && log_lines == 20000);

    std::printf("[2] a code path that locks upwards\n");
    CHECK(!log_then_touch_devices());
    CHECK(devices == 20000); // the bad path changed nothing

    // [3] The thread's level is restored correctly after the exception:
    // a fresh top-down sequence still works on this thread.
    add_device_and_log();
    CHECK(devices == 20001);
    std::printf("[3] after the violation the thread's level was restored; "
                "top-down locking works again\n");

    // [4] scoped_lock with two mutexes of DIFFERENT levels would try both
    // orders internally (deadlock avoidance) and could trip the check, so
    // with a hierarchy you lock explicitly, top-down. Same level twice is
    // always a violation (equal is not "lower").
    HierarchicalMutex peer_a(300), peer_b(300);
    bool threw = false;
    try {
        std::lock_guard<HierarchicalMutex> a(peer_a);
        std::lock_guard<HierarchicalMutex> b(peer_b);
    } catch (const std::logic_error &) {
        threw = true;
    }
    CHECK(threw);
    std::printf("[4] two mutexes on the same level cannot be nested: give "
                "siblings distinct levels or order them by address\n");

    std::printf("02_hierarchical_mutex: all checks passed\n");
    return 0;
}
