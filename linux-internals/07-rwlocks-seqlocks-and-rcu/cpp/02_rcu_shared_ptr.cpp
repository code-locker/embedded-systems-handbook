/*
 * 02_rcu_shared_ptr.cpp — RCU-style publishing in C++17 with
 *                         std::shared_ptr<const T> and the std::atomic_load /
 *                         atomic_store / atomic_compare_exchange overloads
 *
 * Concept : The hard part of RCU is knowing when the old version can be
 *           freed. Reference counting solves it differently from a grace
 *           period: every reader holds a shared_ptr to the snapshot it is
 *           using, and the old version is destroyed automatically when the
 *           LAST reader drops it. The price: readers do an atomic
 *           increment/decrement on a shared counter (and libstdc++ guards
 *           the pointer load with a small lock pool), so it does not scale
 *           like kernel RCU. Still the simplest correct "read-copy-update"
 *           in C++. (C++20: std::atomic<std::shared_ptr<T>>.)
 * Build   : make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/02_rcu_shared_ptr.cpp
 *           make run P=linux-internals/07-rwlocks-seqlocks-and-rcu/cpp/02_rcu_shared_ptr.cpp SAN=tsan
 * Read    : ../README.md  (section "RCU", "C vs C++")
 */
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

static std::atomic<long> g_alive{0}; // RoutingTable objects currently alive

// Immutable once published: readers get shared_ptr<const RoutingTable>.
struct RoutingTable {
    unsigned long version = 0;
    std::array<int, 8> next_hop{};
    long checksum = 0;

    RoutingTable() { ++g_alive; }
    RoutingTable(const RoutingTable &o)
        : version(o.version), next_hop(o.next_hop), checksum(o.checksum) { ++g_alive; }
    ~RoutingTable() { --g_alive; }

    long compute_sum() const
    {
        long s = long(version) * 7;
        for (int h : next_hop)
            s = s * 31 + h;
        return s;
    }
};

class RcuTable {
public:
    RcuTable() : cur_(std::make_shared<const RoutingTable>()) {}

    // rcu_read_lock + rcu_dereference in one: the returned shared_ptr KEEPS
    // that version alive as long as the reader holds it.
    std::shared_ptr<const RoutingTable> read() const { return std::atomic_load(&cur_); }

    // Read-copy-update with a CAS loop, so several writers need no extra
    // mutex: if another writer published first, redo the copy from the
    // newer version (like a lock-free stack push, lesson 02).
    template <class F>
    void update(F modify)
    {
        auto old = std::atomic_load(&cur_);
        for (;;) {
            auto copy = std::make_shared<RoutingTable>(*old); // COPY
            modify(*copy);                                    // UPDATE
            copy->version = old->version + 1;
            copy->checksum = copy->compute_sum();
            std::shared_ptr<const RoutingTable> desired = std::move(copy);
            // PUBLISH: succeeds only if cur_ is still `old`; otherwise
            // `old` is refreshed with the current value and we retry.
            if (std::atomic_compare_exchange_strong(&cur_, &old, desired))
                return;
            ++retries_;
        }
        // `old` goes out of scope: if no reader holds it, it is freed
        // right here. That is the "reclamation" step, done by refcount.
    }

    long retries() const { return retries_.load(); }

private:
    std::shared_ptr<const RoutingTable> cur_; // only touched via std::atomic_*
    std::atomic<long> retries_{0};
};

int main()
{
    RcuTable table;
    CHECK(g_alive.load() == 1);

    // 1. A reader's snapshot survives an update.
    auto snap = table.read();
    table.update([](RoutingTable &t) { t.next_hop[0] = 42; });
    CHECK(snap->version == 0 && snap->next_hop[0] == 0); // old view, still valid
    CHECK(table.read()->next_hop[0] == 42);
    CHECK(g_alive.load() == 2); // the old version lives: `snap` holds it
    snap.reset();
    CHECK(g_alive.load() == 1); // last reader gone -> old version freed
    std::printf("snapshot kept the old version alive until the reader dropped it\n");

    // 2. 3 readers + 2 writers.
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0}, bad{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; r++)
        readers.emplace_back([&] {
            unsigned long last = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                auto t = table.read(); // lock-free-ish read, no reader lock
                if (t->checksum != t->compute_sum() || t->version < last)
                    ++bad;
                last = t->version;
                ++reads;
            }
        });
    const int per_writer = 1500;
    std::vector<std::thread> writers;
    for (int w = 0; w < 2; w++)
        writers.emplace_back([&, w] {
            for (int i = 0; i < per_writer; i++)
                table.update([&](RoutingTable &t) { t.next_hop[(i + w) % 8] += 1; });
        });
    for (auto &t : writers)
        t.join();
    stop = true;
    for (auto &t : readers)
        t.join();

    auto final_t = table.read();
    int total_hops = 0;
    for (int h : final_t->next_hop)
        total_hops += h;
    std::printf("2 writers x %d updates: final version %lu, CAS retries %ld "
                "(timing-dependent)\n", per_writer, final_t->version, table.retries());
    std::printf("3 readers: %ld reads, %ld inconsistent; objects alive now: %ld\n",
                reads.load(), bad.load(), g_alive.load());
    CHECK(bad.load() == 0);
    CHECK(final_t->version == 1 + 2UL * per_writer); // +1 from part 1
    CHECK(total_hops == 42 + 2 * per_writer);        // no update was lost
    CHECK(g_alive.load() == 1); // every replaced version was reclaimed

    std::printf("02_rcu_shared_ptr: all checks passed\n");
    return 0;
}
