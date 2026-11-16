/*
 * 02_signal_thread_and_eventfd.cpp — RAII signal mask guard, a SignalThread
 *                                    with std::function handlers, and an
 *                                    eventfd-based semaphore for a thread pool
 *
 * Concept : Three small C++ building blocks for signal-aware programs:
 *             SignalBlocker  : blocks signals in this thread for a scope and
 *                              restores the previous mask in its destructor.
 *             SignalThread   : a std::thread that sigtimedwait()s for a set
 *                              of signals and calls a std::function for each,
 *                              in normal thread context (no async-safety
 *                              limits: it may lock, allocate, log).
 *             EventFdSemaphore: EFD_SEMAPHORE eventfd = counting semaphore
 *                              whose fd could also sit in epoll.
 *           Together: SIGRTMIN = "enqueue a job" (a real-time signal, so
 *           several are queued, not merged), SIGHUP = "reload",
 *           SIGTERM = "shut down", jobs run on a pool of worker threads.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/02_signal_thread_and_eventfd.cpp
 *           make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/02_signal_thread_and_eventfd.cpp SAN=tsan
 * Read    : ../README.md  (sections 3.6 "Signals and threads", 3.8 "eventfd", 5 "C vs C++")
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

/* Blocks `sigs` in the CALLING thread for the lifetime of the object. */
class SignalBlocker {
public:
    explicit SignalBlocker(std::initializer_list<int> sigs)
    {
        sigemptyset(&set_);
        for (int s : sigs)
            sigaddset(&set_, s);
        int e = ::pthread_sigmask(SIG_BLOCK, &set_, &old_);
        if (e)
            throw std::system_error(e, std::generic_category(), "pthread_sigmask");
    }
    ~SignalBlocker() { ::pthread_sigmask(SIG_SETMASK, &old_, nullptr); }
    SignalBlocker(const SignalBlocker &) = delete;
    SignalBlocker &operator=(const SignalBlocker &) = delete;
    const sigset_t &set() const { return set_; }

private:
    sigset_t set_, old_;
};

static bool is_blocked(int sig)
{
    sigset_t cur;
    ::pthread_sigmask(SIG_SETMASK, nullptr, &cur); /* query only */
    return sigismember(&cur, sig) == 1;
}

/*
 * SignalThread: waits synchronously; handlers run on THIS thread.
 * Stopping: a handler returns false ("stop"), or stop() is called. The
 * wait uses a 50 ms timeout so stop() is noticed without needing a
 * special wake-up signal.
 */
class SignalThread {
public:
    using Handler = std::function<bool(const siginfo_t &)>; /* false = stop */

    SignalThread(const sigset_t &set, std::map<int, Handler> handlers)
        : set_(set), handlers_(std::move(handlers)), th_([this] { loop(); })
    {
    }
    ~SignalThread()
    {
        stop();
        if (th_.joinable())
            th_.join();
    }
    void stop() { stop_.store(true); }
    void join() { th_.join(); }

private:
    void loop()
    {
        while (!stop_.load()) {
            siginfo_t si;
            timespec ts{0, 50 * 1000 * 1000};
            int sig = ::sigtimedwait(&set_, &si, &ts);
            if (sig < 0)
                continue; /* EAGAIN = timeout, EINTR: just re-check stop_ */
            auto it = handlers_.find(sig);
            if (it != handlers_.end() && !it->second(si))
                stop_.store(true);
        }
    }
    sigset_t set_;
    std::map<int, Handler> handlers_;
    std::atomic<bool> stop_{false};
    std::thread th_; /* last member: starts after the others exist */
};

/* Counting semaphore on an eventfd (EFD_SEMAPHORE: each read takes 1). */
class EventFdSemaphore {
public:
    explicit EventFdSemaphore(unsigned initial = 0)
        : fd_(::eventfd(initial, EFD_CLOEXEC | EFD_SEMAPHORE))
    {
        if (fd_ < 0)
            throw std::system_error(errno, std::generic_category(), "eventfd");
    }
    ~EventFdSemaphore() { ::close(fd_); }
    EventFdSemaphore(const EventFdSemaphore &) = delete;
    EventFdSemaphore &operator=(const EventFdSemaphore &) = delete;

    void post(uint64_t n = 1)
    {
        if (::write(fd_, &n, sizeof n) != sizeof n)
            throw std::system_error(errno, std::generic_category(), "eventfd write");
    }
    void wait()
    {
        uint64_t v;
        while (::read(fd_, &v, sizeof v) != sizeof v)
            if (errno != EINTR)
                throw std::system_error(errno, std::generic_category(), "eventfd read");
    }
    int fd() const { return fd_; } /* could be added to an epoll loop */

private:
    int fd_;
};

/* Pool: jobs in a mutex-protected deque, availability in the semaphore.
 * An empty std::function is the "poison pill" that ends one worker. */
class Pool {
public:
    explicit Pool(int n)
    {
        for (int i = 0; i < n; i++)
            workers_.emplace_back([this] {
                for (;;) {
                    sem_.wait(); /* sleeps in read(eventfd) */
                    std::function<void()> job;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        job = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    if (!job)
                        return;
                    job();
                }
            });
    }
    ~Pool() { shutdown(); }
    void submit(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            jobs_.push_back(std::move(job));
        }
        sem_.post(); /* one job = one unit */
    }
    void shutdown()
    {
        if (workers_.empty())
            return;
        for (size_t i = 0; i < workers_.size(); i++)
            submit(nullptr); /* FIFO: real jobs first, then pills */
        for (auto &t : workers_)
            t.join();
        workers_.clear();
    }

private:
    EventFdSemaphore sem_;
    std::mutex mu_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
};

template <class Pred>
static void wait_until(Pred p)
{
    for (int i = 0; i < 5000 && !p(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(p());
}

int main()
{
    std::cout << std::unitbuf;

    std::cout << "1) SignalBlocker restores the mask at scope exit\n";
    CHECK(!is_blocked(SIGUSR2));
    {
        SignalBlocker b{SIGUSR2};
        CHECK(is_blocked(SIGUSR2));
        std::cout << "  inside scope: SIGUSR2 blocked\n";
    }
    CHECK(!is_blocked(SIGUSR2));
    std::cout << "  after scope:  SIGUSR2 unblocked again\n";

    std::cout << "2) SignalThread + eventfd thread pool\n";
    /* Block in main BEFORE any thread exists: all threads inherit it. */
    SignalBlocker blocked{SIGRTMIN, SIGHUP, SIGTERM};

    constexpr int kJobs = 30, kSignalJobs = 3;
    std::atomic<int> done{0}, reloads{0}, signal_jobs{0};
    std::atomic<long> sum{0};
    Pool pool(3);

    SignalThread sigthread(
        blocked.set(),
        {
            {SIGHUP, [&](const siginfo_t &) {
                 reloads++; /* e.g. re-read the config file: allowed here */
                 std::cout << "  SIGHUP  -> reload config\n";
                 return true;
             }},
            {SIGRTMIN, [&](const siginfo_t &si) {
                 pool.submit([&, v = si.si_value.sival_int] {
                     sum += v;
                     signal_jobs++;
                 });
                 return true;
             }},
            {SIGTERM, [&](const siginfo_t &si) {
                 std::cout << "  SIGTERM from pid " << si.si_pid << " -> stop\n";
                 return false;
             }},
        });

    for (int i = 1; i <= kJobs; i++)
        pool.submit([&, i] {
            sum += i;
            done++;
        });

    CHECK(::kill(::getpid(), SIGHUP) == 0);
    wait_until([&] { return reloads.load() == 1; });
    for (int i = 0; i < kSignalJobs; i++) {
        union sigval v;
        v.sival_int = 1000;
        CHECK(::sigqueue(::getpid(), SIGRTMIN, v) == 0);
    }
    /* Real-time signal: all three are queued, none merged (with SIGUSR1,
     * three sent while one is pending would arrive as ONE; lesson c/02). */
    wait_until([&] { return signal_jobs.load() == kSignalJobs; });
    CHECK(::kill(::getpid(), SIGTERM) == 0);
    sigthread.join();
    pool.shutdown();

    std::cout << "  pool ran " << done.load() << " normal jobs + " << signal_jobs.load()
              << " signal jobs, sum " << sum.load() << "\n";
    CHECK(done.load() == kJobs);
    CHECK(signal_jobs.load() == kSignalJobs);
    CHECK(sum.load() == kJobs * (kJobs + 1) / 2 + 1000L * signal_jobs.load());
    CHECK(reloads.load() == 1);
    std::cout << "02_signal_thread_and_eventfd: all checks passed\n";
    return 0;
}
