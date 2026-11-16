/*
 * 01_raii_socket.cpp — an RAII, move-only Socket class for AF_UNIX, plus
 *                      RAII cleanup of the socket file and its directory
 *
 * Concept : In C every early return is a chance to leak an fd or leave a
 *           socket file behind. In C++ the destructor does it: UniqueFd
 *           closes, TempSocketDir unlinks and removes the directory, even
 *           when an exception unwinds the stack. A socket is a unique
 *           resource, so the class is MOVE-ONLY (copying would mean two
 *           owners closing one fd). Errors become std::system_error with
 *           errno inside, so no return value is silently ignored.
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/cpp/01_raii_socket.cpp
 * Read    : ../README.md  (section 5 "C vs C++")
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#include <cassert>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
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

/* ---------------------------------------------------------------------
 * UniqueFd: owns one file descriptor. Like std::unique_ptr, for ints.
 *
 *   UniqueFd a{fd};          a owns fd
 *   UniqueFd b{std::move(a)};  b owns fd, a holds -1 (owns nothing)
 *   ~b                        close(fd) exactly once
 * --------------------------------------------------------------------- */
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd &) = delete;            /* no copies: one owner */
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
    int release() noexcept { return std::exchange(fd_, -1); }
    void reset(int fd = -1) noexcept
    {
        if (fd_ >= 0)
            ::close(fd_); /* close errors are not recoverable here */
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

/* A Unix socket address: filesystem path or abstract name. */
class UnixAddress {
public:
    static UnixAddress path(const std::string &p)
    {
        UnixAddress a;
        if (p.size() >= sizeof a.sa_.sun_path)
            throw std::length_error("socket path longer than sun_path (108 bytes)");
        std::memcpy(a.sa_.sun_path, p.c_str(), p.size() + 1);
        a.len_ = sizeof a.sa_;
        return a;
    }
    static UnixAddress abstract(std::string_view name)
    {
        UnixAddress a;
        if (name.size() + 1 > sizeof a.sa_.sun_path)
            throw std::length_error("abstract name too long");
        a.sa_.sun_path[0] = '\0'; /* the abstract marker */
        std::memcpy(a.sa_.sun_path + 1, name.data(), name.size());
        a.len_ = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        return a;
    }
    const sockaddr *sa() const { return reinterpret_cast<const sockaddr *>(&sa_); }
    socklen_t len() const { return len_; }
    bool is_abstract() const { return sa_.sun_path[0] == '\0'; }

private:
    UnixAddress() { sa_.sun_family = AF_UNIX; }
    sockaddr_un sa_{};
    socklen_t len_ = 0;
};

/* Move-only socket. Every method throws std::system_error on failure. */
class Socket {
public:
    explicit Socket(int type)
        : fd_(::socket(AF_UNIX, type | SOCK_CLOEXEC, 0))
    {
        if (!fd_)
            throw_errno("socket");
    }
    static std::pair<Socket, Socket> pair(int type)
    {
        int sv[2];
        if (::socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, sv) < 0)
            throw_errno("socketpair");
        return {Socket(UniqueFd(sv[0])), Socket(UniqueFd(sv[1]))};
    }
    /* Moves are the defaults: they move the UniqueFd member. Copies are
     * implicitly deleted because UniqueFd is not copyable. */

    void bind(const UnixAddress &a)
    {
        if (::bind(fd_.get(), a.sa(), a.len()) < 0)
            throw_errno("bind");
    }
    void listen(int backlog = 16)
    {
        if (::listen(fd_.get(), backlog) < 0)
            throw_errno("listen");
    }
    Socket accept()
    {
        int c;
        do
            c = ::accept4(fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        while (c < 0 && errno == EINTR);
        if (c < 0)
            throw_errno("accept");
        return Socket(UniqueFd(c));
    }
    void connect(const UnixAddress &a)
    {
        if (::connect(fd_.get(), a.sa(), a.len()) < 0)
            throw_errno("connect");
    }
    void send_all(std::string_view data)
    {
        while (!data.empty()) {
            ssize_t n = ::send(fd_.get(), data.data(), data.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw_errno("send");
            data.remove_prefix(static_cast<size_t>(n));
        }
    }
    std::string recv_until_eof()
    {
        std::string out;
        char buf[256];
        for (;;) {
            ssize_t n = ::recv(fd_.get(), buf, sizeof buf, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw_errno("recv");
            if (n == 0)
                return out;
            out.append(buf, static_cast<size_t>(n));
        }
    }
    void shutdown_write()
    {
        if (::shutdown(fd_.get(), SHUT_WR) < 0)
            throw_errno("shutdown");
    }
    int fd() const { return fd_.get(); }
    bool valid() const { return static_cast<bool>(fd_); }

private:
    explicit Socket(UniqueFd fd) : fd_(std::move(fd)) {}
    UniqueFd fd_;
};

/* RAII for the filesystem side: mkdtemp on construction; unlink the socket
 * file and rmdir on destruction. */
class TempSocketDir {
public:
    TempSocketDir()
    {
        char tmpl[] = "/tmp/uds-cpp-XXXXXX";
        if (!::mkdtemp(tmpl))
            throw_errno("mkdtemp");
        dir_ = tmpl;
    }
    ~TempSocketDir()
    {
        ::unlink(sock_path().c_str()); /* may not exist: ignore ENOENT */
        ::rmdir(dir_.c_str());
    }
    TempSocketDir(const TempSocketDir &) = delete;
    TempSocketDir &operator=(const TempSocketDir &) = delete;
    std::string sock_path() const { return dir_ + "/server.sock"; }
    const std::string &dir() const { return dir_; }

private:
    std::string dir_;
};

static bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF; }

static std::string upper(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/* One echo-upper round trip: server in this process, client in a child. */
static void round_trip(const UnixAddress &addr)
{
    Socket srv(SOCK_STREAM);
    srv.bind(addr);
    srv.listen();

    pid_t pid = ::fork(); /* single-threaded: safe */
    CHECK(pid >= 0);
    if (pid == 0) {
        int rc = 1;
        try { /* never let an exception escape a forked child's frame */
            ::alarm(5);
            Socket c(SOCK_STREAM);
            c.connect(addr);
            c.send_all("raii keeps fds tidy");
            c.shutdown_write();
            rc = c.recv_until_eof() == "RAII KEEPS FDS TIDY" ? 0 : 1;
        } catch (const std::exception &e) {
            std::fprintf(stderr, "child: %s\n", e.what());
        }
        ::_exit(rc); /* _exit: skip the parent's static destructors */
    }

    Socket conn = srv.accept();
    std::string req = conn.recv_until_eof();
    conn.send_all(upper(req));
    /* The client reads until EOF, so we must signal EOF NOW. Waiting for
     * the destructor would be too late: we block in waitpid() first and
     * the two processes would wait for each other (until alarm fires). */
    conn.shutdown_write();
    std::cout << "  " << (addr.is_abstract() ? "abstract  " : "filesystem")
              << ": \"" << req << "\" -> \"" << upper(req) << "\"\n";
    int st = 0;
    CHECK(::waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
} /* conn and srv closed here, in reverse order of construction */

int main()
{
    std::signal(SIGPIPE, SIG_IGN);
    std::cout << std::unitbuf; /* flush every line: nothing duplicated by fork */

    std::cout << "1) RAII round trips\n";
    std::string kept_path;
    {
        TempSocketDir tmp;
        kept_path = tmp.sock_path();
        round_trip(UnixAddress::path(tmp.sock_path()));
        CHECK(::access(kept_path.c_str(), F_OK) == 0); /* file still there */
    } /* ~TempSocketDir: unlink + rmdir */
    CHECK(::access(kept_path.c_str(), F_OK) != 0);
    std::cout << "  TempSocketDir destructor removed " << kept_path << "\n";
    round_trip(UnixAddress::abstract("uds-cpp-" + std::to_string(::getpid())));

    std::cout << "2) move semantics\n";
    int raw;
    {
        Socket a(SOCK_STREAM);
        raw = a.fd();
        Socket b = std::move(a); /* ownership moves, no dup, no close */
        CHECK(!a.valid() && b.valid() && b.fd() == raw); // NOLINT(bugprone-use-after-move)
        std::vector<Socket> pool;
        pool.push_back(std::move(b)); /* vector may reallocate: moves again */
        for (int i = 0; i < 8; i++)
            pool.emplace_back(SOCK_DGRAM);
        CHECK(pool.front().fd() == raw && fd_is_open(raw));
        std::cout << "  fd " << raw << " moved a -> b -> vector, still open\n";
    } /* pool destroyed: every fd closed exactly once */
    CHECK(!fd_is_open(raw));
    std::cout << "  vector destroyed -> fd " << raw << " closed (EBADF)\n";

    std::cout << "3) errors are exceptions, not ignored return codes\n";
    try {
        Socket s(SOCK_STREAM);
        s.connect(UnixAddress::abstract("no-such-server-" + std::to_string(::getpid())));
        CHECK(false);
    } catch (const std::system_error &e) {
        std::cout << "  connect to nobody threw: " << e.what() << "\n";
        CHECK(e.code() == std::errc::connection_refused);
    }
    try {
        (void)UnixAddress::path(std::string(200, 'x'));
        CHECK(false);
    } catch (const std::length_error &e) {
        std::cout << "  200-byte path threw: " << e.what() << "\n";
    }

    std::cout << "01_raii_socket: all checks passed\n";
    return 0;
}
