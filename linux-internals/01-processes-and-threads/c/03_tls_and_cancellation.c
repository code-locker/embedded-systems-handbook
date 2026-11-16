/*
 * 03_tls_and_cancellation.c — thread-local storage (_Thread_local, __thread,
 *                             pthread keys) and cancellation with cleanup
 *
 * Concept : A thread-local variable has one instance PER THREAD at the same
 *           name (errno is the famous one). pthread keys add a destructor
 *           that runs when each thread exits. pthread_cancel() asks a thread
 *           to stop at its next cancellation point; cleanup handlers pushed
 *           with pthread_cleanup_push() release what it holds (memory,
 *           mutexes) on the way out.
 * Build   : make run P=linux-internals/01-processes-and-threads/c/03_tls_and_cancellation.c
 * Read    : ../README.md  (sections 3.7 and 3.8)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define NTHREADS 4
#define ITERS 1000

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* ===== 1. thread-local variables ======================================= */

/*
 *   _Thread_local int tls_count;   (C11 keyword; <threads.h> spells it
 *                                    thread_local; GCC's older spelling is
 *                                    __thread)
 *
 *   thread A ──> [ TLS block A: tls_count ]
 *   thread B ──> [ TLS block B: tls_count ]      same name, own copy each
 *   thread C ──> [ TLS block C: tls_count ]
 *
 * On arm64 the CPU register TPIDR_EL0 points at the current thread's TLS
 * block (x86-64 uses the FS segment base); the kernel switches it on every
 * context switch, so "tls_count" resolves to a different address per thread.
 */
static _Thread_local int tls_count;   /* C11 */
static __thread int tls_count_gnu;    /* GNU extension, same thing */
static int shared_count_unused;       /* NOT thread-local: one for all */

typedef struct {
    int id;
    int final_tls;
    uintptr_t tls_addr;
} TlsResult;

static void *tls_worker(void *arg)
{
    TlsResult *r = arg;
    for (int i = 0; i < ITERS; i++) {
        tls_count++;      /* no lock needed: nobody else can see it */
        tls_count_gnu += 2;
    }
    r->final_tls = tls_count;
    r->tls_addr = (uintptr_t)&tls_count;
    CHECK(tls_count_gnu == 2 * ITERS);
    return NULL;
}

static void demo_thread_local(void)
{
    printf("[1] _Thread_local / __thread: one copy of the variable per thread\n");
    pthread_t th[NTHREADS];
    TlsResult res[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        res[i] = (TlsResult){.id = i};
        CHECK(pthread_create(&th[i], NULL, tls_worker, &res[i]) == 0);
    }
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    for (int i = 0; i < NTHREADS; i++) {
        printf("    thread %d: tls_count = %d at 0x%jx\n", i, res[i].final_tls,
               (uintmax_t)res[i].tls_addr);
        CHECK(res[i].final_tls == ITERS); /* each counted only its own */
    }
    CHECK(tls_count == 0); /* main's copy was never touched */
    printf("    main: tls_count = %d at %p (its own copy)\n", tls_count,
           (void *)&tls_count);
    (void)shared_count_unused;

    /* errno is thread-local too (glibc: a macro around a TLS variable), so a
     * failing call in one thread cannot clobber another thread's errno. */
}

/* ===== 2. pthread keys: TLS with a destructor ========================== */

static pthread_key_t g_buf_key;
static atomic_int g_destructor_runs;

static void free_thread_buffer(void *p)
{
    /* Called automatically at thread exit for every thread whose value
     * for the key is non-NULL. This is how a library (e.g. a per-thread
     * scratch buffer in a logging library) cleans up without the caller
     * knowing. Note: NOT called for the main thread when it returns from
     * main() — free that one yourself. */
    free(p);
    atomic_fetch_add(&g_destructor_runs, 1);
}

static char *thread_buffer(void)
{
    char *b = pthread_getspecific(g_buf_key);
    if (!b) { /* first use in this thread: allocate lazily */
        b = malloc(64);
        CHECK(b != NULL);
        CHECK(pthread_setspecific(g_buf_key, b) == 0);
    }
    return b;
}

static void *key_worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    char *b = thread_buffer();
    snprintf(b, 64, "scratch of thread %d", id);
    CHECK(thread_buffer() == b); /* same buffer on the second call */
    return NULL;
}

static void demo_keys(void)
{
    printf("\n[2] pthread_key_create: per-thread value + destructor at exit\n");
    CHECK(pthread_key_create(&g_buf_key, free_thread_buffer) == 0);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        /* passing a small integer BY VALUE through the void*: no lifetime
         * problem at all, unlike passing &i */
        CHECK(pthread_create(&th[i], NULL, key_worker, (void *)(intptr_t)i) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    printf("    destructor ran %d times (one per exited thread)\n",
           atomic_load(&g_destructor_runs));
    CHECK(atomic_load(&g_destructor_runs) == NTHREADS);
    CHECK(pthread_key_delete(g_buf_key) == 0);
}

/* ===== 3. cancellation + cleanup handlers ============================== */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_cleanup_free_ran, g_cleanup_unlock_ran, g_loops;

static void cleanup_free(void *p)
{
    free(p);
    atomic_store(&g_cleanup_free_ran, 1);
}

static void cleanup_unlock(void *m)
{
    pthread_mutex_unlock(m);
    atomic_store(&g_cleanup_unlock_ran, 1);
}

/*
 * The cleanup stack, pushed in order, run in REVERSE when the thread is
 * cancelled (or calls pthread_exit, or pops with execute=1):
 *
 *     push(cleanup_free, buf)       ─┐  runs 2nd
 *     push(cleanup_unlock, &lock)   ─┘  runs 1st
 *
 * pthread_cleanup_push/pop are MACROS that open and close a { } block, so
 * they must appear in pairs in the same scope.
 */
static void *cancellable_worker(void *arg)
{
    (void)arg;
    char *buf = malloc(4096);
    CHECK(buf != NULL);
    pthread_cleanup_push(cleanup_free, buf);

    CHECK(pthread_mutex_lock(&g_lock) == 0);
    pthread_cleanup_push(cleanup_unlock, &g_lock);

    /* A critical section where we do NOT want to be cancelled half-way:
     * temporarily disable cancellation. */
    int old;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old);
    memset(buf, 0xAB, 4096); /* pretend: update a data structure */
    pthread_setcancelstate(old, NULL);

    for (;;) {
        atomic_fetch_add(&g_loops, 1);
        /* With the default DEFERRED cancel type, cancellation only happens
         * at "cancellation points": blocking calls such as read, write,
         * nanosleep, pthread_cond_wait (see `man 7 pthreads`) and
         * pthread_testcancel(). sched_yield() is NOT one, so here the
         * thread can only be cancelled at the explicit testcancel below —
         * a well-defined place where our state is consistent.
         * (Why not nanosleep? It would work with glibc, but ThreadSanitizer
         * loses track of a mutex unlocked by a cleanup handler that runs
         * while cancelling out of an intercepted blocking call and reports
         * a false "double lock".) */
        sched_yield();
        pthread_testcancel();
    }

    pthread_cleanup_pop(1); /* never reached, but must be here to close */
    pthread_cleanup_pop(1); /* the blocks opened by the push macros     */
    return NULL;
}

static void demo_cancel(void)
{
    printf("\n[3] pthread_cancel + cleanup handlers\n");
    pthread_t t;
    CHECK(pthread_create(&t, NULL, cancellable_worker, NULL) == 0);
    while (atomic_load(&g_loops) < 5) /* let it get going */
        sleep_ms(1);

    CHECK(pthread_cancel(t) == 0); /* a REQUEST, returns immediately */
    void *ret;
    CHECK(pthread_join(t, &ret) == 0);
    printf("    join returned %s, cleanup: free=%d unlock=%d, loops before"
           " cancel: %d (timing-dependent)\n",
           ret == PTHREAD_CANCELED ? "PTHREAD_CANCELED" : "something else",
           atomic_load(&g_cleanup_free_ran), atomic_load(&g_cleanup_unlock_ran),
           atomic_load(&g_loops));
    CHECK(ret == PTHREAD_CANCELED);
    CHECK(atomic_load(&g_cleanup_free_ran) && atomic_load(&g_cleanup_unlock_ran));
    /* proof the mutex was released by the handler: we can take it */
    CHECK(pthread_mutex_trylock(&g_lock) == 0);
    CHECK(pthread_mutex_unlock(&g_lock) == 0);
}

int main(void)
{
    demo_thread_local();
    demo_keys();
    demo_cancel();
    printf("\n03_tls_and_cancellation: all checks passed\n");
    return 0;
}
