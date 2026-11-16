/*
 * 01_fork_exec_wait.c — processes: fork, exec, waitpid, exit status,
 *                       zombies, orphans, copy-on-write, shared fds
 *
 * Concept : fork() clones the calling process; the child gets a COPY of the
 *           address space (copy-on-write) but SHARES the open file
 *           descriptions. exec() replaces the program inside a process.
 *           waitpid() collects a child's exit status; until then the dead
 *           child is a zombie. A child whose parent dies is an orphan and is
 *           re-parented (here: to us, because we become a "subreaper").
 * Build   : make run P=linux-internals/01-processes-and-threads/c/01_fork_exec_wait.c
 * Read    : ../README.md  (sections 3.1 - 3.3)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
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

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/*
 * fork() + stdio: printf() output may still sit in the stdio buffer (always
 * when stdout is a pipe or a file, e.g. under `make test`). fork() copies
 * that buffer, so the text would be printed TWICE — once by each process.
 * Always fflush() before fork(). And in the child use _exit(), not exit():
 * exit() would flush the inherited buffer again and run atexit handlers
 * (including the sanitizer's leak checker) that belong to the parent.
 */
static pid_t fork_flushed(void)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    CHECK(pid != -1);
    return pid;
}

/* waitpid() that retries on EINTR (a signal can interrupt the wait). */
static int wait_for(pid_t pid)
{
    int status;
    pid_t r;
    do
        r = waitpid(pid, &status, 0);
    while (r == -1 && errno == EINTR);
    CHECK(r == pid);
    return status;
}

/* Process state letter from /proc/<pid>/stat: R, S, D, Z (zombie), ... */
static char proc_state(pid_t pid)
{
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return '?';
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* format: "pid (comm) S ..." — comm may contain spaces, so find the
     * LAST ')' and the state letter is two characters after it. */
    char *p = strrchr(buf, ')');
    return (p && p[1] == ' ') ? p[2] : '?';
}

/* ---- 1. exit status macros ------------------------------------------- */
static void demo_exit_status(void)
{
    printf("\n[1] exit status: normal exit vs killed by a signal\n");

    pid_t pid = fork_flushed();
    if (pid == 0)
        _exit(3); /* child: normal termination, status 3 */
    int st = wait_for(pid);
    /*
     * `st` is NOT the exit code. It is a packed word:
     *
     *    bits 15..8  exit code (if exited normally)
     *    bits  6..0  signal number (if killed), bit 7 = core dumped
     *
     * Always decode it with the W* macros.
     */
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);
    printf("    child %d: WIFEXITED, WEXITSTATUS = %d  (raw status 0x%04x)\n",
           (int)pid, WEXITSTATUS(st), st);

    pid = fork_flushed();
    if (pid == 0) {
        kill(getpid(), SIGTERM); /* child kills itself */
        _exit(99);               /* not reached */
    }
    st = wait_for(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    printf("    child %d: WIFSIGNALED, WTERMSIG = %d (%s)\n", (int)pid,
           WTERMSIG(st), strsignal(WTERMSIG(st)));
}

/* ---- 2. exec: replace the program, capture its stdout through a pipe -- */
static void demo_exec(void)
{
    printf("\n[2] fork + exec: run /bin/sh in the child, read its stdout\n");

    int pfd[2];
    CHECK(pipe(pfd) == 0);
    pid_t pid = fork_flushed();
    if (pid == 0) {
        /*
         *   child before exec:  fd 1 -> terminal / file
         *   dup2(pfd[1], 1):    fd 1 -> pipe write end
         *   exec():             the new program keeps its fds, so whatever
         *                       `sh` writes to stdout lands in our pipe.
         */
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        execlp("sh", "sh", "-c", "echo hello from exec; exit 7", (char *)NULL);
        _exit(127); /* only reached if exec failed (127 = "not found") */
    }
    close(pfd[1]); /* parent must close its write end, or read() never sees EOF */
    char buf[128];
    size_t len = 0;
    ssize_t n;
    while ((n = read(pfd[0], buf + len, sizeof buf - 1 - len)) > 0)
        len += (size_t)n;
    buf[len] = '\0';
    close(pfd[0]);

    int st = wait_for(pid);
    printf("    child said: \"%.*s\", exit code %d\n", (int)(len ? len - 1 : 0),
           buf, WEXITSTATUS(st));
    CHECK(strcmp(buf, "hello from exec\n") == 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7);
}

/* ---- 3. copy-on-write: memory is private after fork ----------------- */
static int g_value = 100;

static void demo_cow(void)
{
    printf("\n[3] copy-on-write: the child changes its copy, parent is unaffected\n");

    int *heap = malloc(sizeof *heap); /* owned by the parent; the child's
                                         copy dies with the child */
    CHECK(heap != NULL);
    *heap = 1;

    pid_t pid = fork_flushed();
    if (pid == 0) {
        /* Same VIRTUAL addresses as in the parent, but the first write
         * triggers a page fault and the kernel gives the child its own
         * physical copy of that page. */
        g_value = 200;
        *heap = 2;
        _exit(g_value == 200 && *heap == 2 ? 0 : 1);
    }
    int st = wait_for(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("    child wrote g_value=200 *heap=2; parent still sees g_value=%d *heap=%d"
           " (same address %p in both)\n", g_value, *heap, (void *)&g_value);
    CHECK(g_value == 100 && *heap == 1);
    free(heap);
}

/* ---- 4. fds are shared: one open file description, one offset ------- */
static void demo_shared_fd(void)
{
    printf("\n[4] fork shares open file descriptions (and their offset)\n");

    char path[] = "/tmp/li01_forkfd_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    unlink(path); /* the file lives on while fd is open; nothing to clean up */

    /*
     *   parent fd 3 ─┐
     *                ├──> [ open file description: offset, flags ] ──> inode
     *   child  fd 3 ─┘
     *
     * Both write through the SAME offset, so the parent's later write lands
     * after the child's text instead of overwriting it.
     */
    pid_t pid = fork_flushed();
    if (pid == 0) {
        ssize_t w = write(fd, "child\n", 6);
        _exit(w == 6 ? 0 : 1);
    }
    int st = wait_for(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(write(fd, "parent\n", 7) == 7);

    char buf[32] = {0};
    CHECK(pread(fd, buf, sizeof buf - 1, 0) == 13);
    printf("    file contents: \"child\\nparent\\n\"  offset now %ld\n",
           (long)lseek(fd, 0, SEEK_CUR));
    CHECK(strcmp(buf, "child\nparent\n") == 0);
    close(fd);
}

/* ---- 5. zombie: dead but not yet waited for ------------------------- */
static void demo_zombie(void)
{
    printf("\n[5] zombie: a child that exited but has not been waited for\n");

    pid_t pid = fork_flushed();
    if (pid == 0)
        _exit(0);

    /* Poll /proc until the kernel marks it Z (bounded: max ~2 s). */
    char s = '?';
    for (int i = 0; i < 200 && (s = proc_state(pid)) != 'Z'; i++)
        sleep_ms(10);
    printf("    /proc/%d/stat state = '%c'  (Z = zombie: only the exit status"
           " and pid are kept)\n", (int)pid, s);
    CHECK(s == 'Z');

    wait_for(pid); /* reaping frees the pid and the task struct */
    CHECK(kill(pid, 0) == -1 && errno == ESRCH);
    printf("    after waitpid(): pid %d is gone (kill(pid,0) -> ESRCH)\n", (int)pid);
}

/* ---- 6. orphan: parent dies first ----------------------------------- */
static void demo_orphan(void)
{
    printf("\n[6] orphan: the middle process exits, the grandchild is re-parented\n");

    /*
     * Normally an orphan goes to init (pid 1) or to the nearest ancestor
     * that declared itself a "child subreaper" (systemd --user does that).
     * We become a subreaper ourselves so the orphan comes to US — then we
     * can reap it and nothing is left behind.
     *
     *   us ──fork──> middle ──fork──> grandchild
     *                  │ _exit
     *                  x
     *   us <──────── re-parented ──── grandchild
     */
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 1) == 0);

    int pfd[2], sync[2];
    CHECK(pipe(pfd) == 0);
    CHECK(pipe(sync) == 0);
    pid_t middle = fork_flushed();
    if (middle == 0) {
        pid_t gc = fork(); /* stdio already flushed */
        if (gc == 0) {
            pid_t old_parent = getppid();
            /* Tell the middle process "I have seen you" — without this
             * handshake the middle could exit before we even ran getppid()
             * and we would never see the original parent. */
            ssize_t w = write(sync[1], "k", 1);
            /* wait (bounded) until the middle process is gone */
            for (int i = 0; i < 200 && getppid() == old_parent; i++)
                sleep_ms(10);
            pid_t pair[2] = {old_parent, getppid()};
            w += write(pfd[1], pair, sizeof pair);
            _exit(w == 1 + (ssize_t)sizeof pair ? 0 : 1);
        }
        close(sync[1]);
        char c;
        ssize_t r = read(sync[0], &c, 1); /* EOF too if the grandchild died */
        _exit(gc > 0 && r == 1 ? 0 : 1);  /* middle exits: orphan made */
    }
    close(sync[0]);
    close(sync[1]);
    close(pfd[1]);
    int st = wait_for(middle);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    pid_t pair[2];
    CHECK(read(pfd[0], pair, sizeof pair) == (ssize_t)sizeof pair);
    close(pfd[0]);
    printf("    grandchild's parent: %d (middle) -> %d (us, pid %d)\n",
           (int)pair[0], (int)pair[1], (int)getpid());
    CHECK(pair[0] == middle);
    CHECK(pair[1] == getpid());

    /* We are its parent now, so we must reap it. waitpid(-1) = any child. */
    pid_t r;
    do
        r = waitpid(-1, &st, 0);
    while (r == -1 && errno == EINTR);
    CHECK(r > 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(prctl(PR_SET_CHILD_SUBREAPER, 0) == 0);
}

int main(void)
{
    printf("parent pid %d, its parent %d\n", (int)getpid(), (int)getppid());
    demo_exit_status();
    demo_exec();
    demo_cow();
    demo_shared_fd();
    demo_zombie();
    demo_orphan();
    printf("\n01_fork_exec_wait: all checks passed\n");
    return 0;
}
