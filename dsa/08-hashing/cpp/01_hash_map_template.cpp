/*
 * 01_hash_map_template.cpp — a generic chained HashMap<K, V, Hash, Eq> with
 *                            RAII nodes, rehash, iterators and operator[]
 *
 * Concept : The same separate-chaining design as c/02_chaining_map.c, but
 *           generic over key/value types and hash policy (like
 *           std::unordered_map). Nodes are owned by std::unique_ptr, so there
 *           is no free() anywhere and no leak on any path.
 * Build   : make run P=dsa/08-hashing/cpp/01_hash_map_template.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

template <typename K, typename V, typename Hash = std::hash<K>,
          typename Eq = std::equal_to<K>>
class HashMap {
    struct Node {
        std::pair<const K, V> kv;   // key is const: changing it would
        std::size_t hash;           // put the node in the wrong bucket
        std::unique_ptr<Node> next; // owns the rest of the chain
        Node(K k, V v, std::size_t h) : kv(std::move(k), std::move(v)), hash(h) {}
    };

    /*
     *  buckets_: vector<unique_ptr<Node>>
     *   [0] ─► null
     *   [1] ─► Node{"a"} ─► Node{"q"} ─► null      (each arrow = unique_ptr)
     *   [2] ─► null
     *  Destroying buckets_ destroys every chain recursively.
     *  (Fine for short chains; a pathologically long chain could overflow
     *   the stack in recursive destruction — clear() below is iterative.)
     */
    std::vector<std::unique_ptr<Node>> buckets_;
    std::size_t size_ = 0;
    Hash hasher_;
    Eq eq_;
    static constexpr double kMaxLoad = 1.0; // std::unordered_map default

    std::size_t index(std::size_t h) const { return h & (buckets_.size() - 1); }

    // Pointer to the unique_ptr that owns the node with key k, or to the
    // empty unique_ptr at the end of the chain. The C pointer-to-pointer
    // trick, with unique_ptr instead of raw pointers.
    std::unique_ptr<Node> *slot_of(const K &k, std::size_t h) {
        auto *pp = &buckets_[index(h)];
        while (*pp && !((*pp)->hash == h && eq_((*pp)->kv.first, k)))
            pp = &(*pp)->next;
        return pp;
    }

    // O(n): move nodes into a bigger bucket array. No allocation per node,
    // only unique_ptrs change hands.
    void rehash(std::size_t new_count) {
        std::vector<std::unique_ptr<Node>> nb(new_count);
        for (auto &head : buckets_) {
            while (head) {
                std::unique_ptr<Node> n = std::move(head); // detach first node
                head = std::move(n->next);                 // head -> rest
                auto &dst = nb[n->hash & (new_count - 1)];
                n->next = std::move(dst);                  // push front
                dst = std::move(n);
            }
        }
        buckets_.swap(nb);
    }

public:
    explicit HashMap(std::size_t initial_buckets = 8) {
        std::size_t n = 1;
        while (n < initial_buckets) n <<= 1; // keep a power of two for &
        buckets_.resize(n);
    }
    ~HashMap() { clear(); }
    HashMap(const HashMap &) = delete;            // keep the lesson small:
    HashMap &operator=(const HashMap &) = delete; // move-only
    HashMap(HashMap &&) noexcept = default;
    HashMap &operator=(HashMap &&) noexcept = default;

    std::size_t size() const { return size_; }
    std::size_t bucket_count() const { return buckets_.size(); }
    double load_factor() const { return double(size_) / double(buckets_.size()); }

    // Insert or assign. Average O(1); amortised O(1) including rehash.
    // Returns true if a NEW key was inserted.
    bool insert_or_assign(K k, V v) {
        std::size_t h = hasher_(k);
        auto *pp = slot_of(k, h);
        if (*pp) { (*pp)->kv.second = std::move(v); return false; }
        auto &head = buckets_[index(h)];
        auto n = std::make_unique<Node>(std::move(k), std::move(v), h);
        n->next = std::move(head); // push front: O(1)
        head = std::move(n);
        if (double(++size_) / double(buckets_.size()) > kMaxLoad)
            rehash(buckets_.size() * 2);
        return true;
    }

    // operator[]: default-constructs V if missing (same as std::unordered_map
    // — handy for counting, a trap when you only meant to read).
    V &operator[](const K &k) {
        std::size_t h = hasher_(k);
        if (auto *pp = slot_of(k, h); *pp) return (*pp)->kv.second;
        insert_or_assign(k, V{});
        return *find(k);
    }

    V *find(const K &k) {
        auto *pp = slot_of(k, hasher_(k));
        return *pp ? &(*pp)->kv.second : nullptr;
    }
    bool contains(const K &k) { return find(k) != nullptr; }

    // O(1) average. *pp = std::move(node->next) unlinks AND frees the node
    // in one statement: the old unique_ptr value is destroyed.
    //
    //   pp ─► [A] ─► [B] ─► [C]      erase B
    //   *pp = move(B.next):  [A] ─► [C]   (B destroyed by unique_ptr)
    bool erase(const K &k) {
        auto *pp = slot_of(k, hasher_(k));
        if (!*pp) return false;
        *pp = std::move((*pp)->next);
        --size_;
        return true;
    }

    void clear() { // iterative: no deep recursion in ~unique_ptr chain
        for (auto &head : buckets_)
            while (head) head = std::move(head->next);
        size_ = 0;
    }

    // ---- a forward iterator: walks bucket by bucket, chain by chain -----
    class iterator {
        HashMap *m_;
        std::size_t b_;
        Node *n_;
        void skip_empty() {
            while (!n_ && ++b_ < m_->buckets_.size()) n_ = m_->buckets_[b_].get();
        }
    public:
        iterator(HashMap *m, std::size_t b, Node *n) : m_(m), b_(b), n_(n) { skip_empty(); }
        std::pair<const K, V> &operator*() const { return n_->kv; }
        std::pair<const K, V> *operator->() const { return &n_->kv; }
        iterator &operator++() { n_ = n_->next.get(); skip_empty(); return *this; }
        bool operator!=(const iterator &o) const { return n_ != o.n_; }
    };
    iterator begin() { return iterator(this, 0, buckets_[0].get()); }
    iterator end() { return iterator(this, buckets_.size(), nullptr); }
};

// A deliberately terrible hasher to show that correctness never depends on
// hash quality — only speed does. Every key lands in bucket 0.
struct ConstantHash {
    std::size_t operator()(int) const { return 42; }
};

int main() {
    HashMap<std::string, int> m(4);
    assert(m.insert_or_assign("eth0", 1500));
    assert(m.insert_or_assign("wlan0", 2304));
    assert(!m.insert_or_assign("eth0", 9000)); // assign, not insert
    assert(*m.find("eth0") == 9000);
    assert(m.find("can0") == nullptr);

    // word counting with operator[]
    HashMap<std::string, int> freq;
    for (const char *w : {"ip", "link", "ip", "route", "ip", "link"}) ++freq[w];
    assert(freq["ip"] == 3 && freq["link"] == 2 && freq["route"] == 1);
    assert(freq.size() == 3);

    // growth: load factor never exceeds 1.0
    HashMap<int, int> big;
    for (int i = 0; i < 50000; ++i) big.insert_or_assign(i, i * 3);
    assert(big.size() == 50000 && big.load_factor() <= 1.0);
    for (int i = 0; i < 50000; i += 2) assert(big.erase(i));
    assert(!big.erase(0));
    for (int i = 1; i < 50000; i += 2) assert(*big.find(i) == i * 3);

    // iterate and cross-check against std::unordered_map
    std::unordered_map<int, int> ref;
    for (int i = 1; i < 50000; i += 2) ref[i] = i * 3;
    std::size_t seen = 0;
    for (auto &kv : big) {
        assert(ref.at(kv.first) == kv.second);
        ++seen;
    }
    assert(seen == ref.size());
    std::cout << "HashMap<int,int>: " << big.size() << " keys, "
              << big.bucket_count() << " buckets, load " << big.load_factor() << "\n";

    // terrible hash: still correct (just O(n) per op)
    HashMap<int, int, ConstantHash> bad;
    for (int i = 0; i < 500; ++i) bad.insert_or_assign(i, -i);
    for (int i = 0; i < 500; ++i) assert(*bad.find(i) == -i);
    std::cout << "constant hasher: still correct, every lookup walks one chain\n";

    std::cout << "all HashMap checks passed\n";
    return 0;
}
