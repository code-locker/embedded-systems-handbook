/*
 * 02_framed_channel.cpp — a message Channel over a stream socket: length-
 *                         prefixed frames, optional fd attached, and a
 *                         small RPC layer with std::function handlers
 *
 * Concept : Wrap the framing rules from c/03 in a class so nobody can get
 *           them wrong again: send() writes header + payload with ONE
 *           sendmsg (writev-style iovecs), recv() loops until a whole frame
 *           is in and returns std::optional (nullopt = clean EOF). A frame
 *           may carry one fd (SCM_RIGHTS), received as a UniqueFd so it
 *           cannot leak. On top: an RPC server that maps method names to
 *           std::function handlers, one thread per connection, tested by
 *           several client threads (ThreadSanitizer-clean). Fork happens
 *           first, while the process still has one thread.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/cpp/02_framed_channel.cpp
 *           make run P=linux-internals/12-ipc-unix-sockets/cpp/02_framed_channel.cpp SAN=tsan
 * Read    : ../README.md  (sections 3.4 "Framing", 3.5 "Passing fds", 5 "C vs C++")
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
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

[[noreturn]] static void throw_errno(const char *what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    UniqueFd(UniqueFd &&o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    UniqueFd &operator=(UniqueFd &&o) noexcept
    {
        if (this != &o)
            reset(std::exchange(o.fd_, -1));
        return *this;
    }
    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

struct Message {
    std::string data;
    UniqueFd fd; /* empty unless the sender attached one */
};

/*
 * Channel: frames over a SOCK_STREAM socket.
 *
 *   send("hi", fd):  sendmsg( iov[0] = [00 00 00 02]  iov[1] = "hi",
 *                             cmsg = SCM_RIGHTS{fd} )
 *   The kernel attaches the fd to the FIRST byte of this sendmsg; a stream
 *   receiver gets it with whichever recvmsg returns that byte, so recv()
 *   uses recvmsg for every chunk and keeps any fd it sees.
 *
 * Thread-safety: one Channel per thread pair; not shared (like a socket).
 */
class Channel {
public:
    static constexpr uint32_t kMaxFrame = 1u << 20;

    explicit Channel(UniqueFd fd) : fd_(std::move(fd)) {}

    static std::pair<Channel, Channel> pair()
    {
        int sv[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
            throw_errno("socketpair");
        return {Channel(UniqueFd(sv[0])), Channel(UniqueFd(sv[1]))};
    }

    void send(std::string_view payload, int pass_fd = -1)
    {
        if (payload.size() > kMaxFrame)
            throw std::length_error("frame too large");
        uint32_t be = htonl(static_cast<uint32_t>(payload.size()));
        iovec iov[2] = {{&be, sizeof be},
                        {const_cast<char *>(payload.data()), payload.size()}};
        union {
            char buf[CMSG_SPACE(sizeof(int))];
            cmsghdr align;
        } ctrl{};
        msghdr mh{};
        mh.msg_iov = iov;
        mh.msg_iovlen = 2;
        if (pass_fd >= 0) {
            mh.msg_control = ctrl.buf;
            mh.msg_controllen = sizeof ctrl.buf;
            cmsghdr *c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            std::memcpy(CMSG_DATA(c), &pass_fd, sizeof pass_fd);
        }
        size_t left = sizeof be + payload.size();
        /* First sendmsg carries the fd. If it is short (a signal, a
         * non-blocking socket), finish the rest with plain sends. */
        ssize_t n = ::sendmsg(fd_.get(), &mh, MSG_NOSIGNAL);
        if (n < 0)
            throw_errno("sendmsg");
        size_t done = static_cast<size_t>(n);
        while (done < left) {
            const char *base;
            size_t len;
            if (done < sizeof be) {
                base = reinterpret_cast<const char *>(&be) + done;
                len = sizeof be - done;
            } else {
                base = payload.data() + (done - sizeof be);
                len = left - done;
            }
            ssize_t m = ::send(fd_.get(), base, len, MSG_NOSIGNAL);
            if (m < 0 && errno == EINTR)
                continue;
            if (m < 0)
                throw_errno("send");
            done += static_cast<size_t>(m);
        }
    }

    /* nullopt = the peer closed cleanly between frames. */
    std::optional<Message> recv()
    {
        Message m;
        uint32_t be;
        if (!read_exact(&be, sizeof be, m.fd, true))
            return std::nullopt;
        uint32_t len = ntohl(be);
        if (len > kMaxFrame)
            throw std::runtime_error("protocol error: frame length " + std::to_string(len));
        m.data.resize(len);
        if (len && !read_exact(m.data.data(), len, m.fd, false))
            throw std::runtime_error("peer closed in the middle of a frame");
        return m;
    }

    int fd() const { return fd_.get(); }

private:
    /* Returns false only on EOF before the first byte when eof_ok. */
    bool read_exact(void *dst, size_t len, UniqueFd &fd_out, bool eof_ok)
    {
        auto *p = static_cast<char *>(dst);
        size_t got = 0;
        while (got < len) {
            union {
                char buf[CMSG_SPACE(sizeof(int) * 4)];
                cmsghdr align;
            } ctrl;
            iovec iov{p + got, len - got};
            msghdr mh{};
            mh.msg_iov = &iov;
            mh.msg_iovlen = 1;
            mh.msg_control = ctrl.buf;
            mh.msg_controllen = sizeof ctrl.buf;
            ssize_t n = ::recvmsg(fd_.get(), &mh, MSG_CMSG_CLOEXEC);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw_errno("recvmsg");
            for (cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
                    continue;
                size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < k; i++) {
                    int f;
                    std::memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
                    UniqueFd owned(f); /* own every received fd, keep the first */
                    if (!fd_out)
                        fd_out = std::move(owned);
                }
            }
            if (n == 0) {
                if (got == 0 && eof_ok)
                    return false;
                throw std::runtime_error("unexpected EOF");
            }
            got += static_cast<size_t>(n);
        }
        return true;
    }

    UniqueFd fd_;
};

/* -------------------- part 1: fd passing across fork -------------------- */
static void demo_fd_passing()
{
    std::cout << "1) Channel across fork(): a memfd travels with a frame\n";
    auto [parent_ch, child_ch] = Channel::pair();
    pid_t pid = ::fork(); /* still single-threaded here: fork is safe */
    CHECK(pid >= 0);
    if (pid == 0) {
        int rc = 1;
        try {
            ::alarm(5);
            auto m = child_ch.recv();
            if (m && m->fd) {
                char buf[64] = {};
                ssize_t n = ::pread(m->fd.get(), buf, sizeof buf - 1, 0);
                child_ch.send("child read " + std::to_string(n) + " bytes: " +
                              std::string(buf, n > 0 ? static_cast<size_t>(n) : 0));
                rc = 0;
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "child: %s\n", e.what());
        }
        ::_exit(rc);
    }

    UniqueFd mfd(::memfd_create("channel-demo", MFD_CLOEXEC));
    CHECK(mfd);
    const std::string text = "frame from a memfd";
    CHECK(::write(mfd.get(), text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    parent_ch.send("here is a file", mfd.get());
    auto reply = parent_ch.recv();
    CHECK(reply && !reply->fd);
    std::cout << "  parent got: \"" << reply->data << "\"\n";
    CHECK(reply->data == "child read 18 bytes: " + text);
    int st = 0;
    CHECK(::waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* ------------------- part 2: RPC with std::function --------------------- */
/*
 *   client thread k                         server thread k
 *   ---------------                         ---------------
 *   call("add", "3 4")                      req = recv()
 *     send("add\n3 4")   ----- frame ---->  method, args = split at '\n'
 *                                           reply = handlers[method](args)
 *     recv()             <---- frame -----  send(reply)
 *
 * The handler table is built before any thread starts and is only READ
 * afterwards, so sharing it needs no lock. The call counter is atomic.
 */
class RpcServer {
public:
    using Handler = std::function<std::string(std::string_view)>;

    void add(std::string name, Handler h) { handlers_.emplace(std::move(name), std::move(h)); }

    /* Serve one connection until the client hangs up (runs in its thread). */
    void serve(Channel ch)
    {
        while (auto req = ch.recv()) {
            std::string_view s = req->data;
            size_t nl = s.find('\n');
            std::string method(s.substr(0, nl));
            std::string_view args = nl == std::string_view::npos ? "" : s.substr(nl + 1);
            auto it = handlers_.find(method);
            ch.send(it == handlers_.end() ? "ERR no such method" : it->second(args));
            calls_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    long calls() const { return calls_.load(); }

private:
    std::map<std::string, Handler> handlers_;
    std::atomic<long> calls_{0};
};

class RpcClient {
public:
    explicit RpcClient(Channel ch) : ch_(std::move(ch)) {}
    std::string call(std::string_view method, std::string_view args)
    {
        std::string req(method);
        req += '\n';
        req += args;
        ch_.send(req);
        auto rep = ch_.recv();
        if (!rep)
            throw std::runtime_error("server hung up");
        return std::move(rep->data);
    }

private:
    Channel ch_;
};

static void demo_rpc()
{
    std::cout << "2) RPC: handler table of std::function, one thread per connection\n";
    constexpr int kClients = 3, kCalls = 40;
    RpcServer server;
    server.add("add", [](std::string_view a) {
        long x = 0, y = 0;
        std::sscanf(std::string(a).c_str(), "%ld %ld", &x, &y);
        return std::to_string(x + y);
    });
    server.add("upper", [](std::string_view a) {
        std::string s(a);
        for (char &c : s)
            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        return s;
    });

    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int k = 0; k < kClients; k++) {
        auto [srv_end, cli_end] = Channel::pair();
        threads.emplace_back([&server, ch = std::move(srv_end)]() mutable {
            server.serve(std::move(ch));
        });
        threads.emplace_back([&failures, k, ch = std::move(cli_end)]() mutable {
            RpcClient cli(std::move(ch));
            for (int i = 0; i < kCalls; i++) {
                if (cli.call("add", std::to_string(k) + " " + std::to_string(i)) !=
                    std::to_string(k + i))
                    failures++;
                if (cli.call("upper", "frame") != "FRAME")
                    failures++;
            }
            if (cli.call("nope", "") != "ERR no such method")
                failures++;
        }); /* cli (and its socket) destroyed here -> server sees EOF */
    }
    for (auto &t : threads)
        t.join();
    std::cout << "  " << kClients << " clients, " << server.calls() << " calls, "
              << failures.load() << " wrong replies\n";
    CHECK(failures.load() == 0);
    CHECK(server.calls() == kClients * (2 * kCalls + 1));
}

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::cout << std::unitbuf;
    demo_fd_passing(); /* fork first ... */
    demo_rpc();        /* ... threads after */
    std::cout << "02_framed_channel: all checks passed\n";
    return 0;
}
