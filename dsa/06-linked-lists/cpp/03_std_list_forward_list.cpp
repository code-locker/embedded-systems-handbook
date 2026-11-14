/*
 * 03_std_list_forward_list.cpp — std::list and std::forward_list: the API
 *                                that matters, iterator stability, splice,
 *                                member sort/merge/unique/remove_if, and an
 *                                LRU cache (list + unordered_map) preview
 *
 * Concept : std::list = doubly linked (bidirectional iterators, size() O(1)).
 *           std::forward_list = singly linked, the leanest possible list: no
 *           size(), no push_back, and every modifier is *_after because a
 *           singly list can only change the link that comes AFTER a node.
 *           Both: erase/insert never invalidate other iterators; std::sort
 *           doesn't work (needs random access) so they have member .sort().
 * Build   : make run P=dsa/06-linked-lists/cpp/03_std_list_forward_list.cpp
 * Read    : ../README.md  (sections "C vs C++", "Real world")
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <forward_list>
#include <iostream>
#include <iterator>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

template <typename C>
static std::vector<typename C::value_type> vec(const C &c) { return {c.begin(), c.end()}; }

template <typename C>
static void print(const char *label, const C &c)
{
    std::cout << label;
    for (const auto &v : c) std::cout << v << ' ';
    std::cout << '\n';
}

static void demo_list()
{
    std::list<int> l{5, 1, 4};
    l.push_front(0);                  // O(1)
    l.push_back(9);                   // O(1)
    auto four = std::find(l.begin(), l.end(), 4); // O(n): no indexing!
    l.insert(four, 3);                // O(1) given the iterator: before 4
    print("list:              ", l);
    CHECK((vec(l) == std::vector<int>{0, 5, 1, 3, 4, 9}));

    // Iterator stability: erase elements around `four`; it stays valid.
    // (With std::vector, erase before it would shift it and invalidate it.)
    l.erase(l.begin());
    l.remove(5);                      // removes ALL 5s, O(n)
    CHECK(*four == 4);

    // erase while iterating: use the iterator erase() returns.
    for (auto it = l.begin(); it != l.end();) {
        if (*it % 2 == 1) it = l.erase(it); // O(1) each
        else ++it;
    }
    print("after erase odd:   ", l);
    CHECK((vec(l) == std::vector<int>{4}));

    // Member sort: O(n log n) merge sort that RELINKS nodes (no copies).
    std::list<int> a{7, 3, 5, 1}, b{6, 2, 4};
    a.sort();
    b.sort();
    a.merge(b);                       // O(n+m), b becomes empty — like our C merge
    print("sort + merge:      ", a);
    CHECK((vec(a) == std::vector<int>{1, 2, 3, 4, 5, 6, 7}) && b.empty());

    // splice: move nodes between lists in O(1) — no allocation, no copy, and
    // iterators to the moved nodes stay valid (they now point into `l`).
    auto five = std::find(a.begin(), a.end(), 5);
    l.splice(l.end(), a, five);       // move one node
    CHECK(*five == 5);
    l.splice(l.begin(), a, a.begin(), std::next(a.begin(), 2)); // move range
    print("splice:            ", l);
    CHECK((vec(l) == std::vector<int>{1, 2, 4, 5}));
    CHECK((vec(a) == std::vector<int>{3, 4, 6, 7}));

    std::list<int> d{1, 1, 2, 2, 2, 3, 1};
    d.unique();                       // removes CONSECUTIVE duplicates only
    CHECK((vec(d) == std::vector<int>{1, 2, 3, 1}));
    d.reverse();
    CHECK((vec(d) == std::vector<int>{1, 3, 2, 1}));
}

static void demo_forward_list()
{
    std::forward_list<int> f{3, 4, 5};
    f.push_front(1);                  // O(1). There is no push_back.
    // Insert 2 after 1. To insert "before 3" you need the node BEFORE it.
    f.insert_after(f.begin(), 2);
    print("forward_list:      ", f);
    CHECK((vec(f) == std::vector<int>{1, 2, 3, 4, 5}));

    // before_begin(): a pseudo-iterator to the slot before the head — the
    // STL's version of the dummy node / pointer-to-pointer trick. It lets
    // erase_after remove the head with no special case.
    f.erase_after(f.before_begin());  // removes 1
    CHECK(f.front() == 2);

    // Remove all evens with prev/cur, the singly-list pattern.
    auto prev = f.before_begin();
    for (auto cur = f.begin(); cur != f.end();) {
        if (*cur % 2 == 0) cur = f.erase_after(prev);
        else { prev = cur; ++cur; }
    }
    print("drop evens:        ", f);
    CHECK((vec(f) == std::vector<int>{3, 5}));

    f.remove_if([](int v) { return v > 4; });
    // No size(): it would cost a counter per list. Use std::distance, O(n).
    CHECK(std::distance(f.begin(), f.end()) == 1);
    f.reverse();
    f.sort();

    std::cout << "sizeof(std::list<int>)=" << sizeof(std::list<int>)
              << " sizeof(std::forward_list<int>)=" << sizeof(std::forward_list<int>) << '\n';
    CHECK(sizeof(std::forward_list<int>) <= sizeof(std::list<int>));
}

// LRU cache preview (full version in the hashing lesson): a list keeps the
// usage order (front = most recent) and a hash map stores key -> list
// iterator. Both get and put are O(1) because splice moves a node to the
// front in O(1) and list iterators never get invalidated by other ops.
class LruCache {
    std::size_t cap_;
    std::list<std::pair<int, std::string>> order_; // front = most recently used
    std::unordered_map<int, decltype(order_)::iterator> where_;

public:
    explicit LruCache(std::size_t cap) : cap_(cap) {}

    std::optional<std::string> get(int key)
    {
        auto it = where_.find(key);
        if (it == where_.end()) return std::nullopt;
        order_.splice(order_.begin(), order_, it->second); // touch: O(1)
        return it->second->second;
    }

    void put(int key, std::string value)
    {
        if (auto it = where_.find(key); it != where_.end()) {
            it->second->second = std::move(value);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        if (order_.size() == cap_) {             // evict least recently used
            where_.erase(order_.back().first);
            order_.pop_back();
        }
        order_.emplace_front(key, std::move(value));
        where_[key] = order_.begin();
    }
};

static void demo_lru()
{
    LruCache c(2);
    c.put(1, "frame1");
    c.put(2, "frame2");
    CHECK(c.get(1) == std::optional<std::string>("frame1")); // 1 is now recent
    c.put(3, "frame3");                                      // evicts 2
    CHECK(!c.get(2).has_value());
    CHECK(c.get(3).has_value() && c.get(1).has_value());
    std::cout << "LRU cache:          evicted key 2 as expected\n";
}

int main()
{
    demo_list();
    demo_forward_list();
    demo_lru();
    std::cout << "03_std_list_forward_list: all checks passed\n";
    return 0;
}
