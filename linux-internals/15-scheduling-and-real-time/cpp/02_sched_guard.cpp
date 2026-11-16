/*
 * 02_sched_guard.cpp — RAII guards for scheduling state: policy+priority,
 *                      CPU affinity and timer slack, restored at scope exit;
 *                      std::optional "try" factories for the EPERM case
 *
 * Concept : "Raise to SCHED_FIFO for this section, then restore" is a
 *           resource like a lock: forget the restore on one error path and
 *           a thread stays RT forever (and may starve the system). A guard
 *           object saves the old state in its constructor and restores it in
 *           its destructor. Two lessons hide in the details:
 *             - acquiring may fail with EPERM (no CAP_SYS_NICE): a factory
 *               returning std::optional makes the caller handle it;
 *             - RESTORING may also need privilege (lowering nice, leaving
 *               SCHED_IDLE), and a destructor must not throw: it records
 *               the failure instead. Only guard changes you can undo.
 * Build   : make run P=linux-internals/15-scheduling-and-real-time/cpp/02_sched_guard.cpp
 * Read    : ../README.md  (section "5. C vs C++")
 */
#include <pthread.h>
#include <sched.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <system_error>
#include <thread>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fflush(stdout);                                             \
            std::fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond,       \
                         __FILE__, __LINE__);                                \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

static const char *policy_name(int p)
{
    switch (p) {
    case SCHED_OTHER: return "SCHED_OTHER";
    case SCHED_BATCH: return "SCHED_BATCH";
    case SCHED_IDLE:  return "SCHED_IDLE";
    case SCHED_FIFO:  return "SCHED_FIFO";
    case SCHED_RR:    return "SCHED_RR";
    default:          return "?";
    }
}

static int current_policy(int *prio = nullptr)
{
    int pol;
    sched_param sp{};
    CHECK(pthread_getschedparam(pthread_self(), &pol, &sp) == 0);
    if (prio)
        *prio = sp.sched_priority;
    return pol;
}

// ---- policy + priority ---------------------------------------------------
class ScopedSchedPolicy {
public:
    // Throwing constructor: for code where failure is a bug.
    ScopedSchedPolicy(int policy, int prio)
    {
        if (int e = apply(policy, prio))
            throw std::system_error(e, std::generic_category(), "pthread_setschedparam");
    }
    // Non-throwing factory: for code that can run without RT (EPERM on a
    // dev PC, OK on the board). Guaranteed copy elision builds it in place.
    static std::optional<ScopedSchedPolicy> try_apply(int policy, int prio, int *err)
    {
        ScopedSchedPolicy g(Tag{});
        *err = g.apply(policy, prio);
        if (*err)
            return std::nullopt;
        return std::optional<ScopedSchedPolicy>(std::move(g));
    }

    ScopedSchedPolicy(ScopedSchedPolicy &&o) noexcept
        : old_pol_(o.old_pol_), old_prio_(o.old_prio_), armed_(o.armed_)
    {
        o.armed_ = false; // the moved-from guard must not restore twice
    }
    ScopedSchedPolicy(const ScopedSchedPolicy &) = delete;
    ScopedSchedPolicy &operator=(const ScopedSchedPolicy &) = delete;
    ScopedSchedPolicy &operator=(ScopedSchedPolicy &&) = delete;

    ~ScopedSchedPolicy()
    {
        if (!armed_)
            return;
        sched_param sp{};
        sp.sched_priority = old_prio_;
        // Never throw from a destructor; remember the failure instead.
        restore_error_ = pthread_setschedparam(pthread_self(), old_pol_, &sp);
    }
    static int last_restore_error() { return restore_error_; }

private:
    struct Tag {};
    explicit ScopedSchedPolicy(Tag) {}

    int apply(int policy, int prio)
    {
        sched_param sp{};
        old_pol_ = current_policy(&old_prio_);
        sp.sched_priority = prio;
        int e = pthread_setschedparam(pthread_self(), policy, &sp);
        armed_ = (e == 0);
        return e;
    }

    int old_pol_ = SCHED_OTHER;
    int old_prio_ = 0;
    bool armed_ = false;
    static inline thread_local int restore_error_ = 0;
};

// ---- CPU affinity: always restorable, no privilege needed -----------------
class ScopedAffinity {
public:
    explicit ScopedAffinity(int cpu)
    {
        CHECK(pthread_getaffinity_np(pthread_self(), sizeof old_, &old_) == 0);
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(cpu, &one);
        if (int e = pthread_setaffinity_np(pthread_self(), sizeof one, &one))
            throw std::system_error(e, std::generic_category(), "pthread_setaffinity_np");
    }
    ~ScopedAffinity() { pthread_setaffinity_np(pthread_self(), sizeof old_, &old_); }
    ScopedAffinity(const ScopedAffinity &) = delete;
    ScopedAffinity &operator=(const ScopedAffinity &) = delete;

private:
    cpu_set_t old_;
};

// ---- timer slack (lesson c/08): per thread, restorable ---------------------
class ScopedTimerSlack {
public:
    explicit ScopedTimerSlack(unsigned long ns) : old_(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0))
    {
        CHECK(prctl(PR_SET_TIMERSLACK, ns, 0, 0, 0) == 0);
    }
    ~ScopedTimerSlack() { prctl(PR_SET_TIMERSLACK, static_cast<unsigned long>(old_), 0, 0, 0); }
    ScopedTimerSlack(const ScopedTimerSlack &) = delete;
    ScopedTimerSlack &operator=(const ScopedTimerSlack &) = delete;

private:
    long old_;
};

static void demo()
{
    // Each demo runs in its own thread: scheduling state is per thread, and
    // a failed restore must not leak into main.
    std::printf("1) ScopedSchedPolicy(SCHED_BATCH, 0):\n");
    CHECK(current_policy() == SCHED_OTHER);
    {
        ScopedSchedPolicy g(SCHED_BATCH, 0); // lowering importance: allowed
        std::printf("   inside : %s\n", policy_name(current_policy()));
        CHECK(current_policy() == SCHED_BATCH);
    }
    std::printf("   after  : %s (restore error: %s)\n", policy_name(current_policy()),
                std::strerror(ScopedSchedPolicy::last_restore_error()));
    // BATCH -> OTHER stays in the fair class at the same nice: always allowed.
    CHECK(ScopedSchedPolicy::last_restore_error() == 0 && current_policy() == SCHED_OTHER);

    std::printf("2) try_apply(SCHED_FIFO, 50):\n");
    int err = 0;
    if (auto g = ScopedSchedPolicy::try_apply(SCHED_FIFO, 50, &err)) {
        std::printf("   inside : %s (privileged)\n", policy_name(current_policy()));
        CHECK(current_policy() == SCHED_FIFO);
    } else {
        std::printf("   not applied: %s -> continuing as %s (grant rtprio or CAP_SYS_NICE)\n",
                    std::strerror(err), policy_name(current_policy()));
        CHECK(err == EPERM && current_policy() == SCHED_OTHER);
    }
    CHECK(current_policy() == SCHED_OTHER); // restored or never changed

    std::printf("3) throwing constructor with SCHED_RR 50:\n");
    try {
        ScopedSchedPolicy g(SCHED_RR, 50);
        std::printf("   inside : %s (privileged)\n", policy_name(current_policy()));
    } catch (const std::system_error &e) {
        std::printf("   caught std::system_error: %s\n", e.what());
        CHECK(e.code().value() == EPERM);
    }
    CHECK(current_policy() == SCHED_OTHER);

    std::printf("4) ScopedAffinity + ScopedTimerSlack:\n");
    cpu_set_t before;
    CHECK(pthread_getaffinity_np(pthread_self(), sizeof before, &before) == 0);
    int cpu = -1;
    for (int c = 0; c < CPU_SETSIZE && cpu < 0; ++c)
        if (CPU_ISSET(c, &before))
            cpu = c;
    long slack_before = prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0);
    {
        ScopedAffinity a(cpu);
        ScopedTimerSlack s(1);
        std::printf("   inside : on CPU %d, timer slack %d ns\n", sched_getcpu(),
                    prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0));
        CHECK(sched_getcpu() == cpu);
        CHECK(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) == 1);
    }
    cpu_set_t after;
    CHECK(pthread_getaffinity_np(pthread_self(), sizeof after, &after) == 0);
    CHECK(CPU_EQUAL(&before, &after));
    CHECK(prctl(PR_GET_TIMERSLACK, 0, 0, 0, 0) == slack_before);
    std::printf("   after  : %d CPUs allowed again, slack %ld ns\n", CPU_COUNT(&after), slack_before);

    std::printf("5) what you should NOT guard unprivileged: SCHED_IDLE\n");
    {
        ScopedSchedPolicy g(SCHED_IDLE, 0); // going down: allowed
        CHECK(current_policy() == SCHED_IDLE);
    } // going back up needs RLIMIT_NICE / CAP_SYS_NICE: may fail
    int re = ScopedSchedPolicy::last_restore_error();
    std::printf("   leaving SCHED_IDLE in the destructor: %s -> now %s\n",
                re ? std::strerror(re) : "ok", policy_name(current_policy()));
    CHECK(re == 0 || re == EPERM);
}

int main()
{
    alarm(10);
    std::thread t(demo); // a throwaway thread: step 5 may leave it SCHED_IDLE
    t.join();
    CHECK(current_policy() == SCHED_OTHER); // main untouched
    std::printf("02_sched_guard: all checks passed\n");
    return 0;
}
