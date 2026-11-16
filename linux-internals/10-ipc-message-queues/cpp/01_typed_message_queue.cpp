/*
 * 01_typed_message_queue.cpp — MessageQueue<T>: a typed, RAII wrapper over
 *                              POSIX message queues
 *
 * Concept : mq_send takes `const char*, size_t`: nothing stops you from
 *           sending a Foo and receiving it as a Bar, or sending a struct
 *           that contains a pointer or a std::string (whose heap data stays
 *           behind in the sender). A template fixes the type at compile
 *           time, static_assert(is_trivially_copyable) rejects types that
 *           cannot survive a byte copy, and open() checks at run time that
 *           the queue's msgsize matches sizeof(T). The destructor closes
 *           the descriptor; the creating ("owner") object also unlinks the
 *           name, so no queue is left behind in /dev/mqueue.
 * Build   : make run P=linux-internals/10-ipc-message-queues/cpp/01_typed_message_queue.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <fcntl.h>
#include <mqueue.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
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

[[noreturn]] static void throw_errno(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

template <typename T>
class MessageQueue {
    // The message is copied byte-for-byte into the kernel and out again in
    // another process. Only types whose bytes ARE the value qualify: no
    // pointers to elsewhere, no virtual functions, no owning members.
    // MessageQueue<std::string> fails right here, at compile time.
    static_assert(std::is_trivially_copyable_v<T>,
                  "MessageQueue<T>: T must be trivially copyable");
    static_assert(!std::is_pointer_v<T>,
                  "a pointer is meaningless in another process");

public:
    // Create a new queue; this object OWNS the name and unlinks it.
    static MessageQueue create(const std::string &name, long max_msgs)
    {
        mq_attr attr{};
        attr.mq_maxmsg = max_msgs;
        attr.mq_msgsize = static_cast<long>(sizeof(T));
        mqd_t q = ::mq_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC,
                            0600, &attr);
        if (q == static_cast<mqd_t>(-1))
            throw_errno("mq_open(create)");
        return MessageQueue(q, name, true);
    }

    // Open an existing queue (another process created it). Verifies that
    // it was created for messages of exactly sizeof(T) bytes.
    static MessageQueue open(const std::string &name)
    {
        mqd_t q = ::mq_open(name.c_str(), O_RDWR | O_CLOEXEC);
        if (q == static_cast<mqd_t>(-1))
            throw_errno("mq_open");
        MessageQueue mq(q, name, false); // RAII from here: closes on throw
        if (mq.attr().mq_msgsize != static_cast<long>(sizeof(T)))
            throw std::runtime_error("queue " + name +
                                     ": msgsize does not match sizeof(T)");
        return mq;
    }

    MessageQueue(MessageQueue &&o) noexcept
        : q_(std::exchange(o.q_, kInvalid)), name_(std::move(o.name_)),
          owner_(std::exchange(o.owner_, false))
    {
    }
    MessageQueue &operator=(MessageQueue &&o) noexcept
    {
        if (this != &o) {
            close_and_maybe_unlink();
            q_ = std::exchange(o.q_, kInvalid);
            name_ = std::move(o.name_);
            owner_ = std::exchange(o.owner_, false);
        }
        return *this;
    }
    MessageQueue(const MessageQueue &) = delete;
    MessageQueue &operator=(const MessageQueue &) = delete;
    ~MessageQueue() { close_and_maybe_unlink(); }

    // Blocking send (waits while the queue is full: backpressure).
    void send(const T &msg, unsigned prio = 0)
    {
        while (::mq_send(q_, reinterpret_cast<const char *>(&msg), sizeof(T),
                         prio) != 0) {
            if (errno != EINTR)
                throw_errno("mq_send");
        }
    }

    // Receive with a timeout; nullopt on timeout. A zero timeout = poll.
    std::optional<T> receive_for(std::chrono::milliseconds timeout,
                                 unsigned *prio = nullptr)
    {
        timespec dl = deadline(timeout);
        T msg;
        for (;;) {
            ssize_t n = ::mq_timedreceive(q_, reinterpret_cast<char *>(&msg),
                                          sizeof(T), prio, &dl);
            if (n == static_cast<ssize_t>(sizeof(T)))
                return msg;
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && errno == ETIMEDOUT)
                return std::nullopt;
            throw_errno("mq_timedreceive");
        }
    }
    std::optional<T> try_receive(unsigned *prio = nullptr)
    {
        return receive_for(std::chrono::milliseconds(0), prio);
    }

    long size() const { return attr().mq_curmsgs; }
    long capacity() const { return attr().mq_maxmsg; }
    mqd_t native_handle() const { return q_; } // pollable fd on Linux

private:
    static constexpr mqd_t kInvalid = static_cast<mqd_t>(-1);

    MessageQueue(mqd_t q, std::string name, bool owner)
        : q_(q), name_(std::move(name)), owner_(owner)
    {
    }

    mq_attr attr() const
    {
        mq_attr a{};
        if (::mq_getattr(q_, &a) != 0)
            throw_errno("mq_getattr");
        return a;
    }

    // mq_timed* want an ABSOLUTE CLOCK_REALTIME time. A deadline already
    // in the past means "don't wait at all".
    static timespec deadline(std::chrono::milliseconds rel)
    {
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        long long ns = ts.tv_nsec + static_cast<long long>(rel.count()) * 1000000LL;
        ts.tv_sec += static_cast<time_t>(ns / 1000000000LL);
        ts.tv_nsec = static_cast<long>(ns % 1000000000LL);
        return ts;
    }

    void close_and_maybe_unlink() noexcept
    {
        if (q_ != kInvalid) {
            ::mq_close(q_);
            q_ = kInvalid;
        }
        if (owner_) {
            ::mq_unlink(name_.c_str());
            owner_ = false;
        }
    }

    mqd_t q_ = kInvalid;
    std::string name_;
    bool owner_ = false;
};

// ---- the message type: plain data, fixed-width fields ----
struct SensorReading {
    uint32_t sensor_id;
    uint32_t seq;
    float celsius;
    uint32_t flags; // bit 0 = alarm
};
static_assert(std::is_trivially_copyable_v<SensorReading>);

// This would not compile — try it:
//   struct Bad { std::string label; };
//   MessageQueue<Bad> q = ...;   // error: T must be trivially copyable

int main()
{
    ::alarm(10);
    const std::string name = "/mq_typed_" + std::to_string(::getpid());
    constexpr uint32_t kReadings = 100;
    {
        auto q = MessageQueue<SensorReading>::create(name, 8);
        std::printf("created %s: capacity %ld messages of %zu bytes\n",
                    name.c_str(), q.capacity(), sizeof(SensorReading));

        std::fflush(stdout);
        pid_t pid = ::fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            ::alarm(5);
            // Child = sensor process. Opens BY NAME, as a non-owner: its
            // destructor will close, but not unlink.
            try {
                auto tx = MessageQueue<SensorReading>::open(name);
                for (uint32_t i = 0; i < kReadings; i++) {
                    SensorReading r{7, i, 20.0f + static_cast<float>(i % 10),
                                    0};
                    tx.send(r);
                }
                SensorReading alarm_msg{7, kReadings, 99.5f, 1};
                tx.send(alarm_msg, 5); // high priority
            } catch (const std::exception &e) {
                std::fprintf(stderr, "child: %s\n", e.what());
                _exit(2);
            }
            // _exit, NOT return: the child holds a fork-copy of `q`, the
            // OWNER object. Returning would run its destructor and
            // mq_unlink the parent's queue. RAII + fork = think about who
            // runs which destructors.
            _exit(0);
        }

        uint32_t next = 0;
        bool alarm_seen = false;
        while (next < kReadings || !alarm_seen) {
            unsigned prio = 0;
            auto r = q.receive_for(std::chrono::milliseconds(2000), &prio);
            CHECK(r.has_value()); // 2 s without data = producer is dead
            if (r->flags & 1) {
                CHECK(prio == 5);
                CHECK(r->celsius > 99.0f);
                alarm_seen = true;
                std::printf("ALARM from sensor %u at %.1f C (prio %u), "
                            "after %u normal readings (timing-dependent)\n",
                            r->sensor_id, static_cast<double>(r->celsius),
                            prio, next);
            } else {
                CHECK(r->seq == next);
                CHECK(r->celsius == 20.0f + static_cast<float>(next % 10));
                next++;
            }
        }
        int st = 0;
        CHECK(::waitpid(pid, &st, 0) == pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        std::printf("received %u readings in order + 1 alarm\n", next);
        CHECK(!q.try_receive()); // empty now, try_receive does not block

        // Type check at open(): a queue made for SensorReading (16 bytes)
        // cannot be opened as MessageQueue<uint64_t> (8 bytes).
        bool threw = false;
        try {
            auto wrong = MessageQueue<uint64_t>::open(name);
        } catch (const std::runtime_error &e) {
            threw = true;
            std::printf("open as the wrong type threw: %s\n", e.what());
        }
        CHECK(threw);
    } // q (the owner) destroyed: mq_close + mq_unlink

    errno = 0;
    CHECK(::mq_open(name.c_str(), O_RDONLY) == static_cast<mqd_t>(-1) &&
          errno == ENOENT);
    std::printf("owner destroyed -> queue unlinked (not in /dev/mqueue)\n");

    std::printf("01_typed_message_queue: all checks passed\n");
    return 0;
}
