/*
 * 02_child_process_reader.cpp — run a program and read its stdout line by
 *                               line: a small, safe popen() replacement
 *
 * Concept : popen() goes through /bin/sh (quoting / injection problems) and
 *           gives you a FILE*. Here we fork + execvp the argv directly,
 *           connect the child's stdout to a pipe with dup2, and split the
 *           byte stream into lines ourselves — a line may arrive in several
 *           read() calls, and one read() may contain several lines.
 *           The destructor closes the pipe and reaps the child, so no
 *           zombie is left behind even on an exception.
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/cpp/02_child_process_reader.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fflush(stdout);                                             \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

class ChildProcess {
public:
    // Start argv[0] with arguments; its stdout is ours to read.
    explicit ChildProcess(const std::vector<std::string> &argv)
    {
        // Build the char* array BEFORE fork: after fork, in a child of a
        // multi-threaded program, only async-signal-safe calls are allowed
        // (no malloc). Keeping the child side to dup2/close/execvp/_exit is
        // a good habit even in a single-threaded program.
        std::vector<char *> args;
        for (const auto &a : argv)
            args.push_back(const_cast<char *>(a.c_str()));
        args.push_back(nullptr);

        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0)
            throw std::system_error(errno, std::generic_category(), "pipe2");
        std::fflush(stdout);
        pid_ = ::fork();
        if (pid_ < 0) {
            int e = errno;
            ::close(fds[0]);
            ::close(fds[1]);
            throw std::system_error(e, std::generic_category(), "fork");
        }
        if (pid_ == 0) {
            // child: stdout -> pipe. dup2 CLEARS close-on-exec on fd 1, so
            // the new program keeps it; the O_CLOEXEC originals vanish at
            // exec automatically — no stray write end survives.
            if (::dup2(fds[1], STDOUT_FILENO) < 0)
                _exit(126);
            ::alarm(5);
            ::execvp(args[0], args.data());
            _exit(127); // like the shell: "command not found"
        }
        ::close(fds[1]); // parent: keep only the read end
        fd_ = fds[0];
    }

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;

    ~ChildProcess()
    {
        if (fd_ >= 0)
            ::close(fd_); // child may get SIGPIPE if it still writes: fine
        if (pid_ > 0 && !waited_)
            (void)reap();
    }

    // Next line without the '\n', or nullopt at EOF. The last line is
    // returned even if it has no trailing newline.
    std::optional<std::string> read_line()
    {
        for (;;) {
            auto nl = buf_.find('\n', scan_from_);
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                scan_from_ = 0;
                return line;
            }
            scan_from_ = buf_.size(); // don't rescan what we already checked
            if (eof_) {
                if (buf_.empty())
                    return std::nullopt;
                return std::exchange(buf_, std::string{});
            }
            char chunk[256]; // small on purpose: lines WILL be split
            ssize_t n = ::read(fd_, chunk, sizeof chunk);
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                throw std::system_error(errno, std::generic_category(), "read");
            if (n == 0)
                eof_ = true;
            else
                buf_.append(chunk, static_cast<size_t>(n));
            reads_++;
        }
    }

    // Wait for the child; returns its exit code, or 128+signal like a shell.
    int wait()
    {
        CHECK(!waited_);
        return reap();
    }

    int read_calls() const { return reads_; }

private:
    int reap()
    {
        int st = 0;
        pid_t r;
        do
            r = ::waitpid(pid_, &st, 0);
        while (r < 0 && errno == EINTR);
        waited_ = true;
        if (r != pid_)
            return -1;
        if (WIFEXITED(st))
            return WEXITSTATUS(st);
        return 128 + WTERMSIG(st);
    }

    pid_t pid_ = -1;
    int fd_ = -1;
    bool waited_ = false;
    bool eof_ = false;
    std::string buf_;
    size_t scan_from_ = 0;
    int reads_ = 0;
};

int main()
{
    ::alarm(10);

    // 1. A few lines, the last one without a newline.
    {
        ChildProcess p({"sh", "-c",
                        "for i in 1 2 3; do echo line $i; done; "
                        "printf 'no newline at end'"});
        std::vector<std::string> lines;
        while (auto l = p.read_line())
            lines.push_back(*l);
        int code = p.wait();
        std::printf("1) got %zu lines, exit code %d:\n", lines.size(), code);
        for (const auto &l : lines)
            std::printf("     \"%s\"\n", l.c_str());
        CHECK(code == 0);
        CHECK((lines == std::vector<std::string>{
                            "line 1", "line 2", "line 3", "no newline at end"}));
    }

    // 2. Many lines: they straddle our 256-byte read() chunks.
    {
        ChildProcess p({"seq", "1", "2000"});
        long sum = 0, count = 0;
        while (auto l = p.read_line()) {
            sum += std::stol(*l);
            count++;
        }
        CHECK(p.wait() == 0);
        std::printf("2) seq 1 2000: %ld lines, sum %ld, %d read() calls "
                    "(lines crossed chunk boundaries)\n",
                    count, sum, p.read_calls());
        CHECK(count == 2000);
        CHECK(sum == 2000L * 2001 / 2);
    }

    // 3. Exit codes, including "command not found" from our own _exit(127).
    {
        ChildProcess fail({"sh", "-c", "echo partial; exit 3"});
        CHECK(fail.read_line() == std::optional<std::string>("partial"));
        CHECK(!fail.read_line());
        CHECK(fail.wait() == 3);
        ChildProcess missing({"/nonexistent/program"});
        CHECK(!missing.read_line()); // exec failed: no output, EOF
        CHECK(missing.wait() == 127);
        std::printf("3) exit code 3 propagated; missing program -> 127\n");
    }

    // 4. Destructor without wait(): no zombie left behind.
    {
        ChildProcess p({"echo", "ignored"});
    } // destructor closes the pipe and reaps the child (echo may even get
      // SIGPIPE if it writes after the close; the reap still happens)
    errno = 0;
    CHECK(::waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
    std::printf("4) destructor reaped the child: waitpid(-1) -> ECHILD "
                "(no children, no zombies)\n");

    std::printf("02_child_process_reader: all checks passed\n");
    return 0;
}
