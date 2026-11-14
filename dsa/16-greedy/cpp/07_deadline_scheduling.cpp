/*
 * 07_deadline_scheduling.cpp — Earliest Deadline First and job sequencing with deadlines
 *
 * Concept : Two scheduling greedies an embedded engineer meets in RTOS/kernel work.
 *             1. Minimise the MAXIMUM lateness of jobs on one CPU: run them in
 *                order of deadline (EDF). Proof is an exchange argument on
 *                "inversions". Linux SCHED_DEADLINE is EDF at its core.
 *             2. Unit-time jobs with deadlines and profits, maximise profit:
 *                take jobs by profit, put each in the LATEST free slot before
 *                its deadline.
 *           Both checked against trying every permutation / subset.
 * Build   : make run P=dsa/16-greedy/cpp/07_deadline_scheduling.cpp
 * Read    : ../README.md  (section "Exchange argument")
 */
#include <algorithm>
#include <cassert>
#include <iostream>
#include <numeric>
#include <random>
#include <vector>

struct Task {
    int duration;
    int deadline;
};

// Lateness of a schedule = max over jobs of (finish - deadline), clipped at 0.
static int max_lateness(const std::vector<Task>& ts, const std::vector<std::size_t>& order)
{
    int t = 0, worst = 0;
    for (std::size_t i : order) {
        t += ts[i].duration;
        worst = std::max(worst, t - ts[i].deadline);
    }
    return worst;
}

/*
 * EDF: sort by deadline. O(n log n).
 *
 * Exchange argument in one picture: if job B (deadline 5) runs right before
 * job A (deadline 3) — an "inversion" — swap them:
 *
 *   before: |--B--|--A--|        A finishes at T, lateness T-3
 *   after : |--A--|--B--|        B finishes at T, lateness T-5 <= T-3
 *                                A finishes earlier, so it only improved
 * The max lateness never increases, and each swap removes one inversion, so
 * the sorted (inversion-free) order is at least as good as ANY schedule.
 */
static std::vector<std::size_t> edf(const std::vector<Task>& ts)
{
    std::vector<std::size_t> order(ts.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return ts[a].deadline < ts[b].deadline; });
    return order;
}

static int brute_min_lateness(const std::vector<Task>& ts)
{
    std::vector<std::size_t> p(ts.size());
    std::iota(p.begin(), p.end(), 0);
    int best = max_lateness(ts, p);
    while (std::next_permutation(p.begin(), p.end())) best = std::min(best, max_lateness(ts, p));
    return best;
}

// ---------------- job sequencing with deadlines (unit time) ----------------
struct Job {
    int deadline;  // must finish by the end of slot `deadline` (slots 1..deadline)
    int profit;
};

/*
 *   jobs (deadline, profit): a(2,100) b(1,19) c(2,27) d(1,25) e(3,15)
 *   by profit: a c d b e
 *   a -> latest free slot <= 2: slot 2      [ _ a _ ]
 *   c -> latest free slot <= 2: slot 1      [ c a _ ]
 *   d -> slots <= 1 all full: skip
 *   b -> skip
 *   e -> slot 3                             [ c a e ]   profit 142
 * Putting a job as LATE as possible keeps early slots free for tight deadlines.
 * O(n * maxDeadline) here; a DSU "next free slot" makes it ~O(n log n).
 */
static int job_sequencing(std::vector<Job> jobs)
{
    std::sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.profit > b.profit; });
    int maxd = 0;
    for (const auto& j : jobs) maxd = std::max(maxd, j.deadline);
    std::vector<bool> used(static_cast<std::size_t>(maxd) + 1, false);
    int total = 0;
    for (const auto& j : jobs) {
        for (int s = j.deadline; s >= 1; --s) {
            if (!used[static_cast<std::size_t>(s)]) { used[static_cast<std::size_t>(s)] = true; total += j.profit; break; }
        }
    }
    return total;
}

// A set of unit jobs is schedulable iff, sorted by deadline, the k-th job has deadline >= k.
static int brute_job_sequencing(const std::vector<Job>& jobs)
{
    const std::size_t n = jobs.size();
    int best = 0;
    for (unsigned mask = 0; mask < (1u << n); ++mask) {
        std::vector<int> d;
        int p = 0;
        for (std::size_t i = 0; i < n; ++i)
            if (mask >> i & 1u) { d.push_back(jobs[i].deadline); p += jobs[i].profit; }
        std::sort(d.begin(), d.end());
        bool ok = true;
        for (std::size_t k = 0; k < d.size(); ++k) ok = ok && d[k] >= static_cast<int>(k) + 1;
        if (ok) best = std::max(best, p);
    }
    return best;
}

int main()
{
    const std::vector<Task> ts{{3, 6}, {2, 8}, {1, 9}, {4, 9}, {3, 14}, {2, 15}};
    const auto order = edf(ts);
    std::cout << "EDF order:";
    for (std::size_t i : order) std::cout << " (" << ts[i].duration << "," << ts[i].deadline << ")";
    std::cout << "  max lateness = " << max_lateness(ts, order) << "\n";
    assert(max_lateness(ts, order) == brute_min_lateness(ts));

    const std::vector<Job> jobs{{2, 100}, {1, 19}, {2, 27}, {1, 25}, {3, 15}};
    std::cout << "job sequencing profit = " << job_sequencing(jobs) << "\n";
    assert(job_sequencing(jobs) == 142);

    std::mt19937 rng(1617);
    std::uniform_int_distribution<int> n(1, 7), dur(1, 6), dl(1, 20), jd(1, 5), pr(1, 50);
    for (int t = 0; t < 500; ++t) {
        std::vector<Task> v(static_cast<std::size_t>(n(rng)));
        for (auto& x : v) { x.duration = dur(rng); x.deadline = dl(rng); }
        assert(max_lateness(v, edf(v)) == brute_min_lateness(v));

        std::vector<Job> j(static_cast<std::size_t>(n(rng)) + 3);
        for (auto& x : j) { x.deadline = jd(rng); x.profit = pr(rng); }
        assert(job_sequencing(j) == brute_job_sequencing(j));
    }
    std::cout << "500 random instances: EDF == best permutation, job sequencing == best subset. OK\n";
    return 0;
}
