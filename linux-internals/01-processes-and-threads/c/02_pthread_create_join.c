/*
 * 02_pthread_create_join.c — threads: create, join, detach, arguments,
 *                            return values, attributes, ids, /proc/self/task
 *
 * Concept : A thread is a second flow of control inside the SAME process:
 *           same address space, same fds, but its own stack, registers and
 *           thread id. Give every thread its OWN argument object that lives
 *           until the thread is joined; return results through that object
 *           or through a heap pointer the joiner frees. On Linux every
 *           thread is a kernel task: same PID (tgid), different TID.
 * Build   : make run P=linux-internals/01-processes-and-threads/c/02_pthread_create_join.c
 * Read    : ../README.md  (sections 3.4 - 3.6, 3.9)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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

#define NTHREADS 4

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* ===== 1. arguments in, results out ==================================== */

/*
 * One Task per thread. main() owns the array; it must stay alive until the
 * thread is joined. That is the whole rule: "the argument outlives the
 * thread's use of it".
 *
 *     main stack:  tasks[0] tasks[1] tasks[2] tasks[3]
 *                     ^        ^        ^        ^
 *                  thread0  thread1  thread2  thread3   (each gets its own)
 *
 * The broken version — passing &i of the loop variable — is
 * 05_loop_var_arg_bug.c.
 */
typedef struct {
    int id;
    uint64_t from, to; /* sum the range [from, to) */
    uint64_t sum;      /* result, written by the thread, read after join */
    pid_t tid;         /* kernel thread id, filled by the thread */
    uintptr_t stack_addr; /* address of a local variable: proves private
                          * stacks (kept as a number: the local is dead
                          * after the thread ends) */
    int *global_addr;  /* address of a global: proves shared memory */
} Task;

static int g_shared = 42;
static atomic_int g_go;        /* threads park on this so we can count them */
static atomic_int g_parked;

static void *sum_range(void *arg)
{
    Task *t = arg;
    int local = t->id; /* lives on THIS thread's stack */
    t->stack_addr = (uintptr_t)&local;
    t->global_addr = &g_shared;
    t->tid = gettid(); /* glibc >= 2.30; else syscall(SYS_gettid) */

    char name[16]; /* kernel limit: 15 chars + NUL */
    snprintf(name, sizeof name, "summer-%d", t->id);
    pthread_setname_np(pthread_self(), name); /* shows in top -H, /proc */

    atomic_fetch_add(&g_parked, 1);
    while (!atomic_load(&g_go)) /* wait until main has looked at /proc */
        sleep_ms(1);

    uint64_t s = 0;
    for (uint64_t i = t->from; i < t->to; i++)
        s += i;
    t->sum = s; /* safe: main reads it only AFTER pthread_join */

    /* Second way to return: a heap pointer through pthread_join.
     * Ownership passes to the joiner, which must free() it. Never return
     * the address of a local — the stack is gone when the thread ends. */
    uint64_t *ret = malloc(sizeof *ret);
    if (ret)
        *ret = s;
    return ret;
}

static int count_tasks(void)
{
    DIR *d = opendir("/proc/self/task"); /* one directory per thread */
    CHECK(d != NULL);
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}

static void read_comm(pid_t tid, char *out, size_t len)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/comm", (int)tid);
    FILE *f = fopen(path, "r");
    CHECK(f != NULL);
    CHECK(fgets(out, (int)len, f) != NULL);
    fclose(f);
    out[strcspn(out, "\n")] = '\0';
}

static void demo_create_join(void)
{
    printf("[1] %d threads sum 1..1000000 in slices; shared vs private memory\n",
           NTHREADS);

    const uint64_t N = 1000000;
    Task tasks[NTHREADS];
    pthread_t th[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        tasks[i] = (Task){.id = i,
                          .from = N * (uint64_t)i / NTHREADS + 1,
                          .to = N * (uint64_t)(i + 1) / NTHREADS + 1};
        /* pthread_* functions return an error NUMBER; they do not set errno */
        int rc = pthread_create(&th[i], NULL, sum_range, &tasks[i]);
        CHECK(rc == 0);
    }

    /* All threads parked: look at them through /proc. Sanitizer runtimes
     * may add helper threads, so check ">=" and print the real number. */
    while (atomic_load(&g_parked) < NTHREADS)
        sleep_ms(1);
    int ntask = count_tasks();
    printf("    /proc/self/task has %d entries (main + %d workers%s)\n", ntask,
           NTHREADS, ntask > NTHREADS + 1 ? " + sanitizer helpers" : "");
    CHECK(ntask >= NTHREADS + 1);

    char comm[32];
    read_comm(tasks[2].tid, comm, sizeof comm);
    printf("    /proc/self/task/%d/comm = \"%s\"\n", (int)tasks[2].tid, comm);
    CHECK(strcmp(comm, "summer-2") == 0);

    atomic_store(&g_go, 1);

    uint64_t total = 0;
    for (int i = 0; i < NTHREADS; i++) {
        void *ret;
        CHECK(pthread_join(th[i], &ret) == 0); /* waits, then frees the
                                                  thread's resources */
        CHECK(ret != NULL);
        CHECK(*(uint64_t *)ret == tasks[i].sum);
        free(ret); /* we own it now */
        total += tasks[i].sum;
    }
    printf("    total = %llu (expected %llu)\n", (unsigned long long)total,
           (unsigned long long)(N * (N + 1) / 2));
    CHECK(total == N * (N + 1) / 2);

    /* Shared: everyone saw the same global. Private: every stack differs.
     * Same PID for all, different TIDs; main's TID equals the PID. */
    printf("    pid %d (main tid %d)\n", (int)getpid(), (int)gettid());
    CHECK(gettid() == getpid());
    for (int i = 0; i < NTHREADS; i++) {
        printf("    thread %d: tid %d  &local 0x%jx  &g_shared %p\n", i,
               (int)tasks[i].tid, (uintmax_t)tasks[i].stack_addr,
               (void *)tasks[i].global_addr);
        CHECK(tasks[i].global_addr == &g_shared);
        CHECK(tasks[i].tid != getpid());
        for (int j = 0; j < i; j++) {
            CHECK(tasks[i].stack_addr != tasks[j].stack_addr);
            CHECK(tasks[i].tid != tasks[j].tid);
        }
    }
}

/* ===== 2. attributes: stack size, detached ============================= */

static void *report_stack(void *arg)
{
    size_t *out = arg;
    pthread_attr_t a;
    /* pthread_getattr_np: the REAL attributes of a running thread */
    CHECK(pthread_getattr_np(pthread_self(), &a) == 0);
    CHECK(pthread_attr_getstacksize(&a, out) == 0);
    pthread_attr_destroy(&a);
    return NULL;
}

static atomic_int g_detached_done;

static void *detached_worker(void *arg)
{
    (void)arg;
    /* Nobody will join this thread. When it returns, the kernel task and
     * its stack are freed automatically. Its result must go somewhere
     * shared — here an atomic flag. */
    atomic_store(&g_detached_done, 1);
    return NULL;
}

static void demo_attributes(void)
{
    printf("\n[2] thread attributes: stack size and detach state\n");

    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr) == 0);

    size_t def;
    CHECK(pthread_attr_getstacksize(&attr, &def) == 0);
    /* Default = `ulimit -s` (usually 8 MiB of VIRTUAL memory per thread).
     * On a small board with many threads, shrink it. */
    const size_t want = 256 * 1024;
    CHECK(pthread_attr_setstacksize(&attr, want) == 0);

    pthread_t t;
    size_t got = 0;
    CHECK(pthread_create(&t, &attr, report_stack, &got) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    printf("    default stack %zu KiB, requested %zu KiB, thread got %zu KiB\n",
           def / 1024, want / 1024, got / 1024);
    CHECK(got >= want);

    /* Detached at birth: no join possible (or needed). Calling
     * pthread_join on it would be undefined behaviour. */
    CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    CHECK(pthread_create(&t, &attr, detached_worker, NULL) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);

    for (int i = 0; i < 2000 && !atomic_load(&g_detached_done); i++)
        sleep_ms(1); /* bounded wait: max 2 s */
    printf("    detached thread finished: %s\n",
           atomic_load(&g_detached_done) ? "yes" : "NO");
    CHECK(atomic_load(&g_detached_done));
}

/* ===== 3. pthread_self / pthread_equal ================================= */

static void *return_self(void *arg)
{
    *(pthread_t *)arg = pthread_self();
    return NULL;
}

static void demo_ids(void)
{
    printf("\n[3] pthread_t vs TID\n");
    pthread_t t, seen;
    CHECK(pthread_create(&t, NULL, return_self, &seen) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    /* pthread_t is an opaque handle (on glibc: the address of the thread's
     * control block). Compare with pthread_equal, never with ==. The TID is
     * the KERNEL's id — what you see in top -H, ps -L, /proc/<pid>/task. */
    CHECK(pthread_equal(t, seen));
    CHECK(!pthread_equal(t, pthread_self()));
    printf("    pthread_equal(created handle, thread's own pthread_self()) = 1\n");
}

int main(void)
{
    demo_create_join();
    demo_attributes();
    demo_ids();
    printf("\n02_pthread_create_join: all checks passed\n");
    return 0;
}
