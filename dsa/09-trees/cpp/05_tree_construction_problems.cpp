/*
 * 05_tree_construction_problems.cpp — rebuild trees and solve classic problems
 *
 * Concept : (1) Pre-order + in-order (distinct keys) determine a tree uniquely:
 *               pre-order's first key is the root, its position in in-order
 *               splits left from right. O(n) with a hash map of positions.
 *           (2) Serialise with null markers through a stringstream.
 *           (3) Sorted array -> height-balanced BST (pick the middle).
 *           (4) Maximum path sum and right-side view: "ask children, combine".
 *           Everything is cross-checked on random trees.
 * Build   : make run P=dsa/09-trees/cpp/05_tree_construction_problems.cpp
 * Read    : ../README.md  (sections "Serialise / deserialise" and "Height, diameter, LCA")
 */
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <memory>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Node {
    int key;
    std::unique_ptr<Node> left, right;
    explicit Node(int k) : key(k) {}
};
using Tree = std::unique_ptr<Node>;

static bool same(const Node* a, const Node* b)                 // O(n)
{
    if (!a || !b) return a == b;
    return a->key == b->key && same(a->left.get(), b->left.get()) &&
           same(a->right.get(), b->right.get());
}

static void pre(const Node* n, std::vector<int>& out)
{
    if (!n) return;
    out.push_back(n->key);
    pre(n->left.get(), out);
    pre(n->right.get(), out);
}
static void in(const Node* n, std::vector<int>& out)
{
    if (!n) return;
    in(n->left.get(), out);
    out.push_back(n->key);
    in(n->right.get(), out);
}

// ---- (1) build from pre-order + in-order ------------------------------------
//
//   pre = [3, 9, 20, 15, 7]     root = 3 (first in pre-order)
//   in  = [9, 3, 15, 20, 7]     3 sits at index 1 in in-order:
//          ^  ^  ^^^^^^^^^        left subtree in-order  = [9]         (1 key)
//        left root  right         right subtree in-order = [15, 20, 7] (3 keys)
//   pre-order then lists: root, the 1 left key, the 3 right keys.
//
//        3
//       / \       Recurse on both halves. Each key is placed once and its
//      9   20     in-order position is an O(1) hash lookup -> O(n) total.
//         /  \    (Searching in-order linearly each time would be O(n^2).)
//        15   7
//
// Note pre + post-order is NOT enough in general: a single child could be
// left or right. In-order is what disambiguates.
class Builder {
public:
    Builder(const std::vector<int>& p, const std::vector<int>& i) : pre_(p)
    {
        for (int k = 0; k < static_cast<int>(i.size()); ++k) pos_[i[k]] = k;
    }
    Tree build() { return build(0, static_cast<int>(pre_.size()) - 1); }

private:
    Tree build(int lo, int hi)        // in-order index range [lo, hi]
    {
        if (lo > hi) return nullptr;
        auto t = std::make_unique<Node>(pre_[next_++]);   // next root
        int mid = pos_.at(t->key);
        t->left = build(lo, mid - 1);  // left first: pre-order is root-L-R
        t->right = build(mid + 1, hi);
        return t;
    }
    const std::vector<int>& pre_;
    std::unordered_map<int, int> pos_;
    size_t next_ = 0;
};

// ---- (2) serialise / deserialise with streams --------------------------------
static void ser(const Node* n, std::ostream& os)
{
    if (!n) { os << "# "; return; }
    os << n->key << ' ';
    ser(n->left.get(), os);
    ser(n->right.get(), os);
}
static Tree deser(std::istream& is)
{
    std::string tok;
    if (!(is >> tok) || tok == "#") return nullptr;
    auto t = std::make_unique<Node>(std::stoi(tok));
    t->left = deser(is);
    t->right = deser(is);
    return t;
}

// ---- (3) sorted array -> balanced BST, O(n) ---------------------------------
//   [1 2 3 4 5 6 7]  ->  middle 4 is the root, recurse on [1 2 3] and [5 6 7]
//          4
//        /   \        .
//       2     6       height = floor(log2 n): the best possible
//      / \   / \      .
//     1   3 5   7
static Tree from_sorted(const std::vector<int>& a, int lo, int hi)
{
    if (lo > hi) return nullptr;
    int mid = lo + (hi - lo) / 2;      // not (lo+hi)/2: avoids int overflow
    auto t = std::make_unique<Node>(a[static_cast<size_t>(mid)]);
    t->left = from_sorted(a, lo, mid - 1);
    t->right = from_sorted(a, mid + 1, hi);
    return t;
}

static int height(const Node* n)
{
    return n ? 1 + std::max(height(n->left.get()), height(n->right.get())) : -1;
}

// ---- (4a) maximum path sum (any node to any node), O(n) ----------------------
// Each node returns "best downward path starting here" to its parent
// (it may only use ONE child, else the path would fork), but it records
// "best path bending here" = key + max(0, L) + max(0, R) in `best`.
static long gain(const Node* n, long& best)
{
    if (!n) return 0;
    long l = std::max(0L, gain(n->left.get(), best));   // drop negative branches
    long r = std::max(0L, gain(n->right.get(), best));
    best = std::max(best, n->key + l + r);
    return n->key + std::max(l, r);
}
static long max_path_sum(const Node* root)
{
    long best = LONG_MIN;
    gain(root, best);
    return best;
}

// Brute force for the check: collect all nodes with parent links, then for
// every pair (u, v) sum the path through their LCA. O(n^2 * h).
struct Flat { std::vector<int> key, parent, depth; };
static void flatten(const Node* n, int parent, int depth, Flat& f)
{
    if (!n) return;
    int id = static_cast<int>(f.key.size());
    f.key.push_back(n->key);
    f.parent.push_back(parent);
    f.depth.push_back(depth);
    flatten(n->left.get(), id, depth + 1, f);
    flatten(n->right.get(), id, depth + 1, f);
}
static long max_path_brute(const Node* root)
{
    Flat f;
    flatten(root, -1, 0, f);
    long best = LONG_MIN;
    int n = static_cast<int>(f.key.size());
    for (int u = 0; u < n; ++u)
        for (int v = u; v < n; ++v) {
            int a = u, b = v;
            long sum = 0;
            while (a != b) {                      // climb the deeper one
                if (f.depth[static_cast<size_t>(a)] >= f.depth[static_cast<size_t>(b)]) {
                    sum += f.key[static_cast<size_t>(a)];
                    a = f.parent[static_cast<size_t>(a)];
                } else {
                    sum += f.key[static_cast<size_t>(b)];
                    b = f.parent[static_cast<size_t>(b)];
                }
            }
            sum += f.key[static_cast<size_t>(a)];  // the LCA itself
            best = std::max(best, sum);
        }
    return best;
}

// ---- (4b) right side view: last node of each level (BFS), O(n) ---------------
static std::vector<int> right_view(const Node* root)
{
    std::vector<int> out;
    std::queue<const Node*> q;
    if (root) q.push(root);
    while (!q.empty()) {
        size_t w = q.size();
        for (size_t i = 0; i < w; ++i) {
            const Node* n = q.front();
            q.pop();
            if (i + 1 == w) out.push_back(n->key);
            if (n->left) q.push(n->left.get());
            if (n->right) q.push(n->right.get());
        }
    }
    return out;
}

// Random shape; keys are a random permutation (distinct) offset by `shift`
// so negative keys appear too.
static Tree random_tree(int n, std::mt19937& rng, int shift)
{
    std::vector<int> keys(static_cast<size_t>(n));
    std::iota(keys.begin(), keys.end(), shift);
    std::shuffle(keys.begin(), keys.end(), rng);
    Tree root;
    for (int k : keys) {
        Tree* link = &root;
        while (*link) link = (rng() & 1) ? &(*link)->left : &(*link)->right;
        *link = std::make_unique<Node>(k);
    }
    return root;
}

int main()
{
    // (1) the worked example
    std::vector<int> p{3, 9, 20, 15, 7}, i{9, 3, 15, 20, 7};
    Tree t = Builder(p, i).build();
    assert(t->key == 3 && t->left->key == 9 && t->right->key == 20 &&
           t->right->left->key == 15 && t->right->right->key == 7);
    std::ostringstream os;
    ser(t.get(), os);
    std::printf("rebuilt from pre+in, serialised: %s\n", os.str().c_str());
    assert(os.str() == "3 9 # # 20 15 # # 7 # # ");
    assert((right_view(t.get()) == std::vector<int>{3, 20, 7}));
    assert(max_path_sum(t.get()) == 9 + 3 + 20 + 15);   // 9-3-20-15: a path cannot fork at 20

    // A path that must skip negative parts:
    //      -10
    //      /  \        best = 15 + 20 + 7 = 42 (bends at 20, never touches -10)
    //     9    20
    //         /  \     .
    //        15   7
    t->key = -10;
    assert(max_path_sum(t.get()) == 42);

    // (3) sorted -> balanced
    std::vector<int> sorted(1000);
    std::iota(sorted.begin(), sorted.end(), 0);
    Tree bal = from_sorted(sorted, 0, 999);
    std::vector<int> back;
    in(bal.get(), back);
    assert(back == sorted && height(bal.get()) == 9);   // floor(log2 1000) = 9
    std::printf("1000 sorted keys -> balanced BST of height %d\n", height(bal.get()));

    // randomised
    std::mt19937 rng(8);
    for (int iter = 0; iter < 300; ++iter) {
        int n = 1 + static_cast<int>(rng() % 120);
        Tree r = random_tree(n, rng, -n / 2);
        std::vector<int> pr, ino;
        pre(r.get(), pr);
        in(r.get(), ino);
        Tree rebuilt = Builder(pr, ino).build();
        assert(same(r.get(), rebuilt.get()));

        std::stringstream ss;
        ser(r.get(), ss);
        Tree r2 = deser(ss);
        assert(same(r.get(), r2.get()));

        assert(max_path_sum(r.get()) == max_path_brute(r.get()));
        assert(right_view(r.get()).size() == static_cast<size_t>(height(r.get()) + 1));
    }
    std::printf("all construction / problem checks passed\n");
    return 0;
}
