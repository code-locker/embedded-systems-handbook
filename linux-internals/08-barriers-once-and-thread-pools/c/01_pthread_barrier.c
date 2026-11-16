/*
 * 01_pthread_barrier.c — pthread_barrier_t for phased computation: a 1-D
 *                        heat-diffusion (Jacobi) solver split across
 *                        threads, one barrier per time step
 *
 * Concept : Some parallel work runs in PHASES: every thread must finish
 *           step k before anyone may start step k+1, because step k+1
 *           reads what all threads wrote in step k. A barrier is the
 *           meeting point: nobody passes until all N threads arrived.
 *           Exactly one of them gets PTHREAD_BARRIER_SERIAL_THREAD back
 *           and can do the "between phases" work alone (swap buffers,
 *           check convergence, print progress).
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/01_pthread_barrier.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/01_pthread_barrier.c SAN=tsan
 * Read    : ../README.md  (section "pthread_barrier_t")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define N 256       /* grid points */
#define STEPS 300   /* time steps  */
#define NTHREADS 4

/*
 *  one time step:  next[i] = cur[i] + 0.25 * (cur[i-1] - 2*cur[i] + cur[i+1])
 *
 *   cur:  [ t0 slice ][ t1 slice ][ t2 slice ][ t3 slice ]
 *                    ^ t1 reads cur[63] which t0 owns: t0 must have
 *                      finished WRITING cur (previous step) first
 *
 *   thread 0..3:  compute my slice of next      (phase A)
 *                 barrier                       all of next is written
 *                 SERIAL thread: swap cur/next  (phase B, one thread)
 *                 barrier                       everyone sees the swap
 *
 *  Two barriers per step: the first so the swap happens after all writes,
 *  the second so nobody starts the next step with the old pointers.
 *  (Alternative with one barrier: every thread computes which buffer is
 *  "cur" from the step parity, no swap needed. Exercise 1.)
 */
static double g_a[N], g_b[N];
static double *g_cur = g_a, *g_next = g_b; /* swapped by the serial thread */
static pthread_barrier_t g_bar;
static atomic_int g_serial_count; /* how many times SERIAL was returned */

static void step_range(const double *cur, double *next, int lo, int hi)
{
    for (int i = lo; i < hi; i++) {
        double left = i > 0 ? cur[i - 1] : 0.0;      /* ends held at 0 */
        double right = i < N - 1 ? cur[i + 1] : 0.0;
        next[i] = cur[i] + 0.25 * (left - 2.0 * cur[i] + right);
    }
}

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    int lo = id * N / NTHREADS, hi = (id + 1) * N / NTHREADS;
    for (int s = 0; s < STEPS; s++) {
        step_range(g_cur, g_next, lo, hi);
        int rc = pthread_barrier_wait(&g_bar);
        CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD) {
            /* exactly one thread per round runs this */
            double *t = g_cur;
            g_cur = g_next;
            g_next = t;
            atomic_fetch_add(&g_serial_count, 1);
        }
        /* Second barrier: again one thread gets SERIAL; nothing to do. */
        rc = pthread_barrier_wait(&g_bar);
        CHECK(rc == 0 || rc == PTHREAD_BARRIER_SERIAL_THREAD);
    }
    return NULL;
}

static void init(double *u)
{
    memset(u, 0, sizeof(double) * N);
    for (int i = N / 2 - 8; i < N / 2 + 8; i++)
        u[i] = 100.0; /* a hot spot in the middle */
}

int main(void)
{
    /* Reference: single-threaded, same arithmetic in the same order per
     * element, so the result must be BIT-identical. */
    static double ra[N], rb[N];
    double *rc = ra, *rn = rb;
    init(rc);
    for (int s = 0; s < STEPS; s++) {
        step_range(rc, rn, 0, N);
        double *t = rc;
        rc = rn;
        rn = t;
    }

    init(g_a);
    /* count = number of threads that must call wait() before any passes */
    CHECK(pthread_barrier_init(&g_bar, NULL, NTHREADS) == 0);
    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, worker, (void *)i) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&g_bar) == 0);

    double total = 0, ref_total = 0;
    for (int i = 0; i < N; i++) {
        total += g_cur[i];
        ref_total += rc[i];
    }
    printf("%d threads, %d steps: heat left %.6f (serial reference %.6f), "
           "peak %.4f\n", NTHREADS, STEPS, total, ref_total, g_cur[N / 2]);
    printf("PTHREAD_BARRIER_SERIAL_THREAD returned %d times (once per round "
           "of the first barrier)\n", atomic_load(&g_serial_count));
    CHECK(memcmp(g_cur, rc, sizeof(double) * N) == 0); /* bit-identical */
    CHECK(atomic_load(&g_serial_count) == STEPS);

    printf("01_pthread_barrier: all checks passed\n");
    return 0;
}
