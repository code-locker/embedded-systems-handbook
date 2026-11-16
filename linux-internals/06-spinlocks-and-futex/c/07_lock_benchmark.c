/*
 * 07_lock_benchmark.c — spin vs sleep, measured: pthread mutex (normal and
 *                       adaptive), pthread spinlock, TTAS+backoff, ticket
 *                       lock and the futex mutex from 06, under contention
 *
 * Concept : There is no "fastest lock", only a fastest lock FOR A SITUATION.
 *           Three situations are timed:
 *             A. short critical section, one thread per core (spin's home)
 *             B. longer critical section (~a few us): spinners waste CPU
 *             C. all threads pinned to ONE core: spinning while the owner
 *                is descheduled; the case that kills user-space spinlocks
 *           Only correctness (the counter) is asserted. Timings are printed:
 *           they depend on the machine, the load, and the sanitizer. For
 *           meaningful numbers build without sanitizers and with -O2:
 *               make run P=<this file> SAN=0 OPT=1
 * Build   : make run P=linux-internals/06-spinlocks-and-futex/c/07_lock_benchmark.c
 *           make run P=linux-internals/06-spinlocks-and-futex/c/07_lock_benchmark.c SAN=tsan
 * Read    : ../README.md  (section "Benchmark: when does spinning win?")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static inline void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    sched_yield();
#endif
}

/* ---- the hand-made locks (compact copies of 02, 03, 06) ------------------ */
typedef struct { atomic_bool locked; } Ttas;
static void ttas_lock(Ttas *l)
{
    unsigned delay = 1;
    for (;;) {
        while (atomic_load_explicit(&l->locked, memory_order_relaxed))
            cpu_relax();
        if (!atomic_exchange_explicit(&l->locked, true, memory_order_acquire))
            return;
        for (unsigned i = 0; i < delay; i++)
            cpu_relax();
        if (delay < 1024)
            delay *= 2;
        else
            sched_yield();
    }
}
static void ttas_unlock(Ttas *l)
{
    atomic_store_explicit(&l->locked, false, memory_order_release);
}

typedef struct { atomic_uint next, owner; } Ticket;
static void ticket_lock(Ticket *l)
{
    unsigned my = atomic_fetch_add_explicit(&l->next, 1, memory_order_relaxed);
    unsigned spins = 0;
    while (atomic_load_explicit(&l->owner, memory_order_acquire) != my)
        if (++spins % 1024 == 0)
            sched_yield();
        else
            cpu_relax();
}
static void ticket_unlock(Ticket *l)
{
    unsigned o = atomic_load_explicit(&l->owner, memory_order_relaxed);
    atomic_store_explicit(&l->owner, o + 1, memory_order_release);
}

typedef struct { atomic_int state; } Fmutex;
static void fmutex_lock(Fmutex *m)
{
    int c = 0;
    if (atomic_compare_exchange_strong_explicit(&m->state, &c, 1,
                                                memory_order_acquire,
                                                memory_order_relaxed))
        return;
    if (c != 2)
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    while (c != 0) {
        syscall(SYS_futex, &m->state, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 2,
                NULL, NULL, 0);
        c = atomic_exchange_explicit(&m->state, 2, memory_order_acquire);
    }
}
static void fmutex_unlock(Fmutex *m)
{
    if (atomic_fetch_sub_explicit(&m->state, 1, memory_order_release) != 1) {
        atomic_store_explicit(&m->state, 0, memory_order_release);
        syscall(SYS_futex, &m->state, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL,
                NULL, 0);
    }
}

/* ---- one table of locks behind a tiny vtable ----------------------------- */
enum { L_MUTEX, L_ADAPTIVE, L_PSPIN, L_TTAS, L_TICKET, L_FUTEX, L_COUNT };
static const char *lock_name[L_COUNT] = {
    "pthread_mutex (normal)", "pthread_mutex ADAPTIVE_NP", "pthread_spinlock",
    "TTAS + backoff", "ticket lock", "futex mutex (06)"};

static pthread_mutex_t g_mutex, g_adaptive;
static pthread_spinlock_t g_pspin;
static Ttas g_ttas;
static Ticket g_ticket;
static Fmutex g_fm;

static void do_lock(int k)
{
    switch (k) {
    case L_MUTEX:    pthread_mutex_lock(&g_mutex); break;
    case L_ADAPTIVE: pthread_mutex_lock(&g_adaptive); break;
    case L_PSPIN:    pthread_spin_lock(&g_pspin); break;
    case L_TTAS:     ttas_lock(&g_ttas); break;
    case L_TICKET:   ticket_lock(&g_ticket); break;
    case L_FUTEX:    fmutex_lock(&g_fm); break;
    }
}
static void do_unlock(int k)
{
    switch (k) {
    case L_MUTEX:    pthread_mutex_unlock(&g_mutex); break;
    case L_ADAPTIVE: pthread_mutex_unlock(&g_adaptive); break;
    case L_PSPIN:    pthread_spin_unlock(&g_pspin); break;
    case L_TTAS:     ttas_unlock(&g_ttas); break;
    case L_TICKET:   ticket_unlock(&g_ticket); break;
    case L_FUTEX:    fmutex_unlock(&g_fm); break;
    }
}

/* ---- the workload -------------------------------------------------------- */
typedef struct {
    int kind;
    int iters;
    int cs_work;   /* loop iterations inside the critical section */
    int pin_cpu;   /* -1 = do not pin */
    long *counter; /* protected by the lock under test */
    bool pinned_ok;
} Job;

static void *worker(void *p)
{
    Job *j = p;
    if (j->pin_cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(j->pin_cpu, &set);
        j->pinned_ok = pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
    }
    for (int i = 0; i < j->iters; i++) {
        do_lock(j->kind);
        long v = *j->counter;
        for (volatile int w = 0; w < j->cs_work; w++) {
        }
        *j->counter = v + 1;
        do_unlock(j->kind);
        /* a little work OUTSIDE the lock too, like a real program */
        for (volatile int w = 0; w < 50; w++) {
        }
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* CPU time (user + system) of the whole process so far, in ms. */
static double cpu_ms(void)
{
    struct rusage ru;
    CHECK(getrusage(RUSAGE_SELF, &ru) == 0);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 +
           (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

/* Runs one lock kind in one scenario, returns ns per lock/unlock pair and
 * the CPU time burnt per wall-clock time (*cpu_ratio: 1.0 = one core busy). */
static double run(int kind, int nthreads, int iters, int cs_work, int pin_cpu,
                  bool *pin_failed, double *cpu_ratio)
{
    long counter = 0;
    Job jobs[8];
    pthread_t th[8];
    double c0 = cpu_ms();
    double t0 = now_ms();
    for (int i = 0; i < nthreads; i++) {
        jobs[i] = (Job){kind, iters, cs_work, pin_cpu, &counter, true};
        CHECK(pthread_create(&th[i], NULL, worker, &jobs[i]) == 0);
    }
    for (int i = 0; i < nthreads; i++) {
        CHECK(pthread_join(th[i], NULL) == 0);
        if (!jobs[i].pinned_ok)
            *pin_failed = true;
    }
    double ms = now_ms() - t0;
    *cpu_ratio = (cpu_ms() - c0) / (ms > 0 ? ms : 1);
    CHECK(counter == (long)nthreads * iters); /* the only real assertion */
    return ms * 1e6 / ((double)nthreads * iters);
}

static void scenario(const char *title, int nthreads, int iters, int cs_work,
                     int pin_cpu)
{
    printf("\n%s\n  %d threads x %d iterations, critical section = %d loop "
           "steps\n", title, nthreads, iters, cs_work);
    printf("  %-26s %12s %16s\n", "lock", "ns/lock+unl", "cores busy");
    bool pin_failed = false;
    for (int k = 0; k < L_COUNT; k++) {
        double ratio;
        double ns = run(k, nthreads, iters, cs_work, pin_cpu, &pin_failed,
                        &ratio);
        printf("  %-26s %12.0f %16.2f\n", lock_name[k], ns, ratio);
    }
    if (pin_failed)
        printf("  (could not pin to one CPU here, e.g. restricted cpuset: "
               "numbers are for unpinned threads)\n");
}

int main(void)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int nthreads = (int)(ncpu < 4 ? (ncpu < 2 ? 2 : ncpu) : 4);
    printf("online CPUs: %ld (all numbers below are timing-dependent)\n", ncpu);

    CHECK(pthread_mutex_init(&g_mutex, NULL) == 0);
    /* Adaptive mutex (glibc extension): on contention it first SPINS a
     * bounded number of times (tuned per lock by past behaviour, glibc
     * tunable glibc.pthread.mutex_spin_count, default 100), and only then
     * sleeps in futex_wait. The best of both for short critical sections. */
    pthread_mutexattr_t ma;
    CHECK(pthread_mutexattr_init(&ma) == 0);
    CHECK(pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_ADAPTIVE_NP) == 0);
    CHECK(pthread_mutex_init(&g_adaptive, &ma) == 0);
    CHECK(pthread_mutexattr_destroy(&ma) == 0);
    CHECK(pthread_spin_init(&g_pspin, PTHREAD_PROCESS_PRIVATE) == 0);
    atomic_init(&g_ttas.locked, false);
    atomic_init(&g_ticket.next, 0);
    atomic_init(&g_ticket.owner, 0);
    atomic_init(&g_fm.state, 0);

    /* Iteration counts are small so the whole run stays well under 10 s
     * even under ThreadSanitizer. */
    scenario("A. short critical section, threads on different cores",
             nthreads, 10000, 10, -1);
    scenario("B. long critical section (spinners burn a core while waiting)",
             nthreads, 800, 2000, -1);
    scenario("C. 2 threads pinned to ONE CPU (owner preempted -> spinner "
             "wastes its slice)", 2, 1000, 300, 0);

    printf("\nHow to read it:\n"
           "  ns/lock+unl : wall-clock cost. Spinlocks often look good here\n"
           "                when every thread has a core of its own (A).\n"
           "  cores busy  : CPU time / wall time. A sleeping lock lets waiters\n"
           "                give their core away (< threads); a spinlock keeps\n"
           "                every waiter at 100%% (B): that CPU is stolen from\n"
           "                other work, e.g. your GStreamer pipeline.\n"
           "  C           : one core. Spinning can never help: the owner can\n"
           "                only finish while the spinner is NOT running.\n"
           "                The ticket lock is often 10-50x slower here: the\n"
           "                next ticket holder must be scheduled before ANYONE\n"
           "                can proceed. Our locks yield after 1024 spins,\n"
           "                which limits the damage; pthread_spinlock never\n"
           "                yields and relies on the time slice running out.\n");

    CHECK(pthread_mutex_destroy(&g_mutex) == 0);
    CHECK(pthread_mutex_destroy(&g_adaptive) == 0);
    CHECK(pthread_spin_destroy(&g_pspin) == 0);
    printf("07_lock_benchmark: all checks passed\n");
    return 0;
}
