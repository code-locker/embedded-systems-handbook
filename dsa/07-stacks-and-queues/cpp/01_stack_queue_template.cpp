/*
 * 01_stack_queue_template.cpp — Stack<T> and RingQueue<T> as RAII templates
 *
 * Concept : The C versions, rewritten as generic C++ classes. Stack<T> is an
 *           ADAPTOR over std::vector (exactly how std::stack is built).
 *           RingQueue<T> owns a growable circular buffer through
 *           std::unique_ptr<T[]>, so memory is freed automatically — no
 *           clear()/free() calls to forget. Move semantics let it hold
 *           expensive types (std::string) without copying.
 * Build   : make run P=dsa/07-stacks-and-queues/cpp/01_stack_queue_template.cpp
 * Read    : ../README.md  (section "C vs C++")
 */
#include <cassert>
#include <cstddef>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Stack<T>: push/pop at the back of a vector. All O(1) (push amortised).
// ---------------------------------------------------------------------------
template <typename T>
class Stack {
public:
    void push(const T& v) { data_.push_back(v); }
    void push(T&& v) { data_.push_back(std::move(v)); }  // steal, don't copy

    // Construct the element in place: no temporary at all.
    template <typename... Args>
    T& emplace(Args&&... args) { return data_.emplace_back(std::forward<Args>(args)...); }

    // Why pop() returns void in std::stack: if it returned T by value and
    // the copy threw, the element would already be removed = lost. So the
    // STL splits it into top() + pop(). We offer a safe "try_pop" that
    // moves the value out, which cannot lose data for nothrow-move types.
    T& top()
    {
        if (data_.empty()) throw std::out_of_range("Stack::top on empty");
        return data_.back();
    }
    const T& top() const
    {
        if (data_.empty()) throw std::out_of_range("Stack::top on empty");
        return data_.back();
    }
    void pop()
    {
        if (data_.empty()) throw std::out_of_range("Stack::pop on empty");
        data_.pop_back();
    }
    std::optional<T> try_pop()
    {
        if (data_.empty()) return std::nullopt;
        T v = std::move(data_.back());
        data_.pop_back();
        return v;
    }
    bool empty() const noexcept { return data_.empty(); }
    std::size_t size() const noexcept { return data_.size(); }

private:
    std::vector<T> data_;  // owns the elements; destroyed automatically
};

// ---------------------------------------------------------------------------
// RingQueue<T>: FIFO on a circular buffer, capacity doubles when full.
//
//   buf_: [ e | f | _ | _ | a | b | c | d ]     head_=4, size_=6
//                                ^ front (oldest)
//   logical i lives at (head_ + i) & (cap_ - 1)
//
// Requires T to be default-constructible and movable (new T[n] constructs
// every slot). A production version would use raw storage + placement new
// to avoid that; std::deque does exactly this.
// ---------------------------------------------------------------------------
template <typename T>
class RingQueue {
public:
    explicit RingQueue(std::size_t initial_cap = 4) : cap_(round_up_pow2(initial_cap))
    {
        buf_ = std::make_unique<T[]>(cap_);
    }

    // unique_ptr already makes the class move-only; spelled out for clarity.
    RingQueue(const RingQueue&) = delete;
    RingQueue& operator=(const RingQueue&) = delete;
    RingQueue(RingQueue&&) noexcept = default;
    RingQueue& operator=(RingQueue&&) noexcept = default;
    ~RingQueue() = default;  // unique_ptr<T[]> runs delete[] for us

    template <typename U>
    void push(U&& v)  // amortised O(1); perfect-forwards copy or move
    {
        if (size_ == cap_) grow();
        buf_[slot(size_)] = std::forward<U>(v);
        ++size_;
    }

    std::optional<T> pop()  // O(1)
    {
        if (size_ == 0) return std::nullopt;
        T v = std::move(buf_[head_]);
        head_ = (head_ + 1) & (cap_ - 1);
        --size_;
        return v;
    }

    const T& front() const
    {
        if (size_ == 0) throw std::out_of_range("RingQueue::front on empty");
        return buf_[head_];
    }
    bool empty() const noexcept { return size_ == 0; }
    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return cap_; }

private:
    static std::size_t round_up_pow2(std::size_t n)
    {
        std::size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }
    std::size_t slot(std::size_t i) const noexcept { return (head_ + i) & (cap_ - 1); }

    // O(n): move elements into a new buffer in logical order ("unroll").
    void grow()
    {
        auto nb = std::make_unique<T[]>(cap_ * 2);
        for (std::size_t i = 0; i < size_; ++i) nb[i] = std::move(buf_[slot(i)]);
        buf_ = std::move(nb);  // old buffer is delete[]'d right here
        cap_ *= 2;
        head_ = 0;
    }

    std::unique_ptr<T[]> buf_;
    std::size_t cap_;
    std::size_t head_ = 0;
    std::size_t size_ = 0;
};

// A type that counts copies, to prove moves really happen.
struct Tracked {
    static inline int copies = 0;
    std::string name;
    Tracked() = default;
    explicit Tracked(std::string n) : name(std::move(n)) {}
    Tracked(const Tracked& o) : name(o.name) { ++copies; }
    Tracked& operator=(const Tracked& o) { name = o.name; ++copies; return *this; }
    Tracked(Tracked&&) noexcept = default;
    Tracked& operator=(Tracked&&) noexcept = default;
};

int main()
{
    // ---- Stack ----
    Stack<int> s;
    for (int i = 1; i <= 5; ++i) s.push(i);
    assert(s.size() == 5 && s.top() == 5);
    std::cout << "stack pops:";
    while (auto v = s.try_pop()) std::cout << ' ' << *v;
    std::cout << '\n';
    assert(s.empty());

    bool threw = false;
    try { s.pop(); } catch (const std::out_of_range&) { threw = true; }
    assert(threw);  // underflow is an exception, not UB

    Stack<std::string> words;
    words.emplace(3, 'x');      // constructs "xxx" directly in the vector
    words.push("hello");
    assert(words.top() == "hello");
    words.pop();
    assert(words.top() == "xxx");

    // ---- RingQueue ----
    RingQueue<int> q(4);
    for (int i = 0; i < 3; ++i) q.push(i);
    assert(*q.pop() == 0 && *q.pop() == 1);   // head moves to 2
    for (int i = 3; i < 10; ++i) q.push(i);   // wraps around index 3->0, then grows 4 -> 8 once
    std::cout << "queue capacity after growth: " << q.capacity() << '\n';
    assert(q.capacity() == 8 && q.size() == 8);
    std::cout << "queue pops:";
    for (int expect = 2; expect < 10; ++expect) {
        auto v = q.pop();
        assert(v && *v == expect);  // FIFO survived wrap + grow
        std::cout << ' ' << *v;
    }
    std::cout << '\n';
    assert(!q.pop());

    // ---- move semantics: zero copies when pushing rvalues ----
    RingQueue<Tracked> tq(2);
    for (int i = 0; i < 10; ++i) tq.push(Tracked("job" + std::to_string(i)));
    while (auto t = tq.pop()) (void)t;
    std::cout << "copies made while queueing 10 strings: " << Tracked::copies << '\n';
    assert(Tracked::copies == 0);

    RingQueue<Tracked> moved = std::move(tq);  // O(1): just steals the buffer
    assert(moved.empty());

    std::cout << "01_stack_queue_template: all checks passed\n";
    return 0;
}
