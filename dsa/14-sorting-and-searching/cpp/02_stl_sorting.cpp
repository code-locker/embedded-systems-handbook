/*
 * 02_stl_sorting.cpp — the standard library sorting toolbox and when to use each
 *
 * Concept : std::sort         O(n log n), NOT stable  (introsort)
 *           std::stable_sort  O(n log n) with a buffer (merge sort), stable
 *           std::partial_sort top-k in sorted order, O(n log k)  (heap)
 *           std::nth_element  k-th element + partition, O(n) avg (introselect)
 *           std::list::sort   linked lists can't use std::sort (no random access)
 *           Comparators must be a STRICT WEAK ORDERING: use '<', never '<='.
 *           Multi-key ordering: compare std::tie(...) tuples.
 * Build   : make run P=dsa/14-sorting-and-searching/cpp/02_stl_sorting.cpp
 * Read    : ../README.md  (sections "3.7" and "3.9")
 */
#include <algorithm>
#include <cassert>
#include <functional>
#include <iostream>
#include <list>
#include <numeric>
#include <random>
#include <string>
#include <tuple>
#include <vector>

struct Process {
    std::string name;
    int         nice;     // lower nice = higher priority
    double      cpu;      // % cpu
    int         pid;
};

static void print(const char *title, const std::vector<Process> &v)
{
    std::cout << title << '\n';
    for (const auto &p : v)
        std::cout << "    pid " << p.pid << "  nice " << p.nice << "  cpu " << p.cpu << "  " << p.name << '\n';
}

int main()
{
    std::cout << "=== The STL sorting toolbox ===\n\n";

    std::vector<Process> ps{
        {"gst-launch", 0, 35.0, 101}, {"sshd", 0, 0.1, 7},       {"kworker", -20, 2.5, 3},
        {"python3", 10, 12.0, 220},   {"vision_app", -5, 60.0, 150}, {"bash", 0, 0.1, 99},
        {"irq/42", -20, 0.4, 12},
    };

    // ---- 1. std::sort with a multi-key comparator via std::tie ------------
    // "nice ascending, then cpu DESCENDING, then pid ascending".
    // To flip one key, swap which side it is taken from (b.cpu vs a.cpu).
    auto v = ps;
    std::sort(v.begin(), v.end(), [](const Process &a, const Process &b) {
        return std::tie(a.nice, b.cpu, a.pid) < std::tie(b.nice, a.cpu, b.pid);
    });
    print("1. std::sort by (nice asc, cpu desc, pid asc):", v);
    assert(v[0].name == "kworker" && v[1].name == "irq/42" && v[2].name == "vision_app");

    // ---- 2. std::stable_sort: sort by secondary key FIRST, then primary ----
    v = ps;
    std::stable_sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.name < b.name; });
    std::stable_sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.nice < b.nice; });
    print("\n2. stable_sort by name, then stable_sort by nice -> (nice, name):", v);
    for (size_t i = 1; i < v.size(); ++i)
        assert(v[i - 1].nice < v[i].nice || (v[i - 1].nice == v[i].nice && v[i - 1].name <= v[i].name));

    // ---- 3. partial_sort: top 3 cpu users, only those 3 sorted -------------
    v = ps;
    std::partial_sort(v.begin(), v.begin() + 3, v.end(),
                      [](const auto &a, const auto &b) { return a.cpu > b.cpu; });
    std::cout << "\n3. partial_sort top-3 by cpu: " << v[0].name << ", " << v[1].name << ", " << v[2].name << '\n';
    assert(v[0].name == "vision_app" && v[1].name == "gst-launch" && v[2].name == "python3");

    // ---- 4. nth_element: median cpu without sorting ------------------------
    std::vector<double> cpu;
    for (const auto &p : ps) cpu.push_back(p.cpu);
    auto mid = cpu.begin() + static_cast<long>(cpu.size() / 2);
    std::nth_element(cpu.begin(), mid, cpu.end());
    std::cout << "4. nth_element median cpu = " << *mid << '\n';
    auto sorted_cpu = cpu;
    std::sort(sorted_cpu.begin(), sorted_cpu.end());
    assert(*mid == sorted_cpu[cpu.size() / 2]);
    assert(std::all_of(cpu.begin(), mid, [&](double x) { return x <= *mid; }));

    // ---- 5. sort an index array ("argsort"): leave data where it is --------
    std::vector<size_t> idx(ps.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ps[a].pid < ps[b].pid; });
    std::cout << "5. argsort by pid -> indices:";
    for (size_t i : idx) std::cout << ' ' << i;
    std::cout << "  (ps itself untouched)\n";
    for (size_t i = 1; i < idx.size(); ++i) assert(ps[idx[i - 1]].pid < ps[idx[i]].pid);

    // ---- 6. std::list: must use the member sort (merge sort on nodes) ------
    std::list<int> lst{5, 3, 9, 1, 7};
    // std::sort(lst.begin(), lst.end());   // does not compile: needs random access
    lst.sort();                            // O(n log n), stable, relinks nodes, no copies
    assert(std::is_sorted(lst.begin(), lst.end()));
    std::cout << "6. std::list::sort -> ";
    for (int x : lst) std::cout << x << ' ';
    std::cout << '\n';

    // ---- 7. is_sorted_until, unique after sort (dedupe) --------------------
    std::vector<int> d{4, 1, 4, 2, 1, 4, 3};
    std::sort(d.begin(), d.end());
    d.erase(std::unique(d.begin(), d.end()), d.end());   // the "sort + unique" idiom
    assert((d == std::vector<int>{1, 2, 3, 4}));
    std::cout << "7. sort + unique + erase -> 1 2 3 4\n";

    // ---- 8. random cross-check of the four tools on larger data ------------
    std::mt19937 rng(2024);
    for (int t = 0; t < 100; ++t) {
        std::vector<int> a(1 + rng() % 3000);
        for (int &x : a) x = static_cast<int>(rng() % 500);
        auto ref = a;
        std::stable_sort(ref.begin(), ref.end());
        auto s1 = a; std::sort(s1.begin(), s1.end());
        assert(s1 == ref);
        size_t k = rng() % a.size();
        auto s2 = a; std::partial_sort(s2.begin(), s2.begin() + static_cast<long>(k), s2.end());
        assert(std::equal(s2.begin(), s2.begin() + static_cast<long>(k), ref.begin()));
        auto s3 = a; std::nth_element(s3.begin(), s3.begin() + static_cast<long>(k), s3.end());
        assert(s3[k] == ref[k]);
    }
    std::cout << "8. sort / partial_sort / nth_element agree with stable_sort on 100 vectors\n";

    // ---- 9. strict weak ordering -------------------------------------------
    // A comparator using '<=' says "a before a" is true: cmp(a,a) must be
    // FALSE. With '<=', std::sort may read past the end of the array
    // (its unguarded inner loops rely on cmp(x, pivot) eventually being
    // false). We do NOT run it — it is undefined behaviour — just check the
    // property that catches it:
    auto bad  = [](int a, int b) { return a <= b; };
    auto good = [](int a, int b) { return a < b; };
    assert(bad(5, 5) == true);    // irreflexivity violated -> never use
    assert(good(5, 5) == false);
    std::cout << "9. cmp(x,x) must be false: '<' ok, '<=' is undefined behaviour in std::sort\n";

    std::cout << "\nAll STL sorting checks passed.\n";
    return 0;
}
