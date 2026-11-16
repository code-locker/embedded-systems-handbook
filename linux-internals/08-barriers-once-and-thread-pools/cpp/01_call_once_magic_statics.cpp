/*
 * 01_call_once_magic_statics.cpp — thread-safe lazy initialisation in C++:
 *                                  std::call_once, "magic statics", and
 *                                  double-checked locking with std::atomic
 *
 * Concept : Three ways to say "construct this once, on first use, even if
 *           many threads ask at the same time":
 *             1. Function-local static ("magic static", C++11): the
 *                compiler emits a guard variable plus __cxa_guard_acquire/
 *                release. The simplest and usually the best: a Meyers
 *                singleton.
 *             2. std::call_once(flag, f): when the thing to initialise is
 *                a member or the init needs arguments.
 *             3. Double-checked locking with std::atomic<T*> acquire/
 *                release: what 1 and 2 do internally, by hand.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/01_call_once_magic_statics.cpp
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/cpp/01_call_once_magic_statics.cpp SAN=tsan
 * Read    : ../README.md  (sections "Once", "Double-checked locking", "C++ magic statics")
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
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

static std::atomic<int> g_constructions{0};

// Something expensive: e.g. parsing a sensor calibration file.
struct Calibration {
    std::map<std::string, double> gains;
    Calibration()
    {
        ++g_constructions;
        for (int i = 0; i < 64; i++)
            gains["ch" + std::to_string(i)] = 1.0 + i / 100.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // slow init
    }
};

// 1. Magic static (Meyers singleton). C++11 [stmt.dcl]/4: "If control
//    enters the declaration concurrently while the variable is being
//    initialized, the concurrent execution shall wait for completion."
//    Destroyed automatically at exit (in reverse order of construction).
const Calibration &calibration_magic()
{
    static const Calibration instance; // guard + __cxa_guard_acquire
    return instance;
}

// 2. std::call_once, here per object: each Sensor loads its calibration
//    lazily, once, even if several threads read it first at the same time.
class Sensor {
public:
    double gain(const std::string &ch)
    {
        std::call_once(once_, [this] { cal_ = std::make_unique<Calibration>(); });
        return cal_->gains.at(ch); // safe: call_once synchronises-with us
    }

private:
    std::once_flag once_;
    std::unique_ptr<Calibration> cal_;
};

// 3. Double-checked locking by hand. The atomic pointer is what makes it
//    correct; a plain Calibration* here is the famous broken version.
class LazyDcl {
public:
    ~LazyDcl() { delete p_.load(); }
    const Calibration &get()
    {
        Calibration *p = p_.load(std::memory_order_acquire); // check 1
        if (!p) {
            std::lock_guard<std::mutex> g(m_);
            p = p_.load(std::memory_order_relaxed); // check 2 (under lock)
            if (!p) {
                p = new Calibration();                 // build completely...
                p_.store(p, std::memory_order_release); // ...then publish
            }
        }
        return *p;
    }

private:
    std::atomic<Calibration *> p_{nullptr};
    std::mutex m_;
};

template <class F>
void hammer(int nthreads, F f)
{
    std::vector<std::thread> ts;
    std::atomic<int> ready{0};
    for (int i = 0; i < nthreads; i++)
        ts.emplace_back([&] {
            ++ready; // crude start gate: everyone collides on first use
            while (ready.load() < nthreads)
                std::this_thread::yield();
            for (int k = 0; k < 200; k++)
                f();
        });
    for (auto &t : ts)
        t.join();
}

int main()
{
    const int T = 8;

    g_constructions = 0;
    hammer(T, [] { CHECK(calibration_magic().gains.at("ch10") == 1.1); });
    std::printf("magic static : %d threads -> %d construction(s)\n", T,
                g_constructions.load());
    CHECK(g_constructions.load() == 1);

    g_constructions = 0;
    Sensor s1, s2;
    hammer(T, [&] {
        CHECK(s1.gain("ch1") == 1.01);
        CHECK(s2.gain("ch2") == 1.02);
    });
    std::printf("call_once    : 2 sensors, %d threads -> %d construction(s)\n", T,
                g_constructions.load());
    CHECK(g_constructions.load() == 2); // once per Sensor object

    g_constructions = 0;
    LazyDcl lazy;
    hammer(T, [&] { CHECK(lazy.get().gains.size() == 64); });
    std::printf("DCL (atomic) : %d threads -> %d construction(s)\n", T,
                g_constructions.load());
    CHECK(g_constructions.load() == 1);

    std::printf("01_call_once_magic_statics: all checks passed\n");
    return 0;
}
