/*
 * 01_event_loop.cpp — an RAII EventLoop over epoll with std::function
 *                     callbacks: fds, timers, signals, and post() from
 *                     other threads
 *
 * Concept : The C loop from c/08 as a reusable class. Every kernel object
 *           is a move-only Fd that closes itself. The loop owns a map
 *           fd -> callback; add_timer() wraps a timerfd, add_signals() a
 *           signalfd, and post() lets ANY thread queue a task: the task
 *           goes into a mutex-protected queue and an eventfd wakes the loop
 *           (the same trick as GLib's g_main_context_invoke, libuv's
 *           uv_async_send, Asio's post). Callbacks may add or remove
 *           handlers, even their own, while the loop is dispatching.
 * Build   : make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/01_event_loop.cpp
 *           make run P=linux-internals/13-signals-eventfd-and-epoll/cpp/01_event_loop.cpp SAN=tsan
 * Read    : ../README.md  (sections 3.11 "An epoll event loop" and 5 "C vs C++")
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

[[noreturn]] static void throw_errno(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

class Fd {
public:
    Fd() = default;
    explicit Fd(int fd, const char *what = "fd") : fd_(fd)
    {
        if (fd_ < 0)
            throw_errno(what);
    }
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Fd &operator=(Fd &&o) noexcept
    {
        if (this != &o) {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = std::exchange(o.fd_, -1);
        }
        return *this;
    }
    int get() const noexcept { return fd_; }

private:
    int fd_ = -1;
};

/*
 *                 +---------------- EventLoop ----------------+
 *  any thread --> | post(task): lock; queue.push; unlock;     |
 *                 |             eventfd += 1   ---------+     |
 *                 |                                     v     |
 *                 |  run(): epoll_wait(epfd) -> fd -> handlers_[fd](events)
 *                 |         wake fd   -> swap queue out, run tasks
 *                 |         timer fd  -> read expirations -> cb()
 *                 |         signalfd  -> read siginfo     -> cb(info)
 *                 +-------------------------------------------+
 * Only the loop thread touches handlers_, so it needs no lock. The task
 * queue is the ONLY shared state, guarded by mu_.
 */
class EventLoop {
public:
    using IoCallback = std::function<void(uint32_t events)>;

    EventLoop()
        : ep_(::epoll_create1(EPOLL_CLOEXEC), "epoll_create1"),
          wake_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), "eventfd")
    {
        add_fd(wake_.get(), EPOLLIN, [this](uint32_t) { run_posted(); });
    }
    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    /* Watch fd (not owned) for `events`. */
    void add_fd(int fd, uint32_t events, IoCallback cb)
    {
        epoll_event ev{};
        ev.events = events;
        ev.data.fd = fd;
        if (::epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &ev) < 0)
            throw_errno("epoll_ctl ADD");
        handlers_[fd] = std::make_shared<IoCallback>(std::move(cb));
    }

    void remove_fd(int fd)
    {
        ::epoll_ctl(ep_.get(), EPOLL_CTL_DEL, fd, nullptr);
        handlers_.erase(fd);
        owned_.erase(fd); /* closes it if the loop owns it */
    }

    /* Periodic timer; returns its fd (an id for remove_fd). */
    int add_timer(std::chrono::milliseconds period, std::function<void(uint64_t)> cb)
    {
        Fd t(::timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK), "timerfd");
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(period).count();
        itimerspec its{};
        its.it_value.tv_sec = its.it_interval.tv_sec = ns / 1000000000;
        its.it_value.tv_nsec = its.it_interval.tv_nsec = ns % 1000000000;
        if (::timerfd_settime(t.get(), 0, &its, nullptr) < 0)
            throw_errno("timerfd_settime");
        int fd = t.get();
        add_fd(fd, EPOLLIN, [fd, cb = std::move(cb)](uint32_t) {
            uint64_t exp;
            if (::read(fd, &exp, sizeof exp) == sizeof exp)
                cb(exp);
        });
        owned_.emplace(fd, std::move(t));
        return fd;
    }

    /* The caller must have BLOCKED these signals in every thread first. */
    int add_signals(const sigset_t &set, std::function<void(const signalfd_siginfo &)> cb)
    {
        Fd s(::signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK), "signalfd");
        int fd = s.get();
        add_fd(fd, EPOLLIN, [fd, cb = std::move(cb)](uint32_t) {
            signalfd_siginfo si;
            while (::read(fd, &si, sizeof si) == sizeof si)
                cb(si);
        });
        owned_.emplace(fd, std::move(s));
        return fd;
    }

    /* Thread-safe: may be called from any thread. */
    void post(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            tasks_.push_back(std::move(task));
        }
        uint64_t one = 1;
        /* eventfd write is a syscall: no lock needed, never "full" in
         * practice (the counter saturates at 2^64-2) */
        if (::write(wake_.get(), &one, sizeof one) != sizeof one)
            throw_errno("eventfd write");
    }

    void stop() { running_ = false; } /* loop thread only (or via post) */

    void run()
    {
        running_ = true;
        while (running_) {
            epoll_event evs[16];
            int n = ::epoll_wait(ep_.get(), evs, 16, 2000);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw_errno("epoll_wait");
            if (n == 0)
                throw std::runtime_error("event loop idle for 2 s: watchdog");
            for (int i = 0; i < n; i++) {
                /* Look the handler up for EVERY event: an earlier callback
                 * in this batch may have removed it. Holding a shared_ptr
                 * copy keeps the std::function alive even if the callback
                 * removes itself while running. */
                auto it = handlers_.find(evs[i].data.fd);
                if (it == handlers_.end())
                    continue;
                std::shared_ptr<IoCallback> cb = it->second;
                (*cb)(evs[i].events);
            }
        }
    }

private:
    void run_posted()
    {
        uint64_t v;
        (void)::read(wake_.get(), &v, sizeof v); /* reset the counter */
        std::deque<std::function<void()>> batch;
        {
            std::lock_guard<std::mutex> lk(mu_);
            batch.swap(tasks_); /* run tasks WITHOUT holding the lock */
        }
        for (auto &t : batch)
            t();
    }

    Fd ep_, wake_;
    std::unordered_map<int, std::shared_ptr<IoCallback>> handlers_;
    std::unordered_map<int, Fd> owned_;
    std::mutex mu_;
    std::deque<std::function<void()>> tasks_;
    bool running_ = false;
};

int main()
{
    std::cout << std::unitbuf;

    /* Block SIGTERM/SIGINT BEFORE starting any thread: the worker inherits
     * the mask, so the signal can only be consumed by the loop's signalfd
     * and never kills the process through its default action. */
    sigset_t sigs;
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGTERM);
    sigaddset(&sigs, SIGINT);
    CHECK(::pthread_sigmask(SIG_BLOCK, &sigs, nullptr) == 0);

    EventLoop loop;
    constexpr int kPosts = 100, kTicks = 5;
    int posted_seen = 0;   /* touched only on the loop thread */
    uint64_t ticks = 0;
    bool got_term = false;
    std::string received;

    /* Stop only when EVERYTHING has happened: SIGTERM seen, all posted
     * tasks ran, the worker's pipe message arrived. These complete in a
     * timing-dependent order, so each of their callbacks asks; the last
     * one to finish actually stops the loop. All on the loop thread. */
    auto maybe_stop = [&] {
        if (got_term && posted_seen == kPosts && received == "worker done\n")
            loop.stop();
    };

    /* 1. a pipe: the worker writes a line at the end, the loop reads it */
    int p[2];
    CHECK(::pipe2(p, O_CLOEXEC | O_NONBLOCK) == 0);
    Fd pipe_r(p[0]), pipe_w(p[1]);
    loop.add_fd(pipe_r.get(), EPOLLIN, [&](uint32_t) {
        char buf[64];
        ssize_t n;
        while ((n = ::read(pipe_r.get(), buf, sizeof buf)) > 0)
            received.append(buf, static_cast<size_t>(n));
        maybe_stop();
    });

    /* 2. a timer: after kTicks, remove itself and send SIGTERM to us */
    int timer_id = -1;
    timer_id = loop.add_timer(std::chrono::milliseconds(10), [&](uint64_t exp) {
        ticks += exp;
        if (ticks >= kTicks) {
            loop.remove_fd(timer_id); /* a callback removing itself */
            std::cout << "  timer: " << ticks << " ticks, removed myself, kill(SIGTERM)\n";
            CHECK(::kill(::getpid(), SIGTERM) == 0);
        }
    });

    /* 3. signals: SIGTERM -> stop (once the rest is done too) */
    loop.add_signals(sigs, [&](const signalfd_siginfo &si) {
        std::cout << "  signalfd: " << sigabbrev_np(static_cast<int>(si.ssi_signo))
                  << " from pid " << si.ssi_pid << "\n";
        got_term = true;
        maybe_stop();
    });

    /* 4. another thread posts tasks into the loop */
    std::thread worker([&] {
        for (int i = 0; i < kPosts; i++)
            loop.post([&] { /* runs later, on the loop thread */
                posted_seen++;
                maybe_stop();
            });
        CHECK(::write(pipe_w.get(), "worker done\n", 12) == 12);
    });

    std::cout << "running: timer + signalfd + pipe + " << kPosts << " posted tasks\n";
    loop.run();
    worker.join();

    std::cout << "  posted tasks run: " << posted_seen << ", ticks: " << ticks
              << ", pipe: \"" << received.substr(0, received.size() - 1) << "\"\n";
    CHECK(posted_seen == kPosts);
    CHECK(ticks >= static_cast<uint64_t>(kTicks));
    CHECK(got_term);
    CHECK(received == "worker done\n");
    std::cout << "01_event_loop: all checks passed\n";
    return 0;
}
