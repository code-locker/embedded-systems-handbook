/*
 * 02_pipe_eof_bug.c — INTENTIONAL BUG: the parent forgets to close the
 *                     write end of the pipe, so its read loop never sees EOF
 *                     and the program HANGS.
 *
 * Concept : read() on a pipe returns 0 (EOF) only when NO process holds an
 *           open write end. After fork() the parent still holds fd[1]. The
 *           child writes its lines and exits, but the parent's own copy of
 *           the write end keeps the pipe "alive": read() blocks forever,
 *           waiting for data that the parent itself might write.
 *
 *              child  fd[1] --close at exit--> X
 *              parent fd[1] -----still open-----+   <- the bug
 *                                               v
 *              parent fd[0] read() ... blocks forever, never returns 0
 *
 * Tool    : the hang itself, made visible with `timeout`:
 *             make build/linux-internals/09-ipc-pipes-and-fifos/c/02_pipe_eof_bug
 *             timeout 3 ./build/linux-internals/09-ipc-pipes-and-fifos/c/02_pipe_eof_bug
 *             echo $?        # 124 = killed by timeout -> it hung
 *           While it hangs, from another terminal:
 *             ls -l /proc/$(pgrep -n 02_pipe_eof)/fd   # shows BOTH pipe ends
 *             cat /proc/$(pgrep -n 02_pipe_eof)/stack  # (root) pipe_read
 *           `strace -f` shows the parent sitting in read(3, ...).
 *           (A safety alarm(20) kills it eventually in case you forget.)
 * Fix     : close(fd[1]) in the parent right after fork() — see
 *           01_pipe_fork.c. Not run by `make test` (it is a _bug file).
 * Read    : ../README.md  (section "3.2 EOF and closing unused ends")
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    int fd[2];
    if (pipe(fd) != 0) {
        perror("pipe");
        return 1;
    }
    alarm(20); /* safety net only; use `timeout 3` to see the hang quickly */

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        close(fd[0]);
        for (int i = 0; i < 3; i++) {
            char line[32];
            int len = snprintf(line, sizeof line, "line %d\n", i);
            if (write(fd[1], line, (size_t)len) != len)
                _exit(1);
        }
        close(fd[1]);
        _exit(0); /* child is gone; its write end is closed */
    }

    /* BUG: missing   close(fd[1]);   here. The parent only reads, but it
     * still owns a write end, so the kernel cannot report EOF. */

    char buf[64];
    ssize_t n;
    printf("parent: reading until EOF...\n");
    fflush(stdout);
    while ((n = read(fd[0], buf, sizeof buf)) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            return 1;
        }
        printf("parent got %zd bytes: %.*s", n, (int)n, buf);
        fflush(stdout);
    }
    /* Never reached: */
    printf("parent: EOF\n");
    waitpid(pid, NULL, 0);
    return 0;
}
