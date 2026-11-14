/*
 * 04_merge_k_and_topk.cpp — merge k sorted linked lists, and top-k frequent words
 *
 * Concept : (1) Merge k sorted lists (unique_ptr-owned nodes) by keeping each
 *           list's head in a min-heap: O(N log k). Nodes are RELINKED, not
 *           copied, which is how log merging / LSM-tree compaction works.
 *           (2) Top-k frequent: count with unordered_map, then a size-k
 *           min-heap keyed on count: O(n + m log k) for m distinct words.
 * Build   : make run P=dsa/10-heaps-priority-queues/cpp/04_merge_k_and_topk.cpp
 * Read    : ../README.md  (section "Applications")
 */
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Node {
    int val;
    std::unique_ptr<Node> next;   // each node owns the rest of the list
};
using List = std::unique_ptr<Node>;

static List from_vector(const std::vector<int>& v)        // O(n)
{
    List head;
    for (auto it = v.rbegin(); it != v.rend(); ++it)
        head = std::make_unique<Node>(Node{*it, std::move(head)});
    return head;
}

static std::vector<int> to_vector(const List& l)
{
    std::vector<int> out;
    for (const Node* p = l.get(); p; p = p->next.get()) out.push_back(p->val);
    return out;
}

// Destroy iteratively: the default recursive unique_ptr chain destructor
// would use O(n) stack and can overflow on a million-node list.
static void destroy(List& l)
{
    while (l) l = std::move(l->next);
}

/*
 * merge_k — O(N log k) time, O(k) extra space.
 *
 *   heap holds OWNING pointers to the current heads:
 *     A: 1 -> 4 -> 7      heap {A1, B2, C0}
 *     B: 2 -> 5           pop C0, append to output, push C's next (9)
 *     C: 0 -> 9           heap {A1, B2, C9} ...
 *
 *   output tail: we keep a raw Node* to the last node (non-owning) so each
 *   append is O(1); the OUTPUT list owns every node.
 */
static List merge_k(std::vector<List> lists)
{
    // Why not std::priority_queue<List>? Its top() is const&, so you cannot
    // move a unique_ptr out of it without a const_cast. The heap ALGORITHMS on
    // a vector we own give us the same O(log n) operations AND a mutable back():
    // pop_heap moves the best element to the back, then we move it out.
    auto greater_head = [](const List& a, const List& b) { return a->val > b->val; };
    std::vector<List> heap;
    heap.reserve(lists.size());
    for (List& l : lists)
        if (l) heap.push_back(std::move(l));
    std::make_heap(heap.begin(), heap.end(), greater_head);          // O(k)

    List out;
    Node* tail = nullptr;                          // non-owning: out owns the nodes
    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), greater_head);       // O(log k)
        List head = std::move(heap.back());
        heap.pop_back();
        List rest = std::move(head->next);         // detach the remainder
        if (rest) {
            heap.push_back(std::move(rest));
            std::push_heap(heap.begin(), heap.end(), greater_head);  // O(log k)
        }
        Node* raw = head.get();
        if (tail) tail->next = std::move(head);
        else      out = std::move(head);
        tail = raw;
    }
    return out;
}

/* top_k_frequent — O(n + m log k). Ties broken alphabetically for a stable answer. */
static std::vector<std::string> top_k_frequent(const std::vector<std::string>& words, std::size_t k)
{
    std::unordered_map<std::string, int> count;          // O(n) average
    for (const auto& w : words) ++count[w];

    using Entry = std::pair<int, std::string>;           // (count, word)
    // "worse" entry on top of a min-heap: lower count, or same count and larger word
    auto better = [](const Entry& a, const Entry& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    };
    std::priority_queue<Entry, std::vector<Entry>, decltype(better)> heap(better);
    for (auto& [w, c] : count) {
        heap.emplace(c, w);
        if (heap.size() > k) heap.pop();                 // evict the worst: heap stays size k
    }
    std::vector<std::string> out;
    while (!heap.empty()) { out.push_back(heap.top().second); heap.pop(); }
    std::reverse(out.begin(), out.end());                // best first
    return out;
}

int main()
{
    std::vector<List> ls;
    ls.push_back(from_vector({1, 4, 7}));
    ls.push_back(from_vector({2, 5}));
    ls.push_back(from_vector({0, 9}));
    ls.push_back(nullptr);                               // empty list is fine
    List m = merge_k(std::move(ls));
    std::cout << "merged:";
    for (int x : to_vector(m)) std::cout << ' ' << x;
    std::cout << '\n';
    assert((to_vector(m) == std::vector<int>{0, 1, 2, 4, 5, 7, 9}));
    destroy(m);

    std::mt19937 rng(5);
    for (int round = 0; round < 100; ++round) {
        std::size_t k = rng() % 30 + 1;
        std::vector<List> lists;
        std::vector<int> all;
        for (std::size_t i = 0; i < k; ++i) {
            std::vector<int> v(rng() % 50);
            for (int& x : v) x = static_cast<int>(rng() % 1000);
            std::sort(v.begin(), v.end());
            all.insert(all.end(), v.begin(), v.end());
            lists.push_back(from_vector(v));
        }
        std::sort(all.begin(), all.end());
        List r = merge_k(std::move(lists));
        assert(to_vector(r) == all);
        destroy(r);
    }
    std::cout << "100 random k-way list merges == std::sort of everything\n";

    // Big list: proves the iterative destroy (recursive destruction of
    // 200k nodes could blow the stack, especially under ASan).
    {
        std::vector<int> big(200000);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<int>(i);
        std::vector<List> two;
        two.push_back(from_vector(big));
        two.push_back(from_vector({-1}));
        List r = merge_k(std::move(two));
        assert(r->val == -1);
        destroy(r);
    }

    // top-k frequent words
    std::vector<std::string> words{"v4l2", "gst", "drm", "gst", "v4l2", "gst", "dma", "drm", "v4l2", "v4l2"};
    auto top2 = top_k_frequent(words, 2);
    std::cout << "top-2 frequent: " << top2[0] << ", " << top2[1] << '\n';
    assert((top2 == std::vector<std::string>{"v4l2", "gst"}));

    // randomised check against "sort all (count desc, word asc), take k"
    for (int round = 0; round < 100; ++round) {
        std::vector<std::string> w(rng() % 300);
        for (auto& s : w) s = std::string(1, static_cast<char>('a' + rng() % 20));
        std::size_t k = rng() % 25;
        std::map<std::string, int> c;
        for (auto& s : w) ++c[s];
        std::vector<std::pair<int, std::string>> all;
        for (auto& [s, n] : c) all.emplace_back(-n, s);
        std::sort(all.begin(), all.end());
        std::vector<std::string> expect;
        for (std::size_t i = 0; i < std::min(k, all.size()); ++i) expect.push_back(all[i].second);
        assert(top_k_frequent(w, k) == expect);
    }
    std::cout << "100 random top-k-frequent runs match the sort reference\n";
    std::cout << "all checks passed\n";
    return 0;
}
