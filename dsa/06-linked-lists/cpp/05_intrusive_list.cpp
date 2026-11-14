/*
 * 05_intrusive_list.cpp — a type-safe C++ intrusive list: the kernel's
 *                         list_head idea, with templates instead of macros
 *
 * Concept : Same design as the kernel (08_kernel_list_head.c): the object
 *           embeds a ListHook; the list never allocates. C++ replaces the
 *           container_of MACRO with a tagged base-class hook: Task derives
 *           from ListHook<RunTag> and ListHook<AllTag>, and static_cast does
 *           the offset arithmetic with full type checking, so iterators give
 *           back Task& directly. The hook unlinks itself in
 *           its destructor (RAII "auto-unlink"), so a destroyed object can
 *           never be left dangling on a list. This is what
 *           boost::intrusive::list does.
 * Build   : make run P=dsa/06-linked-lists/cpp/05_intrusive_list.cpp
 * Read    : ../README.md  (section "Linux kernel list_head and container_of")
 */
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <utility>
#include <string>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__,      \
                         __LINE__, #cond);                                   \
            std::exit(EXIT_FAILURE);                                         \
        }                                                                    \
    } while (0)

// The embedded node: struct list_head with RAII. `Tag` lets one class carry
// several hooks (one per list it can be on) as distinct base classes.
template <typename Tag>
class ListHook {
    ListHook *prev_ = this; // self-linked == "not on any list"
    ListHook *next_ = this;
    template <typename T, typename>
    friend class IntrusiveList;

public:
    ListHook() = default;
    ListHook(const ListHook &) = delete;            // copying an object must not
    ListHook &operator=(const ListHook &) = delete; // copy its list membership
    ~ListHook() { unlink(); }                       // auto-unlink: no dangling

    bool linked() const noexcept { return next_ != this; }

    // O(1).  [p] <=> [this] <=> [n]   ==>   [p] <=> [n],  this self-linked
    void unlink() noexcept
    {
        prev_->next_ = next_;
        next_->prev_ = prev_;
        prev_ = next_ = this;
    }
};

// IntrusiveList<Task, RunTag>: a list of Tasks threaded through their
// ListHook<RunTag> base. Does NOT own the objects — whoever created them does.
template <typename T, typename Tag>
class IntrusiveList {
    using Hook = ListHook<Tag>;
    Hook head_; // the sentinel, exactly like LIST_HEAD(name). Never a T.

    // container_of, type-safely. Hook is a BASE of T, so the compiler knows
    // the offset and static_cast does the pointer arithmetic for us:
    //
    //   Task object:  [ ListHook<AllTag> | ListHook<RunTag> | pid | name ]
    //                 ^ T*               ^ Hook* (RunTag)
    //   static_cast<Task*>(hook) subtracts the base-class offset.
    //
    // Only valid for hooks that really are inside a T — never call it on
    // &head_ (that is why iterators compare against end() first).
    static T *owner(Hook *h) noexcept { return static_cast<T *>(h); }
    static Hook *hook(T &obj) noexcept { return static_cast<Hook *>(&obj); }

    static void link_before(Hook *pos, Hook *n) noexcept
    {
        n->prev_ = pos->prev_;
        n->next_ = pos;
        pos->prev_->next_ = n;
        pos->prev_ = n;
    }

public:
    class iterator {
        Hook *h_;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = T *;
        using reference = T &;
        explicit iterator(Hook *h) : h_(h) {}
        T &operator*() const { return *owner(h_); }
        T *operator->() const { return owner(h_); }
        iterator &operator++() { h_ = h_->next_; return *this; }
        iterator &operator--() { h_ = h_->prev_; return *this; }
        bool operator==(const iterator &o) const { return h_ == o.h_; }
        bool operator!=(const iterator &o) const { return h_ != o.h_; }
    };

    IntrusiveList() = default;
    IntrusiveList(const IntrusiveList &) = delete;
    IntrusiveList &operator=(const IntrusiveList &) = delete;
    // Destroying the list detaches everything (objects survive, unlinked).
    ~IntrusiveList() { clear(); }

    bool empty() const noexcept { return !head_.linked(); }

    // O(1). An object may only be on ONE list per hook: adding it first
    // unlinks it from wherever it was, like the kernel's list_move.
    void push_back(T &obj) noexcept
    {
        Hook *h = hook(obj);
        h->unlink();
        link_before(&head_, h);
    }
    void push_front(T &obj) noexcept
    {
        Hook *h = hook(obj);
        h->unlink();
        link_before(head_.next_, h);
    }
    T &front() { return *owner(head_.next_); } // precondition: !empty()
    void pop_front() noexcept { head_.next_->unlink(); }

    // O(n) — no stored size, like the kernel list (a counter is extra state
    // that every unlink-from-anywhere would have to update).
    std::size_t size() const noexcept
    {
        std::size_t n = 0;
        for (const Hook *h = head_.next_; h != &head_; h = h->next_) ++n;
        return n;
    }

    void clear() noexcept
    {
        while (!empty()) head_.next_->unlink();
    }

    iterator begin() noexcept { return iterator(head_.next_); }
    iterator end() noexcept { return iterator(&head_); }
};

// One object, two lists at once — the reason intrusive lists exist.
struct AllTag {};
struct RunTag {};
struct Task : ListHook<AllTag>, ListHook<RunTag> {
    int pid;
    std::string name;
    Task(int p, std::string n) : pid(p), name(std::move(n)) {}
    ListHook<AllTag> &all_hook() { return *this; }
    ListHook<RunTag> &run_hook() { return *this; }
};

using AllList = IntrusiveList<Task, AllTag>;
using RunList = IntrusiveList<Task, RunTag>;

template <typename L>
static std::vector<int> pids(L &l)
{
    std::vector<int> v;
    for (Task &t : l) v.push_back(t.pid);
    return v;
}

int main()
{
    // Ownership: the vector of unique_ptr owns the Tasks; the lists only
    // thread through them. Locals die in reverse order, so `tasks` is
    // destroyed BEFORE the lists: each Task's hooks auto-unlink as it dies,
    // leaving the lists empty and valid. (Declare them the other way round
    // and the lists' clear() simply detaches the still-living tasks.)
    AllList all;
    RunList run;
    std::vector<std::unique_ptr<Task>> tasks;
    for (int i = 1; i <= 4; i++) {
        tasks.push_back(std::make_unique<Task>(i, "task" + std::to_string(i)));
        all.push_back(*tasks.back());
        run.push_back(*tasks.back());
    }
    CHECK((pids(all) == std::vector<int>{1, 2, 3, 4}));
    CHECK((pids(run) == std::vector<int>{1, 2, 3, 4}));

    // Round-robin: front goes to the back. push_back auto-unlinks first.
    run.push_back(run.front());
    CHECK((pids(run) == std::vector<int>{2, 3, 4, 1}));

    // Task 3 blocks: take it off the run list only.
    tasks[2]->run_hook().unlink();
    CHECK((pids(run) == std::vector<int>{2, 4, 1}) && all.size() == 4);
    CHECK(!tasks[2]->run_hook().linked() && tasks[2]->all_hook().linked());

    // Destroy task 2 while it is on BOTH lists: its hooks' destructors unlink
    // it in O(1) — no dangling pointer, nothing for ASan to complain about.
    tasks.erase(tasks.begin() + 1);
    CHECK((pids(run) == std::vector<int>{4, 1}));
    CHECK((pids(all) == std::vector<int>{1, 3, 4}));

    std::cout << "run list:  ";
    for (Task &t : run) std::cout << t.name << "(" << t.pid << ") ";
    std::cout << "\nall list:  ";
    for (Task &t : all) std::cout << t.name << "(" << t.pid << ") ";
    std::cout << '\n';

    run.pop_front();
    CHECK((pids(run) == std::vector<int>{1}));
    run.clear();
    CHECK(run.empty() && !tasks[0]->run_hook().linked());

    std::cout << "05_intrusive_list: all checks passed\n";
    return 0;
}
