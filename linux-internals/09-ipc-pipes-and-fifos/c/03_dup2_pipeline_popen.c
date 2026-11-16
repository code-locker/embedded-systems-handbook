/*
 * 03_dup2_pipeline_popen.c — dup2() redirection, a hand-built shell pipeline
 *                            (`ls DIR | wc -l`) with fork+exec, and popen()
 *
 * Concept : A program does not know it writes into a pipe: it writes to fd 1
 *           (stdout). The shell builds `a | b` by creating a pipe, then in
 *           each child dup2()-ing a pipe end ONTO fd 0 or fd 1 before exec.
 *           popen() is the libc shortcut: it runs `/bin/sh -c cmd` with one
 *           pipe and gives you a FILE*.
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/03_dup2_pipeline_popen.c
 * Read    : ../README.md  (section "3.3 dup2, pipelines and popen")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

#define N_FILES 7

static int wait_status(pid_t pid)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    return st;
}

/* Read everything from fd until EOF into buf (NUL-terminated). */
static size_t read_all(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buf + total, cap - 1 - total);
        if (n < 0 && errno == EINTR)
            continue;
        CHECK(n >= 0);
        if (n == 0)
            break;
        total += (size_t)n;
        CHECK(total < cap - 1);
    }
    buf[total] = '\0';
    return total;
}

int main(void)
{
    /* Watchdog for the whole test. A pending alarm is NOT inherited by a
     * fork() child, so each child sets its own (and it survives exec). */
    alarm(10);

    /* ---- 1. dup2: make a child's stdout BE the pipe ----
     *
     *   before dup2 in child:          after dup2(fd[1], 1); close(fd[1]):
     *     fd 1 -> terminal               fd 1 -> pipe write end
     *     fd 4 -> pipe write end         fd 4    (closed)
     *
     * dup2(old, new) closes `new` if open, then makes `new` refer to the
     * same open file as `old`. Then exec keeps fds 0/1/2, so `echo` writes
     * into our pipe without knowing it. */
    int fd[2];
    CHECK(pipe(fd) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(fd[0]);
        if (dup2(fd[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(fd[1]); /* fd 1 is the only write end the child needs */
        execlp("echo", "echo", "hello through dup2", (char *)NULL);
        _exit(127); /* only reached if exec failed */
    }
    close(fd[1]);
    char buf[4096];
    read_all(fd[0], buf, sizeof buf);
    close(fd[0]);
    int st = wait_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("1) child's stdout captured via dup2: \"%.*s\"\n",
           (int)strcspn(buf, "\n"), buf);
    CHECK(strcmp(buf, "hello through dup2\n") == 0);

    /* Make a directory with a known number of files for the pipeline. */
    char dir[] = "/tmp/pipes_demo_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    for (int i = 0; i < N_FILES; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s/file%d", dir, i);
        int f = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        CHECK(f >= 0);
        close(f);
    }

    /* ---- 2. the pipeline  ls DIR | wc -l  , built by hand ----
     *
     *              p1                        p2
     *   [ls] fd1 ======> fd0 [wc -l] fd1 ======> parent reads result
     *
     * Three processes, two pipes. Each process closes every pipe end it
     * does not use; otherwise wc never sees EOF (02_pipe_eof_bug.c). */
    int p1[2], p2[2];
    CHECK(pipe(p1) == 0);
    CHECK(pipe(p2) == 0);
    fflush(stdout);

    pid_t ls_pid = fork();
    CHECK(ls_pid >= 0);
    if (ls_pid == 0) {
        alarm(5);
        if (dup2(p1[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        execlp("ls", "ls", dir, (char *)NULL);
        _exit(127);
    }
    pid_t wc_pid = fork();
    CHECK(wc_pid >= 0);
    if (wc_pid == 0) {
        alarm(5);
        if (dup2(p1[0], STDIN_FILENO) < 0 || dup2(p2[1], STDOUT_FILENO) < 0)
            _exit(126);
        close(p1[0]); close(p1[1]); close(p2[0]); close(p2[1]);
        execlp("wc", "wc", "-l", (char *)NULL);
        _exit(127);
    }
    /* Parent: keeps only p2[0]. Closing p1[1] here matters: if the parent
     * kept it, wc would wait forever for more input. */
    close(p1[0]); close(p1[1]); close(p2[1]);
    read_all(p2[0], buf, sizeof buf);
    close(p2[0]);
    st = wait_status(ls_pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    st = wait_status(wc_pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    long count = strtol(buf, NULL, 10);
    printf("2) hand-built `ls %s | wc -l` -> %ld\n", dir, count);
    CHECK(count == N_FILES);

    /* ---- 3. popen(): the same with one call ----
     * popen forks, runs `/bin/sh -c cmd`, and connects the child's stdout
     * (mode "r") or stdin (mode "w") to a pipe wrapped in a FILE*. pclose()
     * WAITS for the child and returns its wait status — always pclose, never
     * fclose, or you leave a zombie. Never pass untrusted text into the
     * command string: it goes through the shell (injection). */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "ls %s | wc -l", dir);
    FILE *pf = popen(cmd, "r");
    CHECK(pf != NULL);
    long count2 = -1;
    CHECK(fscanf(pf, "%ld", &count2) == 1);
    st = pclose(pf);
    CHECK(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    printf("3) popen(\"%s\") -> %ld\n", cmd, count2);
    CHECK(count2 == N_FILES);

    /* popen in "w" mode: we write, the command reads its stdin. */
    char outpath[96];
    snprintf(outpath, sizeof outpath, "%s/upper.txt", dir);
    snprintf(cmd, sizeof cmd, "tr a-z A-Z > %s", outpath);
    pf = popen(cmd, "w");
    CHECK(pf != NULL);
    fputs("shout this\n", pf);
    st = pclose(pf); /* closes the pipe -> tr sees EOF, finishes, is reaped */
    CHECK(st != -1 && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    int f = open(outpath, O_RDONLY | O_CLOEXEC);
    CHECK(f >= 0);
    read_all(f, buf, sizeof buf);
    close(f);
    printf("3) popen(\"tr a-z A-Z\", \"w\") wrote: %s", buf);
    CHECK(strcmp(buf, "SHOUT THIS\n") == 0);

    /* Cleanup the temp directory. */
    for (int i = 0; i < N_FILES; i++) {
        char path[64];
        snprintf(path, sizeof path, "%s/file%d", dir, i);
        CHECK(unlink(path) == 0);
    }
    CHECK(unlink(outpath) == 0);
    CHECK(rmdir(dir) == 0);

    printf("03_dup2_pipeline_popen: all checks passed\n");
    return 0;
}
