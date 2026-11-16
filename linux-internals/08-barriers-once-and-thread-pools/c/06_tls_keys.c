/*
 * 06_tls_keys.c — thread-local storage two ways: pthread_key_create (with a
 *                 destructor that runs when each thread exits) and C11
 *                 _Thread_local; plus pthread_once to create the key once
 *
 * Concept : Sometimes each thread needs its OWN copy of something global:
 *           errno, a scratch buffer, a per-thread statistics block, a
 *           random-number state. _Thread_local (C11) / __thread (GCC) is
 *           the fast, simple way for plain values. pthread keys add one
 *           thing it cannot do in C: a DESTRUCTOR per thread, so a buffer
 *           malloc'ed lazily by each thread is freed automatically when
 *           that thread exits.
 * Build   : make run P=linux-internals/08-barriers-once-and-thread-pools/c/06_tls_keys.c
 *           make run P=linux-internals/08-barriers-once-and-thread-pools/c/06_tls_keys.c SAN=tsan
 * Read    : ../README.md  (section "Thread-local storage")
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

#define NTHREADS 6

/*
 *   key K (one per process)       thread A           thread B
 *                                 slot[K] -> bufA    slot[K] -> bufB
 *   destructor(buf) ------------> runs at A's exit   runs at B's exit
 *                                 with bufA          with bufB
 *
 *   The destructor runs only if the slot is non-NULL, and only when the
 *   thread exits (return from start routine or pthread_exit) - NOT when
 *   main() returns (exit() does not run TLS destructors for main).
 */
typedef struct {
    char text[64];
    int uses;
    int owner;
} ScratchBuf;

static pthread_key_t g_key;
static pthread_once_t g_key_once = PTHREAD_ONCE_INIT;
static atomic_int g_destructor_runs;
static atomic_int g_destructor_uses; /* sum of `uses` seen by destructors */

static void scratch_destructor(void *p)
{
    ScratchBuf *b = p;
    atomic_fetch_add(&g_destructor_runs, 1);
    atomic_fetch_add(&g_destructor_uses, b->uses);
    free(b); /* ownership: the key's destructor frees each thread's buffer */
}

static void make_key(void)
{
    CHECK(pthread_key_create(&g_key, scratch_destructor) == 0);
}

/* Lazily get this thread's scratch buffer (like glibc does for strerror's
 * buffer or dlerror's message). */
static ScratchBuf *scratch(int owner)
{
    pthread_once(&g_key_once, make_key); /* create the key exactly once */
    ScratchBuf *b = pthread_getspecific(g_key);
    if (b == NULL) {
        b = calloc(1, sizeof *b);
        CHECK(b != NULL);
        b->owner = owner;
        CHECK(pthread_setspecific(g_key, b) == 0);
    }
    return b;
}

/* C11 thread-local: every thread has its own copy, initialised to 0.
 * Compiles to an offset from the thread pointer (TPIDR_EL0 on ARMv8,
 * %fs on x86-64): as cheap as a global. */
static _Thread_local int tl_counter;
static _Thread_local const char *tl_name = "unnamed";

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    char name[16];
    snprintf(name, sizeof name, "worker-%d", id);
    tl_name = name; /* points into this thread's stack: fine while it runs */

    for (int i = 0; i <= id; i++) { /* thread id uses it id+1 times */
        ScratchBuf *b = scratch(id);
        CHECK(b->owner == id); /* never sees another thread's buffer */
        snprintf(b->text, sizeof b->text, "%s pass %d", tl_name, i);
        b->uses++;
        tl_counter++;
    }
    CHECK(tl_counter == id + 1); /* nobody else touched MY counter */
    return NULL; /* -> scratch_destructor(my buffer) runs now */
}

int main(void)
{
    pthread_t th[NTHREADS];
    for (long i = 0; i < NTHREADS; i++)
        CHECK(pthread_create(&th[i], NULL, worker, (void *)i) == 0);
    for (int i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    int want_uses = NTHREADS * (NTHREADS + 1) / 2; /* 1 + 2 + ... + 6 */
    printf("%d threads exited: destructor ran %d times, saw %d uses in "
           "total (expected %d)\n", NTHREADS, atomic_load(&g_destructor_runs),
           atomic_load(&g_destructor_uses), want_uses);
    CHECK(atomic_load(&g_destructor_runs) == NTHREADS);
    CHECK(atomic_load(&g_destructor_uses) == want_uses);

    /* main's own copies are untouched by the workers */
    CHECK(tl_counter == 0);
    CHECK(strcmp(tl_name, "unnamed") == 0);
    printf("main's _Thread_local counter is still %d, name \"%s\"\n",
           tl_counter, tl_name);

    /* main is a thread too: it gets its own buffer. Its destructor would
     * NOT run on return from main, so free it by hand and clear the slot. */
    ScratchBuf *mine = scratch(-1);
    CHECK(mine->owner == -1 && mine->uses == 0);
    CHECK(pthread_setspecific(g_key, NULL) == 0);
    free(mine);
    /* pthread_key_delete does NOT call destructors either. */
    CHECK(pthread_key_delete(g_key) == 0);

    printf("06_tls_keys: all checks passed\n");
    return 0;
}
