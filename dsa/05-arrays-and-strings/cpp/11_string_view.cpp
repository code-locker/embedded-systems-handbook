/*
 * 11_string_view.cpp — std::string_view: a (pointer, length) window, no owning
 *
 * Concept : string_view is two words: a pointer to characters someone ELSE
 *           owns, and a length. Copying it, slicing it (substr), trimming it
 *           are all O(1) and allocate nothing. It accepts std::string, string
 *           literals and char buffers alike, so it is the best type for
 *           read-only string PARAMETERS. The price: it does not keep the
 *           characters alive (dangling) and is not NUL-terminated (never pass
 *           .data() to a C function expecting a C string).
 * Build   : make run P=dsa/05-arrays-and-strings/cpp/11_string_view.cpp
 * Read    : ../notes/strings.md  (section "std::string_view")
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

// Count heap allocations to prove string_view operations make none.
static std::size_t g_allocs = 0;
void *operator new(std::size_t n)
{
    ++g_allocs;
    if (void *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

using namespace std::literals; // "abc"sv  and  "abc"s

/*
 *   std::string owner = "gst-launch-1.0 videotestsrc";   (heap-owned bytes)
 *
 *   owner : [ptr|size|cap] --> g s t - l a u n c h - 1 . 0   v i d e o ...
 *                               ^                           ^
 *   string_view sv = owner;     |                           |
 *   sv    : [ptr|len=27] -------+                           |
 *   sv.substr(15) : [ptr|len=12] ---------------------------+
 *
 *   No copy, no allocation: just new (pointer, length) pairs.
 */

// Read-only parameter: accepts std::string, "literal", char[] without
// constructing a temporary std::string (const std::string& would allocate
// for a literal longer than the SSO buffer).
static std::size_t count_char(std::string_view s, char c)
{
    std::size_t n = 0;
    for (char ch : s) // O(len)
        n += (ch == c);
    return n;
}

// O(1) trims: they only move the pointer / shrink the length.
static std::string_view trim(std::string_view s)
{
    const char *ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos)
        return {};
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

// Split without allocating per token: every piece views into `s`.
// The VECTOR allocates, the pieces don't. The views are valid only while
// the original characters are alive.
static std::vector<std::string_view> split(std::string_view s, char sep)
{
    std::vector<std::string_view> out;
    while (true) {
        auto pos = s.find(sep);
        out.push_back(s.substr(0, pos)); // substr: O(1), no allocation
        if (pos == std::string_view::npos)
            break;
        s.remove_prefix(pos + 1); // O(1): pointer += pos+1, len -= pos+1
    }
    return out;
}

// Parse "key=value" lines of a config text, entirely with views.
struct KV {
    std::string_view key, value;
};
static bool parse_kv(std::string_view line, KV &out)
{
    auto eq = line.find('=');
    if (eq == std::string_view::npos)
        return false;
    out.key = trim(line.substr(0, eq));
    out.value = trim(line.substr(eq + 1));
    return !out.key.empty();
}

/* ------------------------------------------------------------------------
 * DANGLING — the one rule: a view must not outlive what it views.
 * These are shown, NOT executed (they are undefined behaviour):
 *
 *   std::string_view bad1() {
 *       std::string s = "temporary";
 *       return s;                 // s is destroyed at '}' -> view dangles
 *   }
 *
 *   std::string_view bad2 = std::string("a") + "b";
 *                                 // the temporary dies at the ';'
 *
 *   std::string owner = "short";
 *   std::string_view v = owner;
 *   owner += " and now much longer than fifteen chars";
 *                                 // reallocation: v points to freed memory
 *
 *   auto v3 = "abc"s;             // std::string — fine (owns)
 *   std::string_view v4 = "abc"s; // dangling: temporary std::string
 *   std::string_view v5 = "abc"sv;// fine: views a string LITERAL (static)
 *
 * ASan reports these as heap-use-after-free / stack-use-after-scope;
 * GCC 13+ warns for some with -Wdangling-reference.
 * Rule of thumb: string_view for PARAMETERS and local slicing; std::string
 * for anything you STORE (struct members, containers, return values that
 * outlive their source).
 * --------------------------------------------------------------------- */

int main()
{
    std::puts("== 1. what a view is ==");
    std::printf("  sizeof(std::string_view) = %zu (pointer + length)\n",
                sizeof(std::string_view));
    CHECK(sizeof(std::string_view) == 2 * sizeof(void *));

    std::string owner = "gst-launch-1.0 videotestsrc ! autovideosink";
    std::size_t before = g_allocs;
    std::string_view sv = owner;              // O(1), no copy
    std::string_view tail = sv.substr(15);    // O(1); std::string::substr is O(n) + alloc
    std::string_view first = sv.substr(0, 14);
    CHECK(g_allocs == before); // nothing allocated
    CHECK(tail.data() == owner.data() + 15); // same memory, just offset
    std::printf("  first = \"%.*s\", tail starts at owner+%td\n",
                static_cast<int>(first.size()), first.data(),
                tail.data() - owner.data());

    std::puts("\n== 2. NOT NUL-terminated: never printf(\"%s\", sv.data()) ==");
    /*  first views "gst-launch-1.0" but the byte after it is ' ' not '\0':
     *    printf("%s", first.data())  would print the WHOLE rest of owner.
     *  Print with an explicit length:  printf("%.*s", (int)sv.size(), sv.data())
     *  Need a C string for a C API? make one: std::string(sv).c_str()       */
    CHECK(first.data()[first.size()] == ' ');
    std::string for_c_api(first); // explicit copy -> terminated
    CHECK(std::strlen(for_c_api.c_str()) == 14);
    std::printf("  byte after the view is '%c', not '\\0'; std::string(sv) "
                "gives a terminated copy\n", first.data()[first.size()]);

    std::puts("\n== 3. one parameter type for every kind of string ==");
    char raw[] = "a,b,c";
    CHECK(count_char(owner, '-') == 2);       // std::string
    CHECK(count_char("1,2,3,4", ',') == 3);   // literal: no temporary string
    CHECK(count_char(raw, ',') == 2);         // char array
    CHECK(count_char("x,y"sv, ',') == 1);     // string_view literal
    std::puts("  count_char works on std::string, literal, char[] and sv");

    std::puts("\n== 4. zero-allocation trim / split / parse ==");
    std::string config = "  baud = 115200\nparity=none\n# comment\n data_bits = 8 ";
    std::vector<KV> kvs;
    kvs.reserve(8);
    before = g_allocs;
    for (std::string_view line : split(config, '\n')) { // split's vector: 1+ allocs
        KV kv;
        if (parse_kv(line, kv))
            kvs.push_back(kv);
    }
    std::size_t used = g_allocs - before;
    for (const KV &kv : kvs)
        std::printf("  [%.*s] -> [%.*s]\n", static_cast<int>(kv.key.size()),
                    kv.key.data(), static_cast<int>(kv.value.size()),
                    kv.value.data());
    std::printf("  allocations: %zu (only the vector inside split; no per-token strings)\n",
                used);
    CHECK(kvs.size() == 3);
    CHECK(kvs[0].key == "baud" && kvs[0].value == "115200");
    CHECK(kvs[2].key == "data_bits" && kvs[2].value == "8");
    CHECK(used <= 4); // vector growth only

    std::puts("\n== 5. useful O(1)/O(n) members ==");
    std::string_view path = "/sys/class/gpio/gpio42/value";
    CHECK(path.substr(path.rfind('/') + 1) == "value");
    std::string_view p2 = path;
    p2.remove_prefix(5);  // drop "/sys/"
    p2.remove_suffix(6);  // drop "/value"
    CHECK(p2 == "class/gpio/gpio42");
    CHECK(path.compare(0, 5, "/sys/") == 0);
    // C++20 adds starts_with/ends_with; in C++17 use substr/compare.
    CHECK(path.substr(0, 4) == "/sys");
    std::printf("  basename = %.*s, middle = %.*s\n", 5,
                path.data() + path.size() - 5, static_cast<int>(p2.size()),
                p2.data());

    std::puts("\n== 6. storing views safely: own the data ==");
    std::vector<std::string> stored;          // owners
    {
        std::string temp = "made inside a scope, longer than SSO";
        std::string_view v = temp;
        stored.emplace_back(v);               // copy INTO an owner before temp dies
    }                                         // temp destroyed; stored[0] is fine
    CHECK(stored[0] == "made inside a scope, longer than SSO");
    std::puts("  copied into a std::string before the source died: safe");

    std::puts("\nall checks passed");
    return 0;
}
