/*
 * 02_mutex_types.c — NORMAL, ERRORCHECK, RECURSIVE and DEFAULT mutexes
 *
 * Concept : pthread_mutexattr_settype() decides what happens when a thread
 *           misuses a mutex: locks it twice, or unlocks one it does not own.
 *             NORMAL     : relock = deadlock forever, bad unlock = undefined
 *             ERRORCHECK : relock -> EDEADLK, bad unlock -> EPERM  (debug!)
 *             RECURSIVE  : owner may relock; needs as many unlocks as locks
 *             DEFAULT    : "undefined" in POSIX; NORMAL on glibc
 * Build   : make run P=linux-internals/03-mutexes-and-deadlock/c/02_mutex_types.c
 *           make run P=linux-internals/03-mutexes-and-deadlock/c/02_mutex_types.c SAN=tsan
 * Read    : ../README.md  (section 3.4 "Mutex types")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
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

/* ThreadSanitizer itself reports "unlock of an unlocked mutex (or by a wrong
 * thread)" for the bad unlocks below — even though the ERRORCHECK mutex
 * returns a clean EPERM. That is TSan doing its job, so the misuse parts
 * are skipped under TSan (GCC defines __SANITIZE_THREAD__, Clang has
 * __has_feature(thread_sanitizer)). */
#if defined(__SANITIZE_THREAD__)
#define UNDER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define UNDER_TSAN 1
#endif
#endif
#ifndef UNDER_TSAN
#define UNDER_TSAN 0
#endif

static void make_mutex(pthread_mutex_t *m, int type)
{
    pthread_mutexattr_t a;
    CHECK(pthread_mutexattr_init(&a) == 0);
    CHECK(pthread_mutexattr_settype(&a, type) == 0);
    CHECK(pthread_mutex_init(m, &a) == 0);
    CHECK(pthread_mutexattr_destroy(&a) == 0); /* safe: init copied it */
}

/* A second thread that tries to unlock / trylock a mutex it does not own. */
typedef struct {
    pthread_mutex_t *m;
    int unlock_rc;
    int trylock_rc;
    int do_unlock;
} Foreign;

static void *foreign_thread(void *arg)
{
    Foreign *f = arg;
    f->trylock_rc = pthread_mutex_trylock(f->m);
    if (f->trylock_rc == 0)
        pthread_mutex_unlock(f->m);
    if (f->do_unlock)
        f->unlock_rc = pthread_mutex_unlock(f->m);
    return NULL;
}

static void run_foreign(Foreign *f)
{
    pthread_t t;
    CHECK(pthread_create(&t, NULL, foreign_thread, f) == 0);
    CHECK(pthread_join(t, NULL) == 0);
}

/* ------------------------------------------------------------------------
 * NORMAL: fastest, no checks.
 *
 *   lock(m); lock(m);   -> the thread waits for ITSELF. Forever. No error.
 *
 * We do not do that (it would hang); trylock by the owner shows the state
 * instead: EBUSY, the mutex has no idea that "busy" means "busy by you".
 * ---------------------------------------------------------------------- */
static void demo_normal(void)
{
    pthread_mutex_t m;
    make_mutex(&m, PTHREAD_MUTEX_NORMAL);
    CHECK(pthread_mutex_lock(&m) == 0);
    int rc = pthread_mutex_trylock(&m);
    printf("[NORMAL]     owner trylocks again   -> %s  (a lock() here would "
           "self-deadlock)\n", strerror(rc));
    CHECK(rc == EBUSY);
    CHECK(pthread_mutex_unlock(&m) == 0);
    CHECK(pthread_mutex_destroy(&m) == 0);
}

/* ------------------------------------------------------------------------
 * ERRORCHECK: records the owner's thread id and checks it on every call.
 * Slightly slower; turn it on in debug builds to catch misuse early.
 * ---------------------------------------------------------------------- */
static void demo_errorcheck(void)
{
    pthread_mutex_t m;
    make_mutex(&m, PTHREAD_MUTEX_ERRORCHECK);

    if (UNDER_TSAN) {
        printf("[ERRORCHECK] misuse demos skipped under TSan (TSan reports "
               "them itself)\n");
        CHECK(pthread_mutex_destroy(&m) == 0);
        return;
    }

    CHECK(pthread_mutex_lock(&m) == 0);
    int rc = pthread_mutex_lock(&m); /* relock by owner */
    printf("[ERRORCHECK] owner locks again      -> %s\n", strerror(rc));
    CHECK(rc == EDEADLK);

    Foreign f = {.m = &m, .do_unlock = 1};
    run_foreign(&f);
    printf("[ERRORCHECK] other thread unlocks   -> %s\n", strerror(f.unlock_rc));
    CHECK(f.unlock_rc == EPERM);

    CHECK(pthread_mutex_unlock(&m) == 0);
    rc = pthread_mutex_unlock(&m); /* unlock an unlocked mutex */
    printf("[ERRORCHECK] unlock when unlocked   -> %s\n", strerror(rc));
    CHECK(rc == EPERM);
    CHECK(pthread_mutex_destroy(&m) == 0);
}

/* ------------------------------------------------------------------------
 * RECURSIVE: owner + count.
 *
 *     lock   lock   lock   unlock unlock unlock
 *  count: 1      2      3      2      1      0  <- only now free for others
 *
 * Useful when a locked function calls another public function that also
 * locks. Often a design smell: it hides who really holds the lock and makes
 * condition-variable waits wrong (wait() releases only ONE level).
 * ---------------------------------------------------------------------- */
static pthread_mutex_t g_rec;
static int g_depth_seen;

static void recurse(int depth)
{
    pthread_mutex_lock(&g_rec);
    if (depth > g_depth_seen)
        g_depth_seen = depth;
    if (depth < 3)
        recurse(depth + 1); /* relocks the mutex we already own */
    pthread_mutex_unlock(&g_rec);
}

static void demo_recursive(void)
{
    make_mutex(&g_rec, PTHREAD_MUTEX_RECURSIVE);
    recurse(1);
    printf("[RECURSIVE]  recursion depth 3 relocked the mutex 3 times: ok\n");
    CHECK(g_depth_seen == 3);

    CHECK(pthread_mutex_lock(&g_rec) == 0);
    CHECK(pthread_mutex_lock(&g_rec) == 0); /* count = 2 */
    CHECK(pthread_mutex_unlock(&g_rec) == 0); /* count = 1: still owned */
    Foreign f = {.m = &g_rec, .do_unlock = !UNDER_TSAN};
    run_foreign(&f);
    printf("[RECURSIVE]  after 2 locks + 1 unlock, other thread trylock -> %s\n",
           strerror(f.trylock_rc));
    CHECK(f.trylock_rc == EBUSY);
    if (!UNDER_TSAN) {
        /* glibc checks ownership for recursive mutexes too. */
        printf("[RECURSIVE]  other thread unlocks -> %s\n",
               strerror(f.unlock_rc));
        CHECK(f.unlock_rc == EPERM);
    }
    CHECK(pthread_mutex_unlock(&g_rec) == 0); /* count = 0: free */
    Foreign g = {.m = &g_rec};
    run_foreign(&g);
    printf("[RECURSIVE]  after the last unlock, other thread trylock -> %s\n",
           strerror(g.trylock_rc));
    CHECK(g.trylock_rc == 0);
    CHECK(pthread_mutex_destroy(&g_rec) == 0);
}

/* ------------------------------------------------------------------------
 * DEFAULT: whatever the implementation likes. Read back the type.
 * ---------------------------------------------------------------------- */
static const char *type_name(int t)
{
    switch (t) {
    case PTHREAD_MUTEX_NORMAL: return "NORMAL";
    case PTHREAD_MUTEX_ERRORCHECK: return "ERRORCHECK";
    case PTHREAD_MUTEX_RECURSIVE: return "RECURSIVE";
    default: return "?";
    }
}

static void demo_default(void)
{
    pthread_mutexattr_t a;
    int t = -1;
    CHECK(pthread_mutexattr_init(&a) == 0);
    CHECK(pthread_mutexattr_gettype(&a, &t) == 0);
    printf("[DEFAULT]    a fresh attribute has type %d = %s; "
           "PTHREAD_MUTEX_DEFAULT = %d\n", t, type_name(t), PTHREAD_MUTEX_DEFAULT);
    CHECK(t == PTHREAD_MUTEX_DEFAULT);
    CHECK(pthread_mutexattr_destroy(&a) == 0);

    /* glibc also offers initialisers for the non-default types (GNU ext.):
     * PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP, PTHREAD_RECURSIVE_MUTEX_
     * INITIALIZER_NP. Portable code uses the attribute API shown above. */
#ifdef PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
    pthread_mutex_t e = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
    CHECK(pthread_mutex_lock(&e) == 0);
    CHECK(pthread_mutex_unlock(&e) == 0);
    printf("[DEFAULT]    PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP also works "
           "(glibc)\n");
#endif
}

int main(void)
{
    if (UNDER_TSAN)
        printf("(running under ThreadSanitizer)\n");
    demo_normal();
    demo_errorcheck();
    demo_recursive();
    demo_default();
    printf("02_mutex_types: all checks passed\n");
    return 0;
}
