/*
 * 07_recursion_to_iteration.cpp — tree traversals: recursive vs std::stack,
 *                                 plus how deep recursion actually breaks
 *
 * Concept : Pre-, in- and post-order are three recursive functions that differ
 *           only in WHERE the "visit" sits relative to the two calls. With an
 *           explicit std::stack you must make that position explicit:
 *             pre-order : pop, visit, push right then left
 *             in-order  : go left pushing, pop, visit, go right
 *             post-order: frame = (node, visited_children?) flag
 *           A degenerate tree (a "linked list" of 200,000 nodes) shows why:
 *           the iterative versions handle any depth; the recursive versions
 *           would need 200,000 stack frames (risky under 8 MB, fatal in a
 *           16 KB kernel stack). We only RUN recursion on the balanced tree.
 *           Even destruction must be iterative: the default unique_ptr chain
 *           destructor is itself recursive!
 * Build   : make run P=dsa/15-recursion-and-backtracking/cpp/07_recursion_to_iteration.cpp
 * Read    : ../README.md  (section "Recursion -> iteration")
 */
#include <cassert>
#include <iostream>
#include <memory>
#include <stack>
#include <utility>
#include <vector>

struct Node {
    int val;
    std::unique_ptr<Node> left, right;           // a Node owns its children
    explicit Node(int v) : val(v) {}
};

// Iterative destruction: detach children into a work list before the node
// dies, so no destructor ever recurses more than one level.
static void destroy(std::unique_ptr<Node> root) {
    std::vector<std::unique_ptr<Node>> work;
    if (root) work.push_back(std::move(root));
    while (!work.empty()) {
        std::unique_ptr<Node> n = std::move(work.back());
        work.pop_back();
        if (n->left)  work.push_back(std::move(n->left));
        if (n->right) work.push_back(std::move(n->right));
        // n goes out of scope here with no children: O(1) destructor
    }
}

// ---- recursive ---------------------------------------------------------------
static void pre_rec(const Node* n, std::vector<int>& out) {
    if (!n) return;
    out.push_back(n->val);           // visit BEFORE children
    pre_rec(n->left.get(), out);
    pre_rec(n->right.get(), out);
}
static void in_rec(const Node* n, std::vector<int>& out) {
    if (!n) return;
    in_rec(n->left.get(), out);
    out.push_back(n->val);           // visit BETWEEN children
    in_rec(n->right.get(), out);
}
static void post_rec(const Node* n, std::vector<int>& out) {
    if (!n) return;
    post_rec(n->left.get(), out);
    post_rec(n->right.get(), out);
    out.push_back(n->val);           // visit AFTER children
}

// ---- iterative -------------------------------------------------------------
static std::vector<int> pre_iter(const Node* root) {
    std::vector<int> out;
    std::stack<const Node*> st;
    if (root) st.push(root);
    while (!st.empty()) {
        const Node* n = st.top(); st.pop();
        out.push_back(n->val);
        if (n->right) st.push(n->right.get());   // pushed first -> popped last
        if (n->left)  st.push(n->left.get());
    }
    return out;
}

/*
 * In-order with a stack, tree:      4
 *                                 /   \
 *                                2     6
 *                               / \
 *                              1   3
 *  cur=4 push 4, cur=2 push 2, cur=1 push 1, cur=null
 *  stack [4 2 1] pop 1 visit 1, cur=1.right=null
 *  stack [4 2]   pop 2 visit 2, cur=3 push 3, cur=null
 *  stack [4 3]   pop 3 visit 3 ...  pop 4 visit 4, cur=6 ... visit 6
 *  output 1 2 3 4 6
 */
static std::vector<int> in_iter(const Node* root) {
    std::vector<int> out;
    std::stack<const Node*> st;
    const Node* cur = root;
    while (cur || !st.empty()) {
        while (cur) { st.push(cur); cur = cur->left.get(); }   // "call" left
        cur = st.top(); st.pop();                              // left returned
        out.push_back(cur->val);
        cur = cur->right.get();                                // "call" right
    }
    return out;
}

// Post-order: a frame needs to remember whether its children were done —
// the explicit version of the return address.
static std::vector<int> post_iter(const Node* root) {
    std::vector<int> out;
    std::stack<std::pair<const Node*, bool>> st;   // (node, children_done)
    if (root) st.push({root, false});
    while (!st.empty()) {
        auto [n, done] = st.top(); st.pop();
        if (done) { out.push_back(n->val); continue; }
        st.push({n, true});                        // come back after children
        if (n->right) st.push({n->right.get(), false});
        if (n->left)  st.push({n->left.get(), false});
    }
    return out;
}

// Build a balanced BST holding lo..hi (recursion depth only log2(n)).
static std::unique_ptr<Node> build_balanced(int lo, int hi) {
    if (lo > hi) return nullptr;
    const int mid = lo + (hi - lo) / 2;
    auto n = std::make_unique<Node>(mid);
    n->left = build_balanced(lo, mid - 1);
    n->right = build_balanced(mid + 1, hi);
    return n;
}

int main() {
    auto t = build_balanced(1, 2047);              // 11 levels
    std::vector<int> a, b, c;
    pre_rec(t.get(), a); in_rec(t.get(), b); post_rec(t.get(), c);
    assert(a == pre_iter(t.get()));
    assert(b == in_iter(t.get()));
    assert(c == post_iter(t.get()));
    for (int i = 0; i < 2047; ++i) assert(b[i] == i + 1);   // BST in-order is sorted
    std::cout << "balanced tree (2047 nodes): recursive == iterative for pre/in/post order\n";
    destroy(std::move(t));

    auto small = build_balanced(1, 7);
    std::cout << "tree 1..7  pre :"; for (int v : pre_iter(small.get()))  std::cout << ' ' << v;
    std::cout << "\n           in  :"; for (int v : in_iter(small.get()))   std::cout << ' ' << v;
    std::cout << "\n           post:"; for (int v : post_iter(small.get())) std::cout << ' ' << v;
    std::cout << '\n';
    destroy(std::move(small));

    // Degenerate tree: every node only has a right child -> depth 200,000.
    constexpr int N = 200000;
    auto root = std::make_unique<Node>(0);
    Node* tail = root.get();
    for (int i = 1; i < N; ++i) { tail->right = std::make_unique<Node>(i); tail = tail->right.get(); }
    const auto in = in_iter(root.get());
    const auto post = post_iter(root.get());
    assert(int(in.size()) == N && in.front() == 0 && in.back() == N - 1);
    assert(int(post.size()) == N && post.front() == N - 1 && post.back() == 0);
    std::cout << "degenerate tree, depth " << N << ": iterative traversals fine\n";
    destroy(std::move(root));    // `root.reset()` would recurse N levels deep in ~Node
    std::cout << "destroyed iteratively (no recursive destructor chain)\n";

    std::cout << "\nall recursion->iteration checks passed\n";
    return 0;
}
