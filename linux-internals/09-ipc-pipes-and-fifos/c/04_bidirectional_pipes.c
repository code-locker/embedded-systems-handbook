/*
 * 04_bidirectional_pipes.c — two pipes = one request/reply channel between a
 *                            parent (client) and a child (server)
 *
 * Concept : A pipe is ONE-way. For a conversation you need two: one for
 *           requests (parent -> child) and one for replies (child -> parent).
 *           Wrapping the fds in FILE* (fdopen) gives line-based I/O, but
 *           stdio BUFFERS output: if the client forgets fflush() its request
 *           sits in its own buffer, the server waits for it, the client
 *           waits for the reply -> both block forever (a deadlock).
 * Build   : make run P=linux-internals/09-ipc-pipes-and-fifos/c/04_bidirectional_pipes.c
 * Read    : ../README.md  (section "3.4 Two pipes for request/reply")
 */
#define _GNU_SOURCE
#include <errno.h>
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

static void wait_ok(pid_t pid)
{
    int st;
    pid_t r;
    do
        r = waitpid(pid, &st, 0);
    while (r < 0 && errno == EINTR);
    CHECK(r == pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

/* The server: reads "OP a b\n" lines, answers "= result\n" lines, until
 * "QUIT" or EOF. It is a tiny protocol; real ones (e.g. the git
 * pkt-line protocol over pipes to ssh) work the same way. */
static int server(FILE *in, FILE *out)
{
    char line[128];
    int served = 0;
    while (fgets(line, sizeof line, in)) {
        char op[8];
        long a, b;
        if (strncmp(line, "QUIT", 4) == 0)
            break;
        if (sscanf(line, "%7s %ld %ld", op, &a, &b) != 3) {
            fprintf(out, "ERR bad request\n");
        } else if (strcmp(op, "ADD") == 0) {
            fprintf(out, "= %ld\n", a + b);
        } else if (strcmp(op, "MUL") == 0) {
            fprintf(out, "= %ld\n", a * b);
        } else {
            fprintf(out, "ERR unknown op %s\n", op);
        }
        /* Flush after EVERY reply: the client is waiting for it now. */
        fflush(out);
        served++;
    }
    return served;
}

int main(void)
{
    /*
     *            req[1] ---- requests ---->  req[0]
     *   PARENT                                        CHILD
     *  (client)  rep[0] <---- replies -----  rep[1]  (server)
     */
    int req[2], rep[2];
    CHECK(pipe(req) == 0);
    CHECK(pipe(rep) == 0);

    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(req[1]); /* server never writes requests */
        close(rep[0]); /* server never reads replies   */
        FILE *in = fdopen(req[0], "r");
        FILE *out = fdopen(rep[1], "w");
        CHECK(in && out);
        int served = server(in, out);
        fclose(in); /* fclose closes the underlying fd too */
        fclose(out);
        _exit(served == 4 ? 0 : 2);
    }

    alarm(5);
    close(req[0]);
    close(rep[1]);
    FILE *to_srv = fdopen(req[1], "w");
    FILE *from_srv = fdopen(rep[0], "r");
    CHECK(to_srv && from_srv);

    struct {
        const char *req;
        const char *want;
    } cases[] = {
        {"ADD 2 3", "= 5"},
        {"MUL 6 7", "= 42"},
        {"ADD -10 4", "= -6"},
        {"DIV 1 0", "ERR unknown op DIV"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        fprintf(to_srv, "%s\n", cases[i].req);
        /* Without this fflush the request stays in OUR buffer -> deadlock:
         * we block in fgets() below, the server blocks in its fgets(). */
        fflush(to_srv);
        char reply[128];
        CHECK(fgets(reply, sizeof reply, from_srv) != NULL);
        reply[strcspn(reply, "\n")] = '\0';
        printf("client: %-10s -> server: %s\n", cases[i].req, reply);
        CHECK(strcmp(reply, cases[i].want) == 0);
    }
    fprintf(to_srv, "QUIT\n");
    fclose(to_srv);   /* also gives the server EOF if it missed QUIT */
    char extra[16];
    CHECK(fgets(extra, sizeof extra, from_srv) == NULL); /* server closed */
    fclose(from_srv);
    wait_ok(pid);

    printf("04_bidirectional_pipes: all checks passed\n");
    return 0;
}
