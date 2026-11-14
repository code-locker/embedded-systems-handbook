/*
 * 03_huffman_canonical.cpp — Huffman tree with unique_ptr, real bit packing, canonical codes
 *
 * Concept : 1. Build the tree greedily (merge the two lightest) using a heap.
 *           2. Keep only each symbol's code LENGTH, then rebuild "canonical"
 *              codes from the lengths alone — exactly what DEFLATE (gzip/zlib/PNG)
 *              and JPEG do, so the header only has to store lengths, not a tree.
 *           3. Pack the bits into bytes (MSB first) and decode them back with the
 *              count-per-length method used by zlib's reference decoder "puff".
 * Build   : make run P=dsa/16-greedy/cpp/03_huffman_canonical.cpp
 * Read    : ../README.md  (section "Huffman coding")
 *
 * Memory  : the tree is owned by std::unique_ptr; destroying the root frees all.
 *           (Recursion depth on destruction <= number of distinct symbols.)
 */
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

struct Node {
    long freq;
    int sym;    // -1 for internal nodes
    int order;  // tie-break so the tree is deterministic
    std::unique_ptr<Node> left, right;
};

using NodePtr = std::unique_ptr<Node>;

// Heap comparator for a MIN-heap via std::push_heap/pop_heap (they build max-heaps,
// so "a < b" here means "a is heavier").
static bool heavier(const NodePtr& a, const NodePtr& b)
{
    if (a->freq != b->freq) return a->freq > b->freq;
    return a->order > b->order;
}

// We use a vector + heap algorithms instead of std::priority_queue because
// priority_queue::top() is const, so a unique_ptr could not be MOVED out of it.
static NodePtr build_tree(const std::array<long, 256>& freq)
{
    std::vector<NodePtr> heap;
    int order = 0;
    for (int c = 0; c < 256; ++c)
        if (freq[c])
            heap.push_back(std::make_unique<Node>(Node{freq[c], c, order++, nullptr, nullptr}));
    assert(!heap.empty());
    std::make_heap(heap.begin(), heap.end(), heavier);
    auto pop = [&] {
        std::pop_heap(heap.begin(), heap.end(), heavier);  // lightest moves to back
        NodePtr n = std::move(heap.back());
        heap.pop_back();
        return n;
    };
    while (heap.size() > 1) {
        NodePtr a = pop(), b = pop();  // the greedy choice: two lightest
        const long f = a->freq + b->freq;
        heap.push_back(std::make_unique<Node>(Node{f, -1, order++, std::move(a), std::move(b)}));
        std::push_heap(heap.begin(), heap.end(), heavier);
    }
    return pop();
}

static void collect_lengths(const Node* n, int depth, std::array<int, 256>& len)
{
    if (n->sym >= 0) {
        len[n->sym] = depth == 0 ? 1 : depth;  // lone symbol still needs 1 bit
        return;
    }
    collect_lengths(n->left.get(), depth + 1, len);
    collect_lengths(n->right.get(), depth + 1, len);
}

/*
 * Canonical codes from lengths only.
 * Rule: sort symbols by (length, symbol). The first gets all zeros; each next code
 * is previous+1, shifted left whenever the length grows.
 *
 *   lengths: a=1  b=3  c=3  d=3  r=3
 *   a : 0
 *   b : (0+1)<<2 = 100
 *   c : 101
 *   d : 110
 *   r : 111
 *
 * Any decoder that knows the lengths rebuilds the same table: no tree in the file.
 */
struct Canon {
    std::array<int, 256> len{};
    std::array<std::uint32_t, 256> code{};
    std::vector<int> count;     // count[L] = how many symbols have length L
    std::vector<int> symbols;   // symbols sorted by (length, value)
};

static Canon make_canonical(const std::array<int, 256>& len)
{
    Canon c;
    c.len = len;
    const int maxlen = *std::max_element(len.begin(), len.end());
    c.count.assign(static_cast<std::size_t>(maxlen) + 1, 0);
    for (int s = 0; s < 256; ++s)
        if (len[s]) { c.symbols.push_back(s); ++c.count[len[s]]; }
    std::stable_sort(c.symbols.begin(), c.symbols.end(),
                     [&](int a, int b) { return len[a] < len[b]; });
    std::uint32_t code = 0;
    int prev = len[c.symbols[0]];
    for (std::size_t i = 0; i < c.symbols.size(); ++i) {
        const int L = len[c.symbols[i]];
        if (i > 0) code = (code + 1) << (L - prev);
        c.code[c.symbols[i]] = code;
        prev = L;
    }
    return c;
}

// ---------------- bit I/O, MSB-first ----------------
class BitWriter {
public:
    void put(std::uint32_t code, int nbits)
    {
        for (int i = nbits - 1; i >= 0; --i) {
            if (bitpos_ == 0) bytes_.push_back(0);
            if (code >> i & 1u) bytes_.back() = static_cast<std::uint8_t>(bytes_.back() | (0x80u >> bitpos_));
            bitpos_ = (bitpos_ + 1) & 7;
            ++total_;
        }
    }
    const std::vector<std::uint8_t>& bytes() const { return bytes_; }
    std::size_t bits() const { return total_; }

private:
    std::vector<std::uint8_t> bytes_;
    int bitpos_ = 0;
    std::size_t total_ = 0;
};

class BitReader {
public:
    explicit BitReader(const std::vector<std::uint8_t>& b) : b_(b) {}
    int get()
    {
        const int bit = (b_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return bit;
    }

private:
    const std::vector<std::uint8_t>& b_;
    std::size_t pos_ = 0;
};

/*
 * puff-style decode of one symbol: read bits one at a time; at length L the
 * canonical codes of that length are the contiguous range [first, first+count).
 */
static int decode_symbol(const Canon& c, BitReader& in)
{
    int code = 0, first = 0, index = 0;
    for (std::size_t L = 1; L < c.count.size(); ++L) {
        code |= in.get();
        const int cnt = c.count[L];
        if (code - first < cnt) return c.symbols[index + code - first];
        index += cnt;
        first = (first + cnt) << 1;
        code <<= 1;
    }
    assert(false && "invalid code");
    return -1;
}

// O(k^2) reference for the optimal total bit count (no heap, no tree).
static long reference_cost(const std::array<long, 256>& freq)
{
    std::vector<long> w;
    for (long f : freq) if (f) w.push_back(f);
    if (w.size() == 1) return w[0];
    long cost = 0;
    while (w.size() > 1) {
        std::sort(w.begin(), w.end());
        const long m = w[0] + w[1];
        cost += m;
        w.erase(w.begin(), w.begin() + 2);
        w.push_back(m);
    }
    return cost;
}

static void roundtrip(const std::string& text, bool verbose)
{
    std::array<long, 256> freq{};
    for (unsigned char ch : text) ++freq[ch];
    const NodePtr root = build_tree(freq);
    std::array<int, 256> len{};
    collect_lengths(root.get(), 0, len);
    const Canon c = make_canonical(len);

    BitWriter w;
    for (unsigned char ch : text) w.put(c.code[ch], c.len[ch]);
    assert(static_cast<long>(w.bits()) == reference_cost(freq));  // optimal size

    BitReader r(w.bytes());
    std::string back;
    for (std::size_t i = 0; i < text.size(); ++i) back.push_back(static_cast<char>(decode_symbol(c, r)));
    assert(back == text);

    // Kraft equality: a full binary tree uses the whole code space, sum 2^-L == 1.
    if (c.symbols.size() > 1) {
        double kraft = 0;
        for (int s : c.symbols) kraft += 1.0 / static_cast<double>(1u << c.len[s]);
        assert(kraft > 0.999999 && kraft < 1.000001);
    }

    if (verbose) {
        std::cout << "text \"" << text << "\"\n";
        for (int s : c.symbols) {
            std::cout << "  '" << static_cast<char>(s) << "' freq " << freq[s] << " len "
                      << c.len[s] << " code ";
            for (int i = c.len[s] - 1; i >= 0; --i) std::cout << (c.code[s] >> i & 1u);
            std::cout << "\n";
        }
        std::cout << "  " << text.size() << " bytes -> " << w.bits() << " bits = " << w.bytes().size()
                  << " bytes packed:";
        for (auto b : w.bytes()) std::cout << ' ' << std::hex << static_cast<int>(b) << std::dec;
        std::cout << "\n\n";
    }
}

int main()
{
    roundtrip("abracadabra", true);
    roundtrip("zzzz", true);
    roundtrip("this is an example of a huffman tree", true);

    std::mt19937 rng(1613);
    for (int t = 0; t < 300; ++t) {
        std::uniform_int_distribution<int> alpha(1, 60), length(1, 500);
        const int a = alpha(rng);
        std::geometric_distribution<int> skew(0.15);  // skewed, like real text
        std::string s(static_cast<std::size_t>(length(rng)), ' ');
        for (auto& ch : s) ch = static_cast<char>('!' + skew(rng) % a);
        roundtrip(s, false);
    }
    std::cout << "300 random strings: optimal size, Kraft == 1, byte-packed round trip. OK\n";
    return 0;
}
