/*
 * 12_c_str_lifetime_and_api.cpp — std::string in daily use: the API, C
 *                                 interop, c_str() lifetime, conversions
 *
 * Concept : std::string is a byte container with string helpers. You meet
 *           C APIs constantly on Linux (open, ioctl, GStreamer, sysfs), so
 *           you must know: how long c_str() stays valid, how to let a C
 *           function write INTO a std::string, that std::string can hold
 *           '\0', and how to convert numbers safely (stoi throws,
 *           from_chars doesn't). Every operation is annotated with its cost.
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/12_c_str_lifetime_and_api.cpp
 * Read    : ../notes/strings.md  (section "std::string in practice")
 */
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

// A stand-in for a C library function taking a C string (like open()).
extern "C" std::size_t c_api_len(const char *s) { return std::strlen(s); }

// A stand-in for a C API that WRITES into a caller buffer (like
// readlink, gethostname, snd_card_get_name): returns bytes it wanted.
extern "C" int c_api_fill(char *buf, std::size_t size)
{
    return std::snprintf(buf, size, "j721e-beagleboneai64 rev %d", 3);
}

// Replace every occurrence: O(n * k) worst case; advance past the inserted
// text so replacing "a" with "aa" doesn't loop forever.
static std::string replace_all(std::string s, std::string_view from,
                               std::string_view to)
{
    if (from.empty())
        return s;
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
         pos += to.size())
        s.replace(pos, from.size(), to);
    return s;
}

static std::string to_upper(std::string s)
{
    // ::toupper takes an int that must be an unsigned char value or EOF;
    // a plain char >= 0x80 is negative -> UB. Hence the lambda with a cast.
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return s;
}

int main()
{
    std::puts("== 1. c_str() lifetime ==");
    /*
     * c_str()/data() return a pointer into the string's CURRENT buffer.
     * It stays valid until the string is modified by a non-const operation
     * that may reallocate (append, insert, reserve, operator=, ...) or is
     * destroyed. Same rule as std::vector iterators.
     *
     *   std::string s = "short";
     *   const char *p = s.c_str();      p -> s's buffer
     *   s += " ...long text...";        buffer reallocated, old one freed
     *   puts(p);                        use-after-free (ASan: heap-use-after-free)
     *
     *   const char *q = std::string("tmp").c_str();
     *   puts(q);                        the temporary died at the ';' -> dangling
     *
     * Not executed here — it is UB. The safe patterns:
     */
    std::string dev = "/dev/video";
    dev += std::to_string(2);
    std::size_t n = c_api_len(dev.c_str()); // 1. call the C API in the same
                                            //    full-expression / before modifying
    CHECK(n == dev.size());
    const char *p = dev.c_str();
    const void *before = p;
    dev.reserve(200); // reallocation: p is now INVALID
    p = dev.c_str();  // 2. re-fetch after any modification
    std::printf("  c_str() moved from %p to %p after reserve(200) — re-fetch it\n",
                before, static_cast<const void *>(p));
    CHECK(std::strcmp(p, "/dev/video2") == 0);

    std::puts("\n== 2. let a C API write into a std::string ==");
    std::string host;
    int want = c_api_fill(nullptr, 0);           // ask how many bytes it needs
    host.resize(static_cast<std::size_t>(want)); // size chars + hidden '\0' slot
    // Since C++11 the buffer is contiguous and data()[size()] is a '\0' slot
    // you may overwrite with '\0' — so writing size()+1 bytes is allowed.
    // (C++17: data() returns non-const char*.)
    c_api_fill(host.data(), host.size() + 1);
    std::printf("  host = \"%s\" (size %zu)\n", host.c_str(), host.size());
    CHECK(host == "j721e-beagleboneai64 rev 3");

    std::puts("\n== 3. std::string can contain '\\0' (it stores the length) ==");
    std::string bin("ab\0cd", 5); // the (ptr, len) constructor keeps all 5
    std::string lit = "ab\0cd";   // the const char* constructor stops at '\0'!
    std::printf("  string(\"ab\\0cd\", 5).size() = %zu, string(\"ab\\0cd\").size() = %zu\n",
                bin.size(), lit.size());
    CHECK(bin.size() == 5 && lit.size() == 2);
    CHECK(std::strlen(bin.c_str()) == 2); // C APIs still see only "ab"
    using namespace std::string_literals;
    CHECK(("ab\0cd"s).size() == 5);       // operator""s also keeps the NUL

    std::puts("\n== 4. the everyday API, with costs ==");
    std::string s = "videotestsrc ! videoconvert ! autovideosink";
    CHECK(s.find("convert") == 20);            // O(n*m) worst, fast in practice
    CHECK(s.find("missing") == std::string::npos);
    CHECK(s.rfind('!') == 28);                 // search from the back
    CHECK(s.substr(0, 12) == "videotestsrc");  // O(len) + maybe an allocation
    CHECK(s.compare(0, 5, "video") == 0);      // O(5), no allocation
    CHECK(s.front() == 'v' && s.back() == 'k');
    std::string t = s;
    t.insert(0, "gst-launch-1.0 ");            // O(n): shifts everything right
    t.erase(t.size() - 13);                    // O(1) at the end
    t.append("fakesink");                      // amortised O(len)
    std::printf("  %s\n", t.c_str());
    CHECK(t == "gst-launch-1.0 videotestsrc ! videoconvert ! fakesink");
    CHECK(replace_all("a.b.c", ".", "::") == "a::b::c");
    CHECK(replace_all("aaa", "a", "aa") == "aaaaaa");
    CHECK(to_upper("uart2-\xC3\xA9") == "UART2-\xC3\xA9"); // non-ASCII untouched
    std::string a = "apple", b = "banana";
    CHECK(a < b);                              // lexicographic, like strcmp
    // s.at(i) bounds-checks and throws; s[i] does not (UB if i > size()).
    bool threw = false;
    try {
        (void)s.at(1000);
    } catch (const std::out_of_range &) {
        threw = true;
    }
    CHECK(threw);

    std::puts("\n== 5. numbers <-> strings ==");
    CHECK(std::to_string(-42) == "-42");
    CHECK(std::stoi("  123xyz") == 123); // stops at first non-digit, no error!
    threw = false;
    try {
        (void)std::stoi("abc");          // no digits -> std::invalid_argument
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)std::stoi("99999999999");  // too big -> std::out_of_range
    } catch (const std::out_of_range &) {
        threw = true;
    }
    CHECK(threw);
    // std::from_chars (C++17): no exceptions, no locale, no allocation,
    // tells you exactly where parsing stopped. The fastest and strictest.
    auto parse = [](std::string_view in, int &out) {
        auto [ptr, ec] = std::from_chars(in.data(), in.data() + in.size(), out);
        return ec == std::errc() && ptr == in.data() + in.size();
    };
    int v = 0;
    CHECK(parse("115200", v) && v == 115200);
    CHECK(!parse("12ab", v));        // trailing garbage detected
    CHECK(!parse("99999999999", v)); // errc::result_out_of_range
    char out[16];
    auto res = std::to_chars(out, out + sizeof out, 0x2800, 16);
    CHECK(std::string_view(out, static_cast<std::size_t>(res.ptr - out)) == "2800");
    std::puts("  to_string / stoi (throws) / from_chars+to_chars (no throw) checked");

    std::puts("\n== 6. split lines with getline / istringstream ==");
    std::istringstream in("eth0 up\nwlan0 down\nusb0 up\n");
    std::vector<std::string> ups;
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string ifname, state;
        fields >> ifname >> state;
        if (state == "up")
            ups.push_back(ifname);
    }
    std::printf("  interfaces up: %s, %s\n", ups[0].c_str(), ups[1].c_str());
    CHECK(ups.size() == 2 && ups[1] == "usb0");

    std::puts("\nall checks passed");
    return 0;
}
