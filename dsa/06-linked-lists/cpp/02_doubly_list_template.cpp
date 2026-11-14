/*
 * 02_doubly_list_template.cpp — DList<T>: a doubly linked list with a
 *                               sentinel node and bidirectional iterators,
 *                               i.e. a tiny std::list
 *
 * Concept : std::list is a circular doubly linked list with one sentinel
 *           ("end") node. The sentinel makes every insert/erase the same four
 *           pointer writes — no null checks, no head/tail special cases —
 *           and end() is simply the sentinel, so --end() is the last element.
 *           Here the list owns raw nodes and frees them in its destructor
 *           (RAII), because unique_ptr cannot express the two-way links.
 * Build   : make run P=dsa/06-linked-lists/cpp/02_doubly_list_template.cpp
 * Read    : ../README.md  (sections "Doubly linked list", "C vs C++")
 */
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <string>
#include <type_traits>
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

template <typename T>
class DList {
    // Base holds only links, so the sentinel does not need a T (T may not be
    // default-constructible). Real nodes derive from it and add the value.
    struct Link {
        Link *prev;
        Link *next;
    };
    struct Node : Link {
        T value;
        template <typename... Args>
        explicit Node(Args &&...args) : Link{nullptr, nullptr}, value(std::forward<Args>(args)...) {}
    };

    //   empty:        +---------------+
    //                 v               |
    //            [sentinel] ---next---+   (prev also points at itself)
    //
    //   3 items:  [S] <=> [a] <=> [b] <=> [c] <=> (back to [S])
    //             end()   begin()                 --end() == c
    Link sentinel_{&sentinel_, &sentinel_};
    std::size_t size_ = 0;

    // O(1): link n in front of pos — the ONE insert routine.
    //   [p] <=> [pos]   ==>   [p] <=> [n] <=> [pos]
    static void link_before(Link *pos, Link *n) noexcept
    {
        n->prev = pos->prev;
        n->next = pos;
        pos->prev->next = n;
        pos->prev = n;
    }
    // O(1): the ONE erase routine.  [p] <=> [x] <=> [q]  ==>  [p] <=> [q]
    static void unlink(Link *x) noexcept
    {
        x->prev->next = x->next;
        x->next->prev = x->prev;
    }

public:
    template <bool Const>
    class Iter {
        using LinkPtr = std::conditional_t<Const, const Link *, Link *>;
        LinkPtr l_;
        friend class DList;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<Const, const T *, T *>;
        using reference = std::conditional_t<Const, const T &, T &>;

        explicit Iter(LinkPtr l = nullptr) : l_(l) {}
        Iter(const Iter<false> &o) : l_(o.l_) {} // iterator -> const_iterator
        // static_cast is valid: every non-sentinel Link IS-A Node.
        reference operator*() const
        {
            using NodePtr = std::conditional_t<Const, const Node *, Node *>;
            return static_cast<NodePtr>(l_)->value;
        }
        pointer operator->() const { return &**this; }
        Iter &operator++() { l_ = l_->next; return *this; } // O(1)
        Iter &operator--() { l_ = l_->prev; return *this; } // O(1)
        Iter operator++(int) { Iter t = *this; ++*this; return t; }
        Iter operator--(int) { Iter t = *this; --*this; return t; }
        friend bool operator==(const Iter &a, const Iter &b) { return a.l_ == b.l_; }
        friend bool operator!=(const Iter &a, const Iter &b) { return a.l_ != b.l_; }
    };
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    DList() = default;
    DList(std::initializer_list<T> il) { for (const T &v : il) push_back(v); }
    DList(const DList &o) { for (const T &v : o) push_back(v); }   // O(n)
    DList &operator=(DList o) noexcept { swap(o); return *this; } // copy/move-and-swap
    // Move is O(1) but must re-point the neighbours of the sentinel, because
    // the sentinel lives INSIDE the object (it has a new address). Classic trap.
    DList(DList &&o) noexcept { swap(o); }
    ~DList() { clear(); }

    void swap(DList &o) noexcept
    {
        std::swap(sentinel_, o.sentinel_);
        std::swap(size_, o.size_);
        fix_sentinel();
        o.fix_sentinel();
    }

    template <typename... Args>
    iterator emplace(const_iterator pos, Args &&...args)
    {
        Node *n = new Node(std::forward<Args>(args)...); // owner: this list
        link_before(const_cast<Link *>(pos.l_), n);
        ++size_;
        return iterator(n);
    }
    iterator insert(const_iterator pos, T v) { return emplace(pos, std::move(v)); }

    // O(1). Returns the iterator after the erased element. Other iterators
    // stay valid — the property that makes lists useful (vector can't do it).
    iterator erase(const_iterator pos)
    {
        Link *x = const_cast<Link *>(pos.l_);
        Link *next = x->next;
        unlink(x);
        delete static_cast<Node *>(x);
        --size_;
        return iterator(next);
    }

    void push_front(T v) { emplace(begin(), std::move(v)); }
    void push_back(T v) { emplace(end(), std::move(v)); }
    void pop_front() { erase(begin()); }
    void pop_back() { erase(std::prev(end())); }

    void clear() noexcept
    {
        Link *l = sentinel_.next;
        while (l != &sentinel_) {
            Link *next = l->next;
            delete static_cast<Node *>(l);
            l = next;
        }
        sentinel_.prev = sentinel_.next = &sentinel_;
        size_ = 0;
    }

    // O(n): swap every node's prev/next, then the sentinel's. No allocation.
    void reverse() noexcept
    {
        Link *l = &sentinel_;
        do {
            std::swap(l->prev, l->next);
            l = l->prev; // the old next
        } while (l != &sentinel_);
    }

    // O(1): move [first, last) from other into *this before pos. No copies,
    // no allocation — the reason std::list::splice exists.
    void splice(const_iterator pos, DList &other, const_iterator first, const_iterator last)
    {
        if (first == last) return;
        std::size_t moved = static_cast<std::size_t>(std::distance(first, last)); // O(k) for size
        Link *f = const_cast<Link *>(first.l_);
        Link *lst = const_cast<Link *>(last.l_)->prev; // last element moved
        Link *p = const_cast<Link *>(pos.l_);
        // cut out of other
        f->prev->next = lst->next;
        lst->next->prev = f->prev;
        // stitch before p
        f->prev = p->prev;
        lst->next = p;
        p->prev->next = f;
        p->prev = lst;
        other.size_ -= moved;
        size_ += moved;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    T &front() { return *begin(); }
    T &back() { return *std::prev(end()); }

    iterator begin() noexcept { return iterator(sentinel_.next); }
    iterator end() noexcept { return iterator(&sentinel_); }
    const_iterator begin() const noexcept { return const_iterator(sentinel_.next); }
    const_iterator end() const noexcept { return const_iterator(&sentinel_); }

    // Debug: verify both directions agree. O(n).
    bool invariant() const
    {
        std::size_t n = 0;
        for (const Link *l = sentinel_.next; l != &sentinel_; l = l->next, ++n)
            if (l->next->prev != l) return false;
        return n == size_;
    }

private:
    void fix_sentinel() noexcept
    {
        if (size_ == 0) {
            sentinel_.next = sentinel_.prev = &sentinel_;
        } else {
            sentinel_.next->prev = &sentinel_;
            sentinel_.prev->next = &sentinel_;
        }
    }
};

template <typename T>
static std::vector<T> to_vec(const DList<T> &l) { return {l.begin(), l.end()}; }

template <typename T>
static void print(const char *label, const DList<T> &l)
{
    std::cout << label << "[S] <=> ";
    for (const auto &v : l) std::cout << v << " <=> ";
    std::cout << "[S]\n";
}

int main()
{
    DList<int> l{2, 3};
    l.push_front(1);
    l.push_back(4);
    print("push:      ", l);
    CHECK((to_vec(l) == std::vector<int>{1, 2, 3, 4}) && l.invariant());

    // Iterator stability: keep an iterator to 3, erase around it.
    auto three = std::find(l.begin(), l.end(), 3);
    l.erase(std::find(l.begin(), l.end(), 2));
    l.pop_front();
    l.pop_back();
    CHECK(*three == 3 && l.size() == 1); // still valid after other erases
    l.insert(three, 99);                 // before 3
    print("erase/ins: ", l);
    CHECK((to_vec(l) == std::vector<int>{99, 3}) && l.invariant());

    // Bidirectional: walk backwards with reverse iterators.
    DList<int> r{1, 2, 3, 4, 5};
    std::vector<int> back(std::make_reverse_iterator(r.end()), std::make_reverse_iterator(r.begin()));
    CHECK((back == std::vector<int>{5, 4, 3, 2, 1}));
    r.reverse();
    print("reverse:   ", r);
    CHECK((to_vec(r) == std::vector<int>{5, 4, 3, 2, 1}) && r.invariant());

    // splice: move [4, 2) from r to the front of l. O(1) relinking.
    auto first = std::next(r.begin());     // 4
    auto last = std::next(r.begin(), 4);   // 1 (exclusive)
    l.splice(l.begin(), r, first, last);
    print("splice l:  ", l);
    print("splice r:  ", r);
    CHECK((to_vec(l) == std::vector<int>{4, 3, 2, 99, 3}) && l.invariant());
    CHECK((to_vec(r) == std::vector<int>{5, 1}) && r.invariant());

    // Move: the sentinel is part of the object, so neighbours must be fixed.
    DList<int> moved = std::move(l);
    CHECK(moved.size() == 5 && moved.invariant() && l.empty() && l.invariant()); // NOLINT
    moved.push_back(6);
    CHECK(moved.back() == 6);
    DList<int> copy = moved;
    copy = r; // copy-assign
    CHECK((to_vec(copy) == std::vector<int>{5, 1}) && copy.invariant());

    // Non-default-constructible-friendly and non-trivial T.
    DList<std::string> s;
    s.emplace(s.end(), 3, 'x'); // constructs "xxx" in place
    s.push_front("v4l2src");
    CHECK(s.front() == "v4l2src" && s.back() == "xxx");

    std::cout << "02_doubly_list_template: all checks passed\n";
    return 0;
}
