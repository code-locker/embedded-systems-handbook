/*
 * 02_stl_adaptors.cpp — std::stack, std::queue, std::deque, std::priority_queue
 *
 * Concept : std::stack / std::queue / std::priority_queue are ADAPTORS: thin
 *           wrappers that restrict a real container (std::deque by default,
 *           std::vector for priority_queue) to a narrow interface. Knowing
 *           which container sits underneath tells you the cost of every
 *           call. std::deque itself is a "map of fixed-size blocks", which
 *           is why push_front never moves existing elements.
 * Build   : make run P=dsa/07-stacks-and-queues/cpp/02_stl_adaptors.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <deque>
#include <functional>
#include <iostream>
#include <list>
#include <queue>
#include <stack>
#include <string>
#include <vector>

int main()
{
    // ---------------------------------------------------------------------
    // std::stack<T, Container = std::deque<T>>
    //   push -> c.push_back, top -> c.back, pop -> c.pop_back   all O(1)
    // Choosing std::vector underneath is usually faster (contiguous) but
    // push may reallocate (amortised O(1)).
    // ---------------------------------------------------------------------
    std::stack<int, std::vector<int>> st;
    for (int i : {1, 2, 3}) st.push(i);
    assert(st.top() == 3);
    st.pop();  // returns void: read top() first, then pop()
    assert(st.top() == 2 && st.size() == 2);
    std::cout << "stack top after one pop: " << st.top() << '\n';

    // ---------------------------------------------------------------------
    // std::queue<T, Container = std::deque<T>>
    //   push -> push_back, front -> front, pop -> pop_front    all O(1)
    // std::vector CANNOT be used underneath: it has no pop_front.
    // std::list can (O(1) everything, but a malloc per element).
    // ---------------------------------------------------------------------
    std::queue<std::string> q;
    q.push("frame0");
    q.emplace("frame1");  // construct in place
    q.push("frame2");
    assert(q.front() == "frame0" && q.back() == "frame2");
    q.pop();
    assert(q.front() == "frame1");
    std::queue<int, std::list<int>> lq;  // alternative container
    lq.push(7);
    assert(lq.front() == 7);
    std::cout << "queue front after one pop: " << q.front() << '\n';

    // ---------------------------------------------------------------------
    // std::deque<T>
    //   push/pop front+back O(1), operator[] O(1) (two-level lookup),
    //   insert in the middle O(n).
    //
    //   map (array of block pointers)
    //   +----+----+----+----+
    //   | .  | .  | .  |    |
    //   +-|--+-|--+-|--+----+
    //     v    v    v
    //   [_ _ a b][c d e f][g h _ _]    fixed-size blocks
    //
    // Growing never moves elements -> references to elements stay valid
    // on push_front/push_back (iterators do not!). Not contiguous: you
    // cannot pass &d[0] to a C function expecting an array.
    // ---------------------------------------------------------------------
    std::deque<int> d;
    for (int i = 0; i < 5; ++i) {
        d.push_back(i);
        d.push_front(-i - 1);
    }
    int& ref = d[5];  // element 0
    for (int i = 0; i < 1000; ++i) d.push_back(i);  // many block allocations
    assert(ref == 0);  // still valid: deque does not relocate on push_back
    assert(d.front() == -5 && d[9] == 4);
    std::cout << "deque front " << d.front() << ", size " << d.size() << '\n';

    // ---------------------------------------------------------------------
    // std::priority_queue<T, Container = vector<T>, Compare = less<T>>
    //   a binary heap inside a vector (see lesson 10).
    //   push O(log n), pop O(log n), top O(1).
    //   DEFAULT IS A MAX-HEAP: top() is the LARGEST. With std::less!
    //   (The comparator says "a has lower priority than b".)
    // ---------------------------------------------------------------------
    std::priority_queue<int> maxq;
    for (int x : {5, 1, 8, 3, 9, 2}) maxq.push(x);
    std::cout << "max-heap order:";
    std::vector<int> got;
    while (!maxq.empty()) {
        got.push_back(maxq.top());
        std::cout << ' ' << maxq.top();
        maxq.pop();
    }
    std::cout << '\n';
    assert((got == std::vector<int>{9, 8, 5, 3, 2, 1}));

    // Min-heap: flip the comparator with std::greater.
    std::priority_queue<int, std::vector<int>, std::greater<int>> minq;
    for (int x : {5, 1, 8}) minq.push(x);
    assert(minq.top() == 1);

    // Custom type: a scheduler that runs the most urgent task first and,
    // on ties, the one that arrived first (FIFO among equals — a heap is
    // NOT stable, so we encode arrival order in the key).
    struct Task {
        int priority;  // higher = more urgent
        int seq;       // arrival order
        std::string name;
    };
    struct LowerPriority {
        bool operator()(const Task& a, const Task& b) const
        {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.seq > b.seq;  // later arrival = lower priority
        }
    };
    std::priority_queue<Task, std::vector<Task>, LowerPriority> sched;
    int seq = 0;
    sched.push({1, seq++, "log flush"});
    sched.push({5, seq++, "irq bottom half"});
    sched.push({3, seq++, "video frame"});
    sched.push({5, seq++, "audio period"});
    std::cout << "scheduler order:";
    std::vector<std::string> order;
    while (!sched.empty()) {
        order.push_back(sched.top().name);
        std::cout << " [" << sched.top().name << ']';
        sched.pop();
    }
    std::cout << '\n';
    assert((order == std::vector<std::string>{"irq bottom half", "audio period",
                                              "video frame", "log flush"}));

    std::cout << "02_stl_adaptors: all checks passed\n";
    return 0;
}
