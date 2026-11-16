/*
 * 03_once_and_dcl.c — run initialisation exactly once: pthread_once, and
 *                     double-checked locking done RIGHT with C11 atomics
 *
 * Concept : "Create the expensive thing on first use" from many threads at
 *           once must (1) create it exactly once and (2) never let a thread
 *           see a pointer to a half-constructed object. pthread_once()
 *           does both. Double-checked locking (check without the lock,
 *           lock, check again) is the hand-made fast version. It was famous
 *           for being broken, because with a PLAIN pointer the compiler or
 *           CPU may publish the pointer before the object's fields. With an
 *           atomic pointer and acquire/release it is correct.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/03_once_and_dcl.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/03_once_and_dcl.c SAN=tsan
 * Read    : ../README.md  (sections "Once", "Double-checked locking")
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define NTHREADS 8

/* The expensive singleton: e.g. a lookup table or a hardware handle. */
typedef struct {
    int table[256];
    int ready_magic; /* set last: a half-built object has 0 here */
} Codec;

static Codec *codec_create(atomic_int *counter)
{
    atomic_fetch_add(counter, 1);
    Codec *c = malloc(sizeof *c);
    CHECK(c != NULL);
    for (int i = 0; i < 256; i++)
        c->table[i] = i * i;
    c->ready_magic = 0xC0DEC;
    return c;
}

static int codec_ok(const Codec *c)
{
    return c->ready_magic == 0xC0DEC && c->table[255] == 255 * 255;
}

/* ---- 1. pthread_once ----------------------------------------------------- */
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static Codec *g_codec_once;        /* written once inside init_once */
static atomic_int g_once_inits;

static void init_once(void) /* no arguments, no return value: use globals */
{
    g_codec_once = codec_create(&g_once_inits);
}

static Codec *get_codec_once(void)
{
    /* All callers block until the first caller's init_once() returns, and
     * then see everything it wrote (pthread_once synchronises). On the
     * fast path it is one acquire load in glibc (a futex word). */
    CHECK(pthread_once(&g_once, init_once) == 0);
    return g_codec_once;
}

/* ---- 2. double-checked locking, correct version -------------------------
 *
 *  BROKEN (plain pointer):                 CORRECT (atomic pointer):
 *    if (p == NULL) {                        q = load(p, ACQUIRE);
 *        lock();                             if (q == NULL) {
 *        if (p == NULL)                          lock();
 *            p = create();  <-- may store        q = load(p, relaxed);
 *        unlock();              p BEFORE         if (q == NULL) {
 *    }                          the fields!          q = create();
 *    use(p);   <- other thread sees p != NULL        store(p, q, RELEASE);
 *                 but table[] still garbage      }
 *                                                unlock();
 *                                            }
 *                                            use(q);
 *
 *  The release store publishes the fully built object; the acquire load
 *  on the fast path guarantees the reader sees those fields. On ARMv8 the
 *  fast path costs one LDAR: practically free.
 * ------------------------------------------------------------------------ */
static _Atomic(Codec *) g_codec_dcl;
static pthread_mutex_t g_dcl_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_dcl_inits;
static atomic_int g_dcl_slow_path; /* how many callers took the lock */

static Codec *get_codec_dcl(void)
{
    Codec *c = atomic_load_explicit(&g_codec_dcl, memory_order_acquire);
    if (c == NULL) {                                  /* check 1, no lock */
        atomic_fetch_add(&g_dcl_slow_path, 1);
        pthread_mutex_lock(&g_dcl_lock);
        c = atomic_load_explicit(&g_codec_dcl, memory_order_relaxed);
        if (c == NULL) {                              /* check 2, locked  */
            c = codec_create(&g_dcl_inits);
            atomic_store_explicit(&g_codec_dcl, c, memory_order_release);
        }
        pthread_mutex_unlock(&g_dcl_lock);
    }
    return c;
}

/* ---- the race: all threads call both getters at the same moment ---------- */
static pthread_barrier_t g_start;

static void *worker(void *arg)
{
    (void)arg;
    pthread_barrier_wait(&g_start); /* maximise the collision */
    for (int i = 0; i < 1000; i++) {
        Codec *a = get_codec_once();
        Codec *b = get_codec_dcl();
        CHECK(codec_ok(a));
        CHECK(codec_ok(b));
    }
    return NULL;
}

int main(void)
{
    CHECK(pthread_barrier_init(&g_start, NULL, NTHREADS) == 0);
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, worker, NULL) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);
    CHECK(pthread_barrier_destroy(&g_start) == 0);

    printf("pthread_once : %d threads x 1000 calls -> init ran %d time(s)\n",
           NTHREADS, atomic_load(&g_once_inits));
    printf("DCL (atomic) : init ran %d time(s); %d call(s) took the lock "
           "(timing-dependent, at most %d), the other %d were lock-free\n",
           atomic_load(&g_dcl_inits), atomic_load(&g_dcl_slow_path), NTHREADS,
           NTHREADS * 1000 - atomic_load(&g_dcl_slow_path));
    CHECK(atomic_load(&g_once_inits) == 1);
    CHECK(atomic_load(&g_dcl_inits) == 1);
    /* Only calls that ran before publication can take the slow path, and
     * each thread does so at most once (its 2nd call sees the pointer). */
    CHECK(atomic_load(&g_dcl_slow_path) >= 1 &&
          atomic_load(&g_dcl_slow_path) <= NTHREADS);

    /* Ownership: the singletons live until the end of the program; free
     * them here so LeakSanitizer stays quiet. */
    free(g_codec_once);
    free(atomic_load(&g_codec_dcl));
    printf("03_once_and_dcl: all checks passed\n");
    return 0;
}
