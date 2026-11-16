/*
 * 08_message_per_read_bug.c — INTENTIONALLY BROKEN: assumes one read() on a
 *                             SOCK_STREAM socket returns exactly one message
 *
 * Concept : The most common Unix-socket (and TCP) bug. It "works" in a quick
 *           test where the reader happens to be fast, and breaks as soon as
 *           the writer gets ahead (load, a slow board, a debugger pause):
 *           several messages arrive glued together, or one message arrives
 *           in pieces. Here the writer finishes before the reader starts,
 *           which makes the failure deterministic.
 * Finds it: just run it — the program's own CHECK fails and it exits 1:
 *             make run P=linux-internals/12-ipc-unix-sockets/c/08_message_per_read_bug.c
 *           No sanitizer can see this: there is no memory error and no data
 *           race, only a wrong assumption about the protocol. Code review
 *           rule: every read() on a stream socket needs a framing story.
 * Fix     : length-prefix framing with read_full() (03_socketpair_framing.c),
 *           or use SOCK_SEQPACKET, which keeps message boundaries (02).
 * Build   : make run P=linux-internals/12-ipc-unix-sockets/c/08_message_per_read_bug.c
 * Read    : ../README.md  (section 7 "Common mistakes and pitfalls", item 1)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static const char *const CMDS[] = {"START", "SET gain=4", "STOP"};

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int sv[2];
    /* Change SOCK_STREAM to SOCK_SEQPACKET and the bug disappears. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { /* writer: three "messages", one write() each */
        close(sv[0]);
        for (int i = 0; i < 3; i++)
            if (write(sv[1], CMDS[i], strlen(CMDS[i])) != (ssize_t)strlen(CMDS[i]))
                _exit(1);
        _exit(0);
    }
    close(sv[1]);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid); /* writer is done: reader is "slow" */

    /* BUG: "each read() is one command" */
    for (int i = 0; i < 3; i++) {
        char buf[64];
        ssize_t n = read(sv[0], buf, sizeof buf - 1);
        CHECK(n >= 0);
        buf[n] = '\0';
        printf("command %d: expected \"%s\", got \"%s\"\n", i, CMDS[i], buf);
        CHECK(strcmp(buf, CMDS[i]) == 0); /* fails on the first command */
    }
    close(sv[0]);
    printf("08_message_per_read_bug: all checks passed (the bug did not show)\n");
    return 0;
}
