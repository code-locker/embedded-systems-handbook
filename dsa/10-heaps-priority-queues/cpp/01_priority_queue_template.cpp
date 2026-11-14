/*
 * 01_priority_queue_template.cpp — a hand-written PriorityQueue<T, Compare>
 *                                  with the same semantics as std::priority_queue
 *
 * Concept : The heap is a std::vector plus two loops (sift-up, sift-down). The
 *           comparator decides the order; like the STL, Compare = std::less
 *           gives a MAX-heap ("top is the element that is NOT less than any
 *           other"). Works with move-only types (std::unique_ptr). Then the
 *           same thing with the STL heap algorithms std::make_heap/push_heap/pop_heap.
 * Build   : make run P=dsa/10-heaps-priority-queues/cpp/01_priority_queue_template.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <utility>
#include <vector>

template <class T, class Compare = std::less<T>>
class PriorityQueue {
public:
    PriorityQueue() = default;
    explicit PriorityQueue(Compare cmp) : cmp_(std::move(cmp)) {}

    // O(n): Floyd's build-heap, NOT n pushes. Takes the vector by value so the
    // caller can std::move a big buffer in without copying.
    explicit PriorityQueue(std::vector<T> items, Compare cmp = Compare())
        : a_(std::move(items)), cmp_(std::move(cmp))
    {
        for (std::size_t i = a_.size() / 2; i-- > 0;)
            sift_down(i);
    }

    bool        empty() const { return a_.empty(); }
    std::size_t size()  const { return a_.size(); }
    const T&    top()   const { assert(!a_.empty()); return a_.front(); }  // O(1)

    void push(T v)                       // O(log n) amortised
    {
        a_.push_back(std::move(v));
        sift_up(a_.size() - 1);
    }

    template <class... Args>
    void emplace(Args&&... args)         // O(log n): construct in place
    {
        a_.emplace_back(std::forward<Args>(args)...);
        sift_up(a_.size() - 1);
    }

    // Unlike std::priority_queue::pop (which returns void for exception-safety
    // reasons in C++98), this returns the value by move: handy for unique_ptr.
    T pop()                              // O(log n)
    {
        assert(!a_.empty());
        T top = std::move(a_.front());
        a_.front() = std::move(a_.back());
        a_.pop_back();
        if (!a_.empty()) sift_down(0);
        return top;
    }

    bool valid() const
    {
        for (std::size_t i = 1; i < a_.size(); ++i)
            if (cmp_(a_[(i - 1) / 2], a_[i])) return false;   // parent "less" than child
        return true;
    }

private:
    // "before(x, y)" == x must be ABOVE y. With std::less: x > y, so max on top.
    bool before(std::size_t x, std::size_t y) const { return cmp_(a_[y], a_[x]); }

    void sift_up(std::size_t i)          // O(log n)
    {
        while (i > 0) {
            std::size_t p = (i - 1) / 2;
            if (!before(i, p)) break;
            std::swap(a_[i], a_[p]);     // swap = 3 moves; fine for unique_ptr
            i = p;
        }
    }

    void sift_down(std::size_t i)        // O(log n)
    {
        const std::size_t n = a_.size();
        for (;;) {
            std::size_t l = 2 * i + 1, r = l + 1, m = i;
            if (l < n && before(l, m)) m = l;
            if (r < n && before(r, m)) m = r;
            if (m == i) return;
            std::swap(a_[i], a_[m]);
            i = m;
        }
    }

    std::vector<T> a_;   // owns the elements; RAII frees them
    Compare cmp_{};
};

int main()
{
    // 1) max-heap (default), compared with std::priority_queue
    std::mt19937 rng(1);
    std::uniform_int_distribution<int> val(-1000, 1000);
    for (int round = 0; round < 200; ++round) {
        PriorityQueue<int> mine;
        std::priority_queue<int> ref;
        for (int step = 0; step < 500; ++step) {
            if (ref.empty() || rng() % 3) {
                int v = val(rng);
                mine.push(v);
                ref.push(v);
            } else {
                assert(mine.top() == ref.top());
                assert(mine.pop() == ref.top());
                ref.pop();
            }
            assert(mine.size() == ref.size());
        }
        assert(mine.valid());
    }
    std::cout << "200 random rounds: PriorityQueue<int> == std::priority_queue<int>\n";

    // 2) min-heap: flip the comparator, exactly like the STL
    PriorityQueue<int, std::greater<int>> minq(std::vector<int>{5, 3, 8, 1, 9, 2});
    std::cout << "min-heap built in O(n) from {5,3,8,1,9,2}, pops:";
    std::vector<int> got;
    while (!minq.empty()) got.push_back(minq.pop());
    for (int x : got) std::cout << ' ' << x;
    std::cout << '\n';
    assert(std::is_sorted(got.begin(), got.end()));

    // 3) move-only payload: tasks owned by unique_ptr, ordered by priority
    struct Task { int prio; std::string name; };
    auto by_prio = [](const std::unique_ptr<Task>& a, const std::unique_ptr<Task>& b) {
        return a->prio < b->prio;                        // max prio on top
    };
    PriorityQueue<std::unique_ptr<Task>, decltype(by_prio)> tasks(by_prio);
    tasks.push(std::make_unique<Task>(Task{2, "flush-log"}));
    tasks.push(std::make_unique<Task>(Task{9, "irq-bottom-half"}));
    tasks.emplace(new Task{5, "render-frame"});          // emplace from raw ptr: unique_ptr takes ownership
    std::cout << "task order:";
    int last = 100;
    while (!tasks.empty()) {
        std::unique_ptr<Task> t = tasks.pop();           // ownership moves out to us
        std::cout << ' ' << t->name << '(' << t->prio << ')';
        assert(t->prio <= last);
        last = t->prio;
    }                                                    // each Task freed here: no leaks
    std::cout << '\n';

    // 4) the STL heap algorithms work on any random-access range you own
    std::vector<int> v{4, 1, 7, 3, 9, 2};
    std::make_heap(v.begin(), v.end());                  // O(n)
    assert(std::is_heap(v.begin(), v.end()) && v.front() == 9);
    v.push_back(10);
    std::push_heap(v.begin(), v.end());                  // O(log n): sift-up of the last
    assert(v.front() == 10);
    std::pop_heap(v.begin(), v.end());                   // O(log n): max moved to back
    assert(v.back() == 10);
    v.pop_back();
    std::sort_heap(v.begin(), v.end());                  // O(n log n): this IS heapsort
    assert(std::is_sorted(v.begin(), v.end()));
    std::cout << "make_heap/push_heap/pop_heap/sort_heap checks ok\n";

    std::cout << "all checks passed\n";
    return 0;
}
