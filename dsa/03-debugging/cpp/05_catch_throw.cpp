/*
 * 05_catch_throw.cpp — a CORRECT program for practising gdb on C++: exceptions, STL, templates
 *
 * Concept : In C++ the hard question is often "WHO threw this?". By the time
 *           the catch block runs, the stack has been unwound. gdb's
 *           'catch throw' stops at the throw point with the full stack.
 *           Also practise pretty-printing of std::vector/std::map/std::string,
 *           breaking on a template instance, and 'rbreak'.
 * Build   : make run P=dsa/03-debugging/cpp/05_catch_throw.cpp
 *           gdb -q ./build/dsa/03-debugging/cpp/05_catch_throw
 *             (gdb) catch throw
 *             (gdb) run
 *             (gdb) bt              <- __cxa_throw <- parse_value <- load <- main
 *             (gdb) frame 1         <- parse_value, where the throw statement is
 *             (gdb) p key           <- "height"  (std::string pretty-printed)
 *             (gdb) p text          <- "1080px"
 *             (gdb) up              <- load(): 'p cfg' shows the std::map built so far
 * Read    : ../README.md  (section "3.1 gdb" -> "C++ specifics")
 */
#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

template <typename T>
static T clamp_to(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }   // 'break clamp_to<int>'

static int parse_value(const std::string &key, const std::string &text)
{
    std::size_t used = 0;
    int v = std::stoi(text, &used);              // throws std::invalid_argument on "abc"
    if (used != text.size())
        throw std::runtime_error("trailing junk in '" + key + "'");
    return clamp_to(v, 0, 240);
}

static std::map<std::string, int> load(const std::vector<std::pair<std::string, std::string>> &raw)
{
    std::map<std::string, int> cfg;
    for (const auto &[k, v] : raw)
        cfg[k] = parse_value(k, v);
    return cfg;
}

int main()
{
    auto good = load({{"fps", "30"}, {"width", "1920"}, {"gain", "500"}});
    assert(good.at("fps") == 30 && good.at("gain") == 240);   // clamped

    int caught = 0;
    try {
        load({{"fps", "30"}, {"height", "1080px"}});           // throws in parse_value
    } catch (const std::runtime_error &e) {
        std::cout << "caught: " << e.what() << '\n';
        caught++;
    }
    try {
        load({{"fps", "abc"}});                                 // std::stoi throws
    } catch (const std::invalid_argument &e) {
        std::cout << "caught invalid_argument from " << e.what() << '\n';
        caught++;
    }
    assert(caught == 2);
    std::cout << "All checks passed.\n";
    return 0;
}
