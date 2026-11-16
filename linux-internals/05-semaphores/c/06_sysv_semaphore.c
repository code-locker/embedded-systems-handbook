/*
 * 06_sysv_semaphore.c — System V semaphores: semget / semop / semctl,
 *                       atomic operations on a SET, and SEM_UNDO
 *
 * Concept : The older (1983, UNIX System V) API, still everywhere in legacy
 *           code, databases (PostgreSQL used them for years) and `ipcs`.
 *           Differences from POSIX semaphores:
 *             - you get a SET of semaphores (semget(key, nsems, flags))
 *             - one semop() call applies several operations to the set
 *               ATOMICALLY: all of them, or (blocking) none
 *             - SEM_UNDO: the kernel remembers your changes and REVERSES
 *               them when your process exits — even on a crash. A lock held
 *               by a dead process is released automatically.
 *             - every operation is a syscall (slower than POSIX's futex
 *               fast path); objects persist until IPC_RMID or reboot
 * Build   : make run P=linux-internals/05-semaphores/c/06_sysv_semaphore.c
 *           make run P=linux-internals/05-semaphores/c/06_sysv_semaphore.c SAN=tsan
 * Read    : ../README.md  (section 3.8 "System V semaphores")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* The CALLER must define this union (man 2 semctl) — a famous oddity. */
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

static int g_semid = -1;
static pid_t g_parent;

static void cleanup(void)
{
    /* SysV objects outlive the process: forgetting IPC_RMID leaks one
     * until reboot (see `ipcs -s`, remove by hand with `ipcrm -s <id>`). */
    if (getpid() == g_parent && g_semid >= 0)
        semctl(g_semid, 0, IPC_RMID);
}

static int getval(int idx) { return semctl(g_semid, idx, GETVAL); }

static void setval(int idx, int v)
{
    union semun a = {.val = v};
    CHECK(semctl(g_semid, idx, SETVAL, a) == 0);
}

/* Run fn in a child process and wait for it. */
static void in_child(void (*fn)(void))
{
    fflush(stdout);
    pid_t p = fork();
    CHECK(p >= 0);
    if (p == 0) {
        fn();
        _exit(0);
    }
    int st;
    CHECK(waitpid(p, &st, 0) == p);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* Takes semaphore 0 WITH SEM_UNDO and exits without giving it back. */
static void child_take_with_undo(void)
{
    struct sembuf op = {.sem_num = 0, .sem_op = -1, .sem_flg = SEM_UNDO};
    if (semop(g_semid, &op, 1) != 0)
        _exit(2);
    /* "crash" while holding it */
}

/* Same, without SEM_UNDO. */
static void child_take_no_undo(void)
{
    struct sembuf op = {.sem_num = 0, .sem_op = -1, .sem_flg = 0};
    if (semop(g_semid, &op, 1) != 0)
        _exit(2);
}

int main(void)
{
    g_parent = getpid();
    CHECK(atexit(cleanup) == 0);

    /* IPC_PRIVATE: a new set nobody else can find by key — perfect for a
     * parent and its children, and unique per run. Unrelated processes
     * would agree on a key from ftok(path, id) instead. */
    g_semid = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
    CHECK(g_semid >= 0);
    setval(0, 1); /* sem 0: a lock (1 = free) */
    setval(1, 2); /* sem 1: a pool of 2 buffers */
    printf("[0] semget(IPC_PRIVATE, 2 sems) -> id %d; sem0=%d sem1=%d\n",
           g_semid, getval(0), getval(1));

    /* --- 1. SEM_UNDO: the kernel cleans up after a dead holder ---------- */
    in_child(child_take_with_undo);
    printf("[1] child took sem0 WITH SEM_UNDO and exited holding it -> "
           "sem0 = %d (kernel undid the -1)\n", getval(0));
    CHECK(getval(0) == 1);

    in_child(child_take_no_undo);
    printf("    child took sem0 WITHOUT SEM_UNDO and exited -> sem0 = %d "
           "(lost forever; a later locker would hang)\n", getval(0));
    CHECK(getval(0) == 0);
    setval(0, 1); /* repair by hand */

    /* --- 2. IPC_NOWAIT = trywait ---------------------------------------- */
    struct sembuf take_lock = {0, -1, SEM_UNDO};
    CHECK(semop(g_semid, &take_lock, 1) == 0);
    struct sembuf try_again = {0, -1, IPC_NOWAIT};
    errno = 0;
    CHECK(semop(g_semid, &try_again, 1) == -1 && errno == EAGAIN);
    printf("[2] sem0 held; semop with IPC_NOWAIT -> EAGAIN\n");
    struct sembuf give_lock = {0, +1, SEM_UNDO};
    CHECK(semop(g_semid, &give_lock, 1) == 0);

    /* --- 3. Atomic multi-semaphore operation ---------------------------- */
    /* "Take the lock AND 2 buffers" as ONE operation: either all three
     * decrements happen, or none. With POSIX semaphores you would take them
     * one by one and could deadlock / hold one while waiting for another
     * (hold-and-wait, lesson 03). */
    struct sembuf both[2] = {{0, -1, SEM_UNDO}, {1, -2, SEM_UNDO}};
    CHECK(semop(g_semid, both, 2) == 0);
    printf("[3] one semop(): lock -1 and pool -2 atomically -> sem0=%d "
           "sem1=%d\n", getval(0), getval(1));
    CHECK(getval(0) == 0 && getval(1) == 0);
    /* Now ask for lock + 1 buffer with IPC_NOWAIT: the pool is empty, so
     * NOTHING is taken, not even a partial result. */
    setval(0, 1); /* pretend the lock was freed, pool still 0 */
    struct sembuf partial[2] = {{0, -1, IPC_NOWAIT}, {1, -1, IPC_NOWAIT}};
    errno = 0;
    CHECK(semop(g_semid, partial, 2) == -1 && errno == EAGAIN);
    printf("    lock free but pool empty: combined semop -> EAGAIN and sem0 "
           "is still %d (all or nothing)\n", getval(0));
    CHECK(getval(0) == 1);

    /* Explicit removal (cleanup() would do it too). */
    CHECK(semctl(g_semid, 0, IPC_RMID) == 0);
    g_semid = -1;
    printf("[4] IPC_RMID done (check `ipcs -s`: nothing left behind)\n");
    printf("06_sysv_semaphore: all checks passed\n");
    return 0;
}
