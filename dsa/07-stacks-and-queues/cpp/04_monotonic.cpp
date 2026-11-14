/*
 * 04_monotonic.cpp — monotonic stack and deque problems with the STL
 *
 * Concept : Four classics, all O(n) because each index is pushed and popped
 *           at most once:
 *             1. next greater element          (decreasing stack)
 *             2. daily temperatures ("wait")   (decreasing stack, distances)
 *             3. largest rectangle in histogram(increasing stack)
 *             4. sliding window maximum        (decreasing deque)
 *           Each is checked against a brute-force O(n^2) version on random
 *           data.
 * Build   : make run P=dsa/07-stacks-and-queues/cpp/04_monotonic.cpp
 * Read    : ../README.md  (section "Monotonic stack and queue")
 */
#include <algorithm>
#include <cassert>
#include <deque>
#include <iostream>
#include <random>
#include <vector>

using Vec = std::vector<int>;

// 1. O(n). Stack of indices whose values are strictly decreasing.
Vec next_greater(const Vec& a)
{
    Vec ans(a.size(), -1);
    std::vector<std::size_t> st;  // std::vector used as a stack: back = top
    for (std::size_t i = 0; i < a.size(); ++i) {
        while (!st.empty() && a[st.back()] < a[i]) {
            ans[st.back()] = a[i];  // a[i] is the first bigger value to the right
            st.pop_back();
        }
        st.push_back(i);
    }
    return ans;
}

// 2. O(n). Same stack, but store the DISTANCE to the answer.
Vec days_until_warmer(const Vec& t)
{
    Vec ans(t.size(), 0);
    std::vector<std::size_t> st;
    for (std::size_t i = 0; i < t.size(); ++i) {
        while (!st.empty() && t[st.back()] < t[i]) {
            ans[st.back()] = static_cast<int>(i - st.back());
            st.pop_back();
        }
        st.push_back(i);
    }
    return ans;
}

// 3. O(n). Increasing stack: when a lower bar arrives, every taller bar on
// the stack has found its RIGHT boundary (i) and its LEFT boundary is the
// element under it in the stack. Its rectangle = height * (right-left-1).
//
//   heights 2 1 5 6 2 3          ___
//                            ___|   |
//                           |   |   |      bars 5,6 popped when 2 arrives:
//            ___            |   |   |___       6 * 1 = 6
//           |   |___  ___   |   |   |   |      5 * 2 = 10  <- best
//           |   |   ||   |  |   |   |   |
//
// A sentinel height 0 at the end flushes everything left on the stack.
long largest_rectangle(const Vec& h)
{
    std::vector<std::size_t> st;
    long best = 0;
    for (std::size_t i = 0; i <= h.size(); ++i) {
        int cur = (i == h.size()) ? 0 : h[i];
        while (!st.empty() && h[st.back()] >= cur) {
            long height = h[st.back()];
            st.pop_back();
            long left = st.empty() ? -1 : static_cast<long>(st.back());
            best = std::max(best, height * (static_cast<long>(i) - left - 1));
        }
        st.push_back(i);
    }
    return best;
}

// 4. O(n). std::deque of indices, values decreasing front -> back.
Vec sliding_max(const Vec& a, std::size_t k)
{
    Vec out;
    std::deque<std::size_t> dq;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!dq.empty() && dq.front() + k <= i) dq.pop_front();       // expired
        while (!dq.empty() && a[dq.back()] <= a[i]) dq.pop_back();     // dominated
        dq.push_back(i);
        if (i + 1 >= k) out.push_back(a[dq.front()]);
    }
    return out;
}

// ----- brute-force references, O(n^2) / O(n*k) -----
Vec next_greater_bf(const Vec& a)
{
    Vec ans(a.size(), -1);
    for (std::size_t i = 0; i < a.size(); ++i)
        for (std::size_t j = i + 1; j < a.size(); ++j)
            if (a[j] > a[i]) { ans[i] = a[j]; break; }
    return ans;
}
long largest_rectangle_bf(const Vec& h)
{
    long best = 0;
    for (std::size_t i = 0; i < h.size(); ++i) {
        int mn = h[i];
        for (std::size_t j = i; j < h.size(); ++j) {
            mn = std::min(mn, h[j]);
            best = std::max(best, static_cast<long>(mn) * static_cast<long>(j - i + 1));
        }
    }
    return best;
}
Vec sliding_max_bf(const Vec& a, std::size_t k)
{
    Vec out;
    for (std::size_t i = 0; i + k <= a.size(); ++i)
        out.push_back(*std::max_element(a.begin() + static_cast<long>(i),
                                        a.begin() + static_cast<long>(i + k)));
    return out;
}

void print(const char* label, const Vec& v)
{
    std::cout << label << '[';
    for (std::size_t i = 0; i < v.size(); ++i) std::cout << (i ? " " : "") << v[i];
    std::cout << "]\n";
}

int main()
{
    Vec a{2, 1, 5, 3, 4};
    print("next greater   ", next_greater(a));
    assert((next_greater(a) == Vec{5, 5, -1, 4, -1}));

    Vec temps{73, 74, 75, 71, 69, 72, 76, 73};
    print("days to warmer ", days_until_warmer(temps));
    assert((days_until_warmer(temps) == Vec{1, 1, 4, 2, 1, 1, 0, 0}));

    Vec hist{2, 1, 5, 6, 2, 3};
    std::cout << "largest rectangle in [2 1 5 6 2 3] = " << largest_rectangle(hist) << '\n';
    assert(largest_rectangle(hist) == 10);

    Vec w{1, 3, -1, -3, 5, 3, 6, 7};
    print("sliding max k=3", sliding_max(w, 3));
    assert((sliding_max(w, 3) == Vec{3, 3, 5, 5, 6, 7}));

    std::mt19937 rng(42);  // fixed seed: deterministic test
    std::uniform_int_distribution<int> val(0, 30);
    for (int trial = 0; trial < 100; ++trial) {
        Vec r(static_cast<std::size_t>(1 + trial * 3));
        for (auto& x : r) x = val(rng);
        assert(next_greater(r) == next_greater_bf(r));
        assert(largest_rectangle(r) == largest_rectangle_bf(r));
        std::size_t k = 1 + static_cast<std::size_t>(trial) % r.size();
        assert(sliding_max(r, k) == sliding_max_bf(r, k));
    }
    std::cout << "100 random arrays: monotonic == brute force\n";

    std::cout << "04_monotonic: all checks passed\n";
    return 0;
}
