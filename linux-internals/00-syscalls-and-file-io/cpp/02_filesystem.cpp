/*
 * 02_filesystem.cpp — std::filesystem (C++17): paths, creating and walking
 *                     directories, file_size, rename, permissions, and the
 *                     throwing vs std::error_code overloads
 *
 * Concept : std::filesystem is the portable wrapper over stat, mkdir,
 *           opendir/readdir, rename, unlink and chmod. path does the string
 *           work (joining with '/', stem, extension) that C code gets wrong
 *           with snprintf. Every function comes in two forms: one throws
 *           filesystem_error, one takes a std::error_code& and never throws
 *           — use that one on paths where failure is normal (does the
 *           config exist?) or where exceptions are disabled (some embedded
 *           builds use -fno-exceptions).
 *           It does NOT replace file descriptors: no fsync, no locks, no
 *           ioctl, no O_EXCL. On GCC >= 9 no extra library is needed.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/cpp/02_filesystem.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fflush(stdout);                                             \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

static void touch(const fs::path &p, const std::string &text)
{
    std::ofstream(p) << text; // temporary stream: written and closed now
}

int main()
{
    ::alarm(10);
    // mkdtemp gives a unique, private (0700) directory; std::filesystem has
    // no equivalent (temp_directory_path() is only "/tmp").
    std::string tmpl = (fs::temp_directory_path() /
                        ("fsdemo_" + std::to_string(::getpid()) + "_XXXXXX"))
                           .string();
    CHECK(::mkdtemp(tmpl.data()) != nullptr);
    const fs::path root = tmpl;

    // ---- 1. path arithmetic ----
    fs::path cfg = root / "etc" / "camera" / "sensor.conf";
    std::printf("1) path: %s\n   parent_path().filename()=%s\n   filename=%s stem=%s "
                "extension=%s\n", cfg.c_str(),
                cfg.parent_path().filename().c_str(), cfg.filename().c_str(),
                cfg.stem().c_str(), cfg.extension().c_str());
    CHECK(cfg.extension() == ".conf" && cfg.stem() == "sensor");

    // ---- 2. create a tree ----
    CHECK(fs::create_directories(cfg.parent_path())); // like mkdir -p
    touch(cfg, "exposure=10\n");
    fs::create_directories(root / "var" / "log");
    touch(root / "var" / "log" / "a.log", "aaaa");
    touch(root / "var" / "log" / "b.log", "bbbbbbbb");
    fs::create_directory_symlink("var/log", root / "logs");
    CHECK(fs::file_size(cfg) == 12);
    CHECK(fs::is_symlink(fs::symlink_status(root / "logs")));
    std::printf("2) created etc/camera/sensor.conf (%ju bytes), var/log/*.log, "
                "symlink logs -> var/log\n",
                static_cast<uintmax_t>(fs::file_size(cfg)));

    // ---- 3. walk it (recursive_directory_iterator = opendir/readdir/stat) --
    std::vector<std::string> seen;
    uintmax_t bytes = 0;
    for (const auto &e : fs::recursive_directory_iterator(root)) {
        // Does not follow directory symlinks unless asked
        // (directory_options::follow_directory_symlink): no loops.
        // lexically_relative: pure string work. fs::relative() would
        // RESOLVE the symlink and print "logs" as "var/log".
        bool link = e.is_symlink();
        seen.push_back(e.path().lexically_relative(root).string() +
                       (link ? "@" : e.is_directory() ? "/" : ""));
        if (e.is_regular_file() && !e.is_symlink())
            bytes += e.file_size();
    }
    std::sort(seen.begin(), seen.end()); // iteration order is unspecified
    std::printf("3) recursive walk:");
    for (const auto &s : seen)
        std::printf(" %s", s.c_str());
    std::printf("\n   %zu entries, %ju bytes in regular files\n", seen.size(),
                bytes);
    CHECK(seen.size() == 8 && bytes == 12 + 4 + 8);

    // ---- 4. rename, permissions ----
    fs::rename(root / "var" / "log" / "a.log", root / "var" / "log" / "a.old");
    CHECK(!fs::exists(root / "var" / "log" / "a.log"));
    fs::permissions(cfg, fs::perms::owner_read | fs::perms::group_read);
    auto p = fs::status(cfg).permissions();
    CHECK((p & fs::perms::owner_write) == fs::perms::none);
    std::printf("4) renamed a.log -> a.old; sensor.conf mode now %o\n",
                static_cast<unsigned>(p));

    // ---- 5. error_code vs exceptions ----
    std::error_code ec;
    auto sz = fs::file_size(root / "missing.conf", ec); // never throws
    CHECK(ec && sz == static_cast<uintmax_t>(-1));
    std::printf("5) file_size(missing, ec): ec = \"%s\" (errno %d), no "
                "exception\n", ec.message().c_str(), ec.value());
    CHECK(ec == std::errc::no_such_file_or_directory);
    try {
        (void)fs::file_size(root / "missing.conf");
        CHECK(false);
    } catch (const fs::filesystem_error &e) {
        std::printf("   file_size(missing) throws filesystem_error: %s\n",
                    e.what());
        CHECK(e.path1() == root / "missing.conf");
    }

    // ---- 6. space() = statvfs ----
    auto si = fs::space(root);
    std::printf("6) filesystem of %s: %ju MiB free of %ju MiB (printed)\n",
                root.parent_path().c_str(), si.available >> 20,
                si.capacity >> 20);

    // ---- cleanup: remove_all = rm -rf (does not follow the symlink) ----
    auto n = fs::remove_all(root);
    std::printf("   remove_all removed %ju entries\n",
                static_cast<uintmax_t>(n));
    CHECK(n == 9 && !fs::exists(root)); // 8 inside + root itself
    std::printf("02_filesystem: all checks passed\n");
    return 0;
}
