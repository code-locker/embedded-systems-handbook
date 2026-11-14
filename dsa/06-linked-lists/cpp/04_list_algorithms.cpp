/*
 * 04_list_algorithms.cpp — the interview classics in C++: reverse (iterative
 *                          and recursive), middle, Floyd cycle + start,
 *                          merge two sorted, remove n-th from end, reorder —
 *                          cross-checked against std::list on random input
 *
 * Concept : The algorithms only RELINK nodes, so they are written on plain
 *           non-owning Node* links. Ownership lives elsewhere: a NodePool
 *           (std::deque of nodes) owns every node and frees them all at once
 *           with RAII. That separation is what lets us build cycles safely —
 *           with unique_ptr links a cycle would be impossible (or leak).
 * Build   : make run P=dsa/06-linked-lists/cpp/04_list_algorithms.cpp
 * Read    : ../README.md  (sections "Reverse", "Slow and fast pointers",
 *           "Floyd cycle detection", "Merge two sorted lists")
 */
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <list>
#include <random>
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
struct Node {
    T value;
    Node *next = nullptr; // NON-owning: the pool owns the node
};

// Owns nodes. std::deque never moves existing elements on push_back, so the
// Node* we hand out stay valid (a std::vector would invalidate them on growth).
template <typename T>
class NodePool {
    std::deque<Node<T>> nodes_;

public:
    Node<T> *make(T v) { nodes_.push_back(Node<T>{std::move(v), nullptr}); return &nodes_.back(); }

    Node<T> *build(const std::vector<T> &vals)
    {
        Node<T> dummy{T{}, nullptr};
        Node<T> *tail = &dummy;
        for (const T &v : vals) tail = tail->next = make(v);
        return dummy.next;
    }
};

template <typename T>
std::vector<T> to_vec(const Node<T> *h)
{
    std::vector<T> out;
    for (; h; h = h->next) out.push_back(h->value);
    return out;
}

// O(n) time, O(1) space.   prev <- cur   next -> ...   (flip one arrow per step)
template <typename T>
Node<T> *reverse_iter(Node<T> *head)
{
    Node<T> *prev = nullptr;
    while (head) {
        Node<T> *next = head->next;
        head->next = prev;
        prev = head;
        head = next;
    }
    return prev;
}

// O(n) time, O(n) stack.  After the recursive call, head->next is the TAIL of
// the reversed rest; hang head after it.
template <typename T>
Node<T> *reverse_rec(Node<T> *head)
{
    if (!head || !head->next) return head;
    Node<T> *new_head = reverse_rec(head->next);
    head->next->next = head;
    head->next = nullptr;
    return new_head;
}

// Second middle for even lengths.
template <typename T>
Node<T> *middle(Node<T> *head)
{
    Node<T> *slow = head, *fast = head;
    while (fast && fast->next) { slow = slow->next; fast = fast->next->next; }
    return slow;
}

// Floyd: meet inside the loop, then one pointer from head; they meet at start.
template <typename T>
Node<T> *cycle_start(Node<T> *head)
{
    Node<T> *slow = head, *fast = head;
    while (fast && fast->next) {
        slow = slow->next;
        fast = fast->next->next;
        if (slow == fast) {
            for (slow = head; slow != fast; slow = slow->next, fast = fast->next) {}
            return slow;
        }
    }
    return nullptr;
}

// O(n+m). Pointer-to-pointer variant (no dummy node): `link` is the slot the
// next chosen node must be written into.
template <typename T>
Node<T> *merge_sorted(Node<T> *a, Node<T> *b)
{
    Node<T> *head = nullptr;
    Node<T> **link = &head;
    while (a && b) {
        Node<T> *&smaller = (b->value < a->value) ? b : a; // ref to a or b
        *link = smaller;
        link = &smaller->next;
        smaller = smaller->next; // advances a or b through the reference
    }
    *link = a ? a : b;
    return head;
}

// O(n) one pass: lead is n nodes ahead of `link`'s target.
template <typename T>
Node<T> *remove_nth_from_end(Node<T> *head, std::size_t n)
{
    Node<T> *lead = head;
    for (std::size_t i = 0; i < n; i++) {
        if (!lead) return head; // n > length
        lead = lead->next;
    }
    Node<T> **link = &head; // slot holding the candidate victim
    while (lead) { lead = lead->next; link = &(*link)->next; }
    *link = (*link)->next; // unlink; the pool still owns (and later frees) it
    return head;
}

// Reorder L0 L1 ... Ln  ->  L0 Ln L1 Ln-1 ...  (combines three classics:
// middle, reverse, and an alternating merge). O(n), O(1) space.
//   1 2 3 4 5  -> halves [1 2 3] [4 5] -> reverse 2nd [5 4] -> 1 5 2 4 3
template <typename T>
void reorder(Node<T> *head)
{
    if (!head || !head->next) return;
    Node<T> *slow = head, *fast = head;
    while (fast->next && fast->next->next) { slow = slow->next; fast = fast->next->next; }
    Node<T> *second = reverse_iter(slow->next);
    slow->next = nullptr;
    for (Node<T> *first = head; second;) {
        Node<T> *fn = first->next, *sn = second->next;
        first->next = second;  // 1 -> 5
        second->next = fn;     // 5 -> 2
        first = fn;
        second = sn;
    }
}

int main()
{
    NodePool<int> pool; // frees every node at the end of main, whatever happened

    Node<int> *h = pool.build({1, 2, 3, 4, 5});
    h = reverse_iter(h);
    CHECK((to_vec(h) == std::vector<int>{5, 4, 3, 2, 1}));
    h = reverse_rec(h);
    CHECK((to_vec(h) == std::vector<int>{1, 2, 3, 4, 5}));
    CHECK(middle(h)->value == 3);
    std::cout << "reverse, middle:      ok\n";

    reorder(h);
    CHECK((to_vec(h) == std::vector<int>{1, 5, 2, 4, 3}));
    std::cout << "reorder 1..5:         1 5 2 4 3\n";

    Node<int> *m = merge_sorted(pool.build({1, 3, 5}), pool.build({2, 3, 4, 6}));
    CHECK((to_vec(m) == std::vector<int>{1, 2, 3, 3, 4, 5, 6}));
    m = remove_nth_from_end(m, 1);
    m = remove_nth_from_end(m, 6); // the head
    CHECK((to_vec(m) == std::vector<int>{2, 3, 3, 4, 5}));
    CHECK((to_vec(remove_nth_from_end(m, 99)) == std::vector<int>{2, 3, 3, 4, 5}));
    std::cout << "merge, remove nth:    ok\n";

    // Cycle: safe because the pool, not the links, owns the nodes.
    Node<int> *c = pool.build({0, 1, 2, 3, 4, 5});
    Node<int> *last = c;
    while (last->next) last = last->next;
    CHECK(cycle_start(c) == nullptr);
    last->next = c->next->next; // 5 -> 2
    CHECK(cycle_start(c) != nullptr && cycle_start(c)->value == 2);
    std::cout << "floyd cycle start:    [" << cycle_start(c)->value << "]\n";

    // Randomised cross-check against std::list.
    std::mt19937 rng(42);
    for (int round = 0; round < 200; round++) {
        std::uniform_int_distribution<int> len(0, 30), val(-50, 50);
        std::vector<int> va(static_cast<std::size_t>(len(rng))), vb(static_cast<std::size_t>(len(rng)));
        for (int &x : va) x = val(rng);
        for (int &x : vb) x = val(rng);
        std::list<int> la(va.begin(), va.end()), lb(vb.begin(), vb.end());
        la.sort();
        lb.sort();
        la.merge(lb);
        std::list<int> sa(va.begin(), va.end()), sb(vb.begin(), vb.end());
        sa.sort();
        sb.sort();
        Node<int> *mine = merge_sorted(pool.build({sa.begin(), sa.end()}),
                                       pool.build({sb.begin(), sb.end()}));
        CHECK((to_vec(mine) == std::vector<int>(la.begin(), la.end())));

        std::list<int> rl(va.begin(), va.end());
        rl.reverse();
        CHECK((to_vec(reverse_iter(pool.build(va))) == std::vector<int>(rl.begin(), rl.end())));
    }
    std::cout << "random vs std::list:  200 rounds ok\n";

    std::cout << "04_list_algorithms: all checks passed\n";
    return 0;
}
