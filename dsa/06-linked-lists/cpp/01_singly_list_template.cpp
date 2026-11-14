/*
 * 01_singly_list_template.cpp — SinglyList<T>: a singly linked list template
 *                               with unique_ptr ownership, forward iterators,
 *                               copy/move semantics and range-for support
 *
 * Concept : Each node OWNS the next node through std::unique_ptr, so the
 *           whole chain is freed automatically (RAII) — no free() to forget.
 *           Trap: the default destructor would destroy the chain RECURSIVELY
 *           (node 1's unique_ptr destroys node 2, which destroys node 3...),
 *           one stack frame per node -> stack overflow on long lists. So we
 *           write an iterative clear(). Iterators make it work with range-for
 *           and <algorithm>.
 * Build   : make run P=dsa/06-linked-lists/cpp/01_singly_list_template.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <memory>
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
class SinglyList {
    struct Node {
        T value;
        std::unique_ptr<Node> next; // owning link
        explicit Node(T v) : value(std::move(v)) {}
    };

    std::unique_ptr<Node> head_; // owns node 1, which owns node 2, ...
    Node *tail_ = nullptr;       // NON-owning shortcut for O(1) push_back
    std::size_t size_ = 0;

public:
    // ---- iterator: just a wrapper around a Node* -------------------------
    template <bool Const>
    class Iter {
        using NodePtr = std::conditional_t<Const, const Node *, Node *>;
        NodePtr n_;
        friend class SinglyList;

    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<Const, const T *, T *>;
        using reference = std::conditional_t<Const, const T &, T &>;

        explicit Iter(NodePtr n = nullptr) : n_(n) {}
        reference operator*() const { return n_->value; }
        pointer operator->() const { return &n_->value; }
        Iter &operator++() { n_ = n_->next.get(); return *this; }   // O(1)
        Iter operator++(int) { Iter t = *this; ++*this; return t; }
        friend bool operator==(Iter a, Iter b) { return a.n_ == b.n_; }
        friend bool operator!=(Iter a, Iter b) { return a.n_ != b.n_; }
    };
    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    SinglyList() = default;
    SinglyList(std::initializer_list<T> il) { for (const T &v : il) push_back(v); }

    // Rule of five. Copy = deep copy, O(n). Move = steal the head, O(1).
    SinglyList(const SinglyList &o) { for (const T &v : o) push_back(v); }
    SinglyList &operator=(const SinglyList &o)
    {
        if (this != &o) { SinglyList tmp(o); swap(tmp); } // copy-and-swap
        return *this;
    }
    SinglyList(SinglyList &&o) noexcept { swap(o); }
    // Move-assign by swap: o receives our old nodes and frees them in its dtor.
    SinglyList &operator=(SinglyList &&o) noexcept { swap(o); return *this; }
    ~SinglyList() { clear(); }

    void swap(SinglyList &o) noexcept
    {
        std::swap(head_, o.head_);
        std::swap(tail_, o.tail_);
        std::swap(size_, o.size_);
    }

    // O(n), ITERATIVE. Detach the head's successor before destroying the head,
    // so each unique_ptr dies holding nothing: no recursion.
    void clear() noexcept
    {
        while (head_) head_ = std::move(head_->next); // old head destroyed here
        tail_ = nullptr;
        size_ = 0;
    }

    // O(1).  new->next = move(head); head = move(new)
    void push_front(T v)
    {
        auto n = std::make_unique<Node>(std::move(v));
        n->next = std::move(head_);
        head_ = std::move(n);
        if (!tail_) tail_ = head_.get();
        ++size_;
    }

    // O(1) via the raw tail pointer. The unique_ptr lives in tail_->next.
    void push_back(T v)
    {
        auto n = std::make_unique<Node>(std::move(v));
        Node *raw = n.get();
        if (tail_) tail_->next = std::move(n);
        else       head_ = std::move(n);
        tail_ = raw;
        ++size_;
    }

    // O(1). Precondition: !empty().
    T pop_front()
    {
        T v = std::move(head_->value);
        head_ = std::move(head_->next); // old head freed automatically
        if (!head_) tail_ = nullptr;
        --size_;
        return v;
    }

    // O(1): insert after the node `pos` refers to; returns iterator to it.
    iterator insert_after(iterator pos, T v)
    {
        auto n = std::make_unique<Node>(std::move(v));
        n->next = std::move(pos.n_->next);   // [N] -> [B]
        pos.n_->next = std::move(n);         // [A] -> [N]
        if (tail_ == pos.n_) tail_ = pos.n_->next.get();
        ++size_;
        return iterator(pos.n_->next.get());
    }

    // O(n): remove all elements equal to v. Walk with a pointer to the owning
    // unique_ptr — the C++ version of the C pointer-to-pointer trick.
    std::size_t remove(const T &v)
    {
        std::size_t removed = 0;
        std::unique_ptr<Node> *link = &head_;
        Node *last = nullptr;
        while (*link) {
            if ((*link)->value == v) {
                *link = std::move((*link)->next); // unlinks AND frees
                ++removed;
            } else {
                last = link->get();
                link = &(*link)->next;
            }
        }
        tail_ = last;
        size_ -= removed;
        return removed;
    }

    // O(n), O(1) space: same prev/cur/next dance as the C version, but the
    // "arrows" are unique_ptrs so each flip is a move.
    void reverse() noexcept
    {
        std::unique_ptr<Node> prev;
        tail_ = head_.get();
        while (head_) {
            std::unique_ptr<Node> next = std::move(head_->next);
            head_->next = std::move(prev);
            prev = std::move(head_);
            head_ = std::move(next);
        }
        head_ = std::move(prev);
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    T &front() { return head_->value; }
    T &back() { return tail_->value; }

    iterator begin() noexcept { return iterator(head_.get()); }
    iterator end() noexcept { return iterator(nullptr); }
    const_iterator begin() const noexcept { return const_iterator(head_.get()); }
    const_iterator end() const noexcept { return const_iterator(nullptr); }
};

template <typename T>
static std::vector<T> to_vec(const SinglyList<T> &l) { return {l.begin(), l.end()}; }

template <typename T>
static void print(const char *label, const SinglyList<T> &l)
{
    std::cout << label;
    for (const auto &v : l) std::cout << '[' << v << "] -> ";
    std::cout << "null\n";
}

int main()
{
    SinglyList<int> l{1, 2, 3};
    l.push_front(0);
    l.push_back(4);
    print("init+push:     ", l);
    CHECK((to_vec(l) == std::vector<int>{0, 1, 2, 3, 4}));

    auto it = std::find(l.begin(), l.end(), 2); // works with <algorithm>
    CHECK(it != l.end());
    l.insert_after(it, 25);
    l.insert_after(std::find(l.begin(), l.end(), 4), 5); // after tail
    CHECK(l.back() == 5);
    print("insert_after:  ", l);

    l.reverse();
    print("reverse:       ", l);
    CHECK((to_vec(l) == std::vector<int>{5, 4, 3, 25, 2, 1, 0}));
    CHECK(l.front() == 5 && l.back() == 0);

    CHECK(l.pop_front() == 5);
    l.push_back(3);
    CHECK(l.remove(3) == 2);
    print("remove(3):     ", l);
    CHECK((to_vec(l) == std::vector<int>{4, 25, 2, 1, 0}) && l.back() == 0);
    CHECK(l.remove(0) == 1 && l.back() == 1); // removing the tail updates tail_
    l.push_back(7);
    CHECK(l.back() == 7 && l.size() == 5);

    // Copy is deep; move is a pointer steal.
    SinglyList<int> copy = l;
    copy.push_front(-1);
    CHECK(copy.size() == l.size() + 1);
    SinglyList<int> moved = std::move(copy);
    CHECK(moved.size() == 6 && copy.empty()); // NOLINT: checking moved-from state

    // Works with non-trivial types: strings are moved in, destroyed by RAII.
    SinglyList<std::string> names{"alsa", "v4l2"};
    names.push_front("gst");
    CHECK(names.front() == "gst" && names.size() == 3);
    print("strings:       ", names);

    // 1,000,000 nodes: an implicit recursive destructor would blow the stack
    // under ASan; our iterative clear() handles it.
    SinglyList<int> big;
    for (int i = 0; i < 1000000; i++) big.push_front(i);
    CHECK(big.size() == 1000000 && big.front() == 999999);

    std::cout << "01_singly_list_template: all checks passed\n";
    return 0;
}
