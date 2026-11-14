/*
 * 01_traversals_iterators.cpp — traversals in modern C++: unique_ptr nodes,
 *                               visitors, std::stack / std::queue, and an
 *                               in-order ITERATOR so range-for works on a tree
 *
 * Concept : Same traversals as c/02, but ownership is automatic (each node
 *           owns its children through std::unique_ptr, so deleting the root
 *           frees everything) and traversal is packaged as (a) a template
 *           visitor and (b) an iterator that keeps the explicit stack inside
 *           itself. The iterator is how std::map lets you write
 *           `for (auto& kv : m)`. All versions are cross-checked on random trees.
 * Build   : make run P=dsa/09-trees/cpp/01_traversals_iterators.cpp
 * Read    : ../README.md  (section "Traversals")
 */
#include <cassert>
#include <cstdio>
#include <iterator>
#include <memory>
#include <queue>
#include <random>
#include <stack>
#include <string>
#include <vector>

template <class T>
struct Node {
    T value;
    std::unique_ptr<Node> left, right;   // owning: Node destroys its children
    explicit Node(T v) : value(std::move(v)) {}
};

template <class T>
using Tree = std::unique_ptr<Node<T>>;

// ---- recursive visitors: O(n) time, O(h) call-stack ------------------------
// F is any callable; templates let the compiler inline it (no std::function
// overhead).
template <class T, class F> void preorder(const Node<T>* n, F&& f)
{
    if (!n) return;
    f(n->value);
    preorder(n->left.get(), f);
    preorder(n->right.get(), f);
}
template <class T, class F> void inorder(const Node<T>* n, F&& f)
{
    if (!n) return;
    inorder(n->left.get(), f);
    f(n->value);
    inorder(n->right.get(), f);
}
template <class T, class F> void postorder(const Node<T>* n, F&& f)
{
    if (!n) return;
    postorder(n->left.get(), f);
    postorder(n->right.get(), f);
    f(n->value);
}

// ---- iterative with std::stack: O(n) time, O(h) heap memory ---------------
template <class T, class F> void preorder_iter(const Node<T>* root, F&& f)
{
    std::stack<const Node<T>*> st;
    if (root) st.push(root);
    while (!st.empty()) {
        const Node<T>* n = st.top();
        st.pop();
        f(n->value);
        if (n->right) st.push(n->right.get());   // right first -> popped last
        if (n->left) st.push(n->left.get());
    }
}

// Post-order with TWO stacks: s1 produces root-right-left (a mirrored
// pre-order); popping everything off s2 reverses it into left-right-root.
template <class T, class F> void postorder_two_stacks(const Node<T>* root, F&& f)
{
    std::stack<const Node<T>*> s1, s2;
    if (root) s1.push(root);
    while (!s1.empty()) {
        const Node<T>* n = s1.top();
        s1.pop();
        s2.push(n);
        if (n->left) s1.push(n->left.get());
        if (n->right) s1.push(n->right.get());
    }
    for (; !s2.empty(); s2.pop()) f(s2.top()->value);
}

// Level order, returning one vector per level (O(n) time, O(width) queue).
template <class T> std::vector<std::vector<T>> levels(const Node<T>* root)
{
    std::vector<std::vector<T>> out;
    std::queue<const Node<T>*> q;
    if (root) q.push(root);
    while (!q.empty()) {
        out.emplace_back();
        for (size_t width = q.size(); width > 0; --width) {   // one level
            const Node<T>* n = q.front();
            q.pop();
            out.back().push_back(n->value);
            if (n->left) q.push(n->left.get());
            if (n->right) q.push(n->right.get());
        }
    }
    return out;
}

// ---- the in-order iterator ------------------------------------------------
// It is the iterative in-order algorithm cut into pieces: the constructor
// does the first "dive left", operator++ does "pop, step right, dive left".
// Each node is pushed and popped once -> a full walk is O(n), so ++ is
// amortised O(1); memory O(h).
template <class T>
class InorderIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T*;
    using reference = const T&;

    InorderIterator() = default;                         // == end()
    explicit InorderIterator(const Node<T>* root) { dive_left(root); }

    reference operator*() const { return stack_.back()->value; }
    pointer operator->() const { return &stack_.back()->value; }
    InorderIterator& operator++()
    {
        const Node<T>* n = stack_.back();
        stack_.pop_back();
        dive_left(n->right.get());
        return *this;
    }
    InorderIterator operator++(int) { auto tmp = *this; ++*this; return tmp; }
    // Two iterators are equal when they point at the same node (or both end).
    bool operator==(const InorderIterator& o) const
    {
        if (stack_.empty() || o.stack_.empty()) return stack_.empty() == o.stack_.empty();
        return stack_.back() == o.stack_.back();
    }
    bool operator!=(const InorderIterator& o) const { return !(*this == o); }

private:
    void dive_left(const Node<T>* n)
    {
        for (; n; n = n->left.get()) stack_.push_back(n);
    }
    std::vector<const Node<T>*> stack_;
};

// A tiny range wrapper so we can write: for (auto& v : InorderRange{root}) ...
template <class T>
struct InorderRange {
    const Node<T>* root;
    InorderIterator<T> begin() const { return InorderIterator<T>(root); }
    InorderIterator<T> end() const { return {}; }
};
template <class T> InorderRange(const Node<T>*) -> InorderRange<T>;   // C++17 CTAD

// ---- destruction ----------------------------------------------------------
// The default ~unique_ptr chain is recursive: depth = tree height. A
// degenerate tree of a few hundred thousand nodes overflows the stack.
// This iterative version rotates left children up until the root has no
// left child, then drops the root. O(n) time, O(1) extra space.
template <class T> void destroy_iteratively(Tree<T>& root)
{
    while (root) {
        if (root->left) {
            Tree<T> l = std::move(root->left);   //      r          l
            root->left = std::move(l->right);    //     /    ->      \  .
            l->right = std::move(root);          //    l              r
            root = std::move(l);
        } else {
            root = std::move(root->right);       // old root freed here
        }
    }
}

// Random-shaped tree with values 0..n-1 (random walk down, insert at NULL).
static Tree<int> random_tree(int n, std::mt19937& rng)
{
    Tree<int> root;
    for (int k = 0; k < n; ++k) {
        Tree<int>* link = &root;
        while (*link) link = (rng() & 1) ? &(*link)->left : &(*link)->right;
        *link = std::make_unique<Node<int>>(k);
    }
    return root;
}

int main()
{
    //            F
    //          /   \           .
    //         B     G
    //        / \     \         .
    //       A   D     I
    //          / \   /
    //         C   E H
    auto t = std::make_unique<Node<char>>('F');
    t->left = std::make_unique<Node<char>>('B');
    t->left->left = std::make_unique<Node<char>>('A');
    t->left->right = std::make_unique<Node<char>>('D');
    t->left->right->left = std::make_unique<Node<char>>('C');
    t->left->right->right = std::make_unique<Node<char>>('E');
    t->right = std::make_unique<Node<char>>('G');
    t->right->right = std::make_unique<Node<char>>('I');
    t->right->right->left = std::make_unique<Node<char>>('H');

    std::string s;
    auto add = [&s](char c) { s += c; };
    preorder(t.get(), add);            assert(s == "FBADCEGIH"); std::printf("pre   %s\n", s.c_str());
    s.clear(); preorder_iter(t.get(), add);       assert(s == "FBADCEGIH");
    s.clear(); inorder(t.get(), add);             assert(s == "ABCDEFGHI"); std::printf("in    %s\n", s.c_str());
    s.clear(); postorder(t.get(), add);           assert(s == "ACEDBHIGF"); std::printf("post  %s\n", s.c_str());
    s.clear(); postorder_two_stacks(t.get(), add); assert(s == "ACEDBHIGF");

    auto lv = levels(t.get());
    std::printf("levels:");
    for (auto& level : lv) {
        std::printf(" [");
        for (char c : level) std::printf("%c", c);
        std::printf("]");
    }
    std::printf("\n");
    assert(lv.size() == 4 && lv[3] == (std::vector<char>{'C', 'E', 'H'}));

    s.clear();
    for (char c : InorderRange{t.get()}) s += c;          // range-for on a tree!
    assert(s == "ABCDEFGHI");
    std::printf("range-for over InorderRange: %s\n", s.c_str());
    // t goes out of scope here: unique_ptr frees all 9 nodes (RAII).

    // ---- randomised cross-check ----------------------------------------------
    std::mt19937 rng(77);
    for (int iter = 0; iter < 1000; ++iter) {
        int n = static_cast<int>(rng() % 300);
        Tree<int> r = random_tree(n, rng);
        std::vector<int> a, b;
        auto pa = [&a](int v) { a.push_back(v); };
        auto pb = [&b](int v) { b.push_back(v); };
        preorder(r.get(), pa); preorder_iter(r.get(), pb);
        assert(a == b && static_cast<int>(a.size()) == n);
        a.clear(); b.clear();
        postorder(r.get(), pa); postorder_two_stacks(r.get(), pb);
        assert(a == b);
        a.clear(); b.clear();
        inorder(r.get(), pa);
        for (int v : InorderRange{r.get()}) b.push_back(v);
        assert(a == b);
        size_t total = 0;
        for (auto& level : levels(r.get())) total += level.size();
        assert(total == static_cast<size_t>(n));
        destroy_iteratively(r);
        assert(!r);
    }

    // ---- a 300000-node chain: recursive destruction could overflow ----------
    Tree<int> chain;
    Tree<int>* link = &chain;
    for (int i = 0; i < 300000; ++i) {
        *link = std::make_unique<Node<int>>(i);
        link = &(*link)->left;
    }
    long sum = 0;
    for (int v : InorderRange{chain.get()}) sum += v;     // iterator: no recursion
    assert(sum == 300000L * 299999L / 2);
    destroy_iteratively(chain);                           // no recursion either
    std::printf("300000-node chain walked and destroyed without recursion\n");

    std::printf("all C++ traversal checks passed\n");
    return 0;
}
