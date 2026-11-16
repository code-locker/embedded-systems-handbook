/*
 * 01_posix_mq_basics.c — POSIX message queues: mq_open with attributes,
 *                        priorities, timeouts, O_NONBLOCK, mq_getattr,
 *                        limits in /proc/sys/fs/mqueue, mq_unlink
 *
 * Concept : A message queue keeps MESSAGE BOUNDARIES (one send = one
 *           receive, never half or two glued together) and orders messages
 *           by PRIORITY (highest first, FIFO within one priority). The queue
 *           lives in the kernel, has a name ("/something"), and survives
 *           until mq_unlink() or reboot — even with no process using it.
 * Build   : make run P=linux-internals/10-ipc-message-queues/c/01_posix_mq_basics.c
 * Read    : ../README.md  (section "3.1 POSIX message queues")
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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

#define MAXMSG 8   /* <= /proc/sys/fs/mqueue/msg_max (10) for normal users */
#define MSGSIZE 64 /* <= /proc/sys/fs/mqueue/msgsize_max (8192)            */

static long read_long(const char *path)
{
    FILE *f = fopen(path, "r");
    long v = -1;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = -1;
        fclose(f);
    }
    return v;
}

/* Absolute deadline `ms` from now on CLOCK_REALTIME: mq_timed* use
 * absolute REALTIME timeouts (a wall-clock jump changes the wait; that is
 * a known wart of this API). */
static struct timespec deadline_ms(long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return ts;
}

int main(void)
{
    alarm(10);

    /* ---- limits ---- */
    printf("limits: msg_max=%ld msgsize_max=%ld queues_max=%ld "
           "(see /proc/sys/fs/mqueue/)\n",
           read_long("/proc/sys/fs/mqueue/msg_max"),
           read_long("/proc/sys/fs/mqueue/msgsize_max"),
           read_long("/proc/sys/fs/mqueue/queues_max"));

    /* ---- 1. create with explicit attributes ----
     * Name rules: starts with '/', no other '/'. Make it unique per run
     * (pid) so parallel runs and leftovers from crashed runs cannot clash.
     * O_EXCL: fail if it already exists instead of silently reusing it. */
    char name[64];
    snprintf(name, sizeof name, "/mq_basics_%d", (int)getpid());
    struct mq_attr attr = {.mq_maxmsg = MAXMSG, .mq_msgsize = MSGSIZE};
    mqd_t mq = mq_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600,
                       &attr);
    CHECK(mq != (mqd_t)-1);

    struct mq_attr got;
    CHECK(mq_getattr(mq, &got) == 0);
    printf("1) mq_open(\"%s\"): maxmsg=%ld msgsize=%ld curmsgs=%ld\n", name,
           got.mq_maxmsg, got.mq_msgsize, got.mq_curmsgs);
    CHECK(got.mq_maxmsg == MAXMSG && got.mq_msgsize == MSGSIZE);
    CHECK(got.mq_curmsgs == 0);

    /* On Linux the queue is visible as a file in the mqueue filesystem. */
    char fspath[96];
    snprintf(fspath, sizeof fspath, "/dev/mqueue%s", name);
    struct stat sb;
    if (stat(fspath, &sb) == 0) {
        FILE *f = fopen(fspath, "r");
        char info[160] = "";
        if (f) {
            if (!fgets(info, sizeof info, f))
                info[0] = '\0';
            fclose(f);
        }
        printf("   %s exists: %s", fspath, info);
    } else {
        printf("   /dev/mqueue is not mounted here (mount -t mqueue none "
               "/dev/mqueue)\n");
    }

    /* ---- 2. priorities ----
     * Send in a scrambled order; receive returns highest priority first,
     * FIFO among equal priorities.
     *
     *   send:    A(1) B(5) C(1) D(9) E(5)
     *   queue:   D(9) | B(5) E(5) | A(1) C(1)
     *   receive: D B E A C
     */
    struct {
        const char *text;
        unsigned prio;
    } sends[] = {{"A", 1}, {"B", 5}, {"C", 1}, {"D", 9}, {"E", 5}};
    for (size_t i = 0; i < 5; i++)
        CHECK(mq_send(mq, sends[i].text, strlen(sends[i].text) + 1,
                      sends[i].prio) == 0);
    CHECK(mq_getattr(mq, &got) == 0 && got.mq_curmsgs == 5);

    const char *want = "DBEAC";
    printf("2) sent A(1) B(5) C(1) D(9) E(5); received:");
    for (int i = 0; i < 5; i++) {
        /* The receive buffer MUST be >= mq_msgsize, else EMSGSIZE — even
         * if the next message is tiny. */
        char buf[MSGSIZE];
        unsigned prio;
        ssize_t n = mq_receive(mq, buf, sizeof buf, &prio);
        CHECK(n == 2); /* message boundaries kept: exactly what was sent */
        printf(" %s(%u)", buf, prio);
        CHECK(buf[0] == want[i]);
    }
    printf("\n");

    /* Receive buffer too small -> EMSGSIZE (message stays queued). */
    CHECK(mq_send(mq, "x", 2, 0) == 0);
    char small[8];
    errno = 0;
    CHECK(mq_receive(mq, small, sizeof small, NULL) == -1 &&
          errno == EMSGSIZE);
    /* Sending more than msgsize -> EMSGSIZE too. */
    char big[MSGSIZE + 1];
    memset(big, 'b', sizeof big);
    errno = 0;
    CHECK(mq_send(mq, big, sizeof big, 0) == -1 && errno == EMSGSIZE);
    char buf[MSGSIZE];
    CHECK(mq_receive(mq, buf, sizeof buf, NULL) == 2);
    printf("   receive buffer < msgsize -> EMSGSIZE; message > msgsize -> "
           "EMSGSIZE\n");

    /* ---- 3. timeouts ----
     * mq_timedreceive on an empty queue waits until the deadline. */
    struct timespec dl = deadline_ms(100);
    errno = 0;
    CHECK(mq_timedreceive(mq, buf, sizeof buf, NULL, &dl) == -1 &&
          errno == ETIMEDOUT);
    printf("3) mq_timedreceive on empty queue -> ETIMEDOUT after 100 ms\n");

    /* Fill it up, then mq_timedsend on a full queue also times out. */
    for (int i = 0; i < MAXMSG; i++)
        CHECK(mq_send(mq, "f", 2, 0) == 0);
    dl = deadline_ms(100);
    errno = 0;
    CHECK(mq_timedsend(mq, "f", 2, 0, &dl) == -1 && errno == ETIMEDOUT);
    printf("   mq_timedsend on full queue (%d msgs) -> ETIMEDOUT\n", MAXMSG);

    /* ---- 4. O_NONBLOCK: EAGAIN instead of waiting ----
     * mq_setattr can only change mq_flags (O_NONBLOCK). maxmsg/msgsize are
     * fixed at creation. */
    struct mq_attr nb = {.mq_flags = O_NONBLOCK};
    CHECK(mq_setattr(mq, &nb, NULL) == 0);
    errno = 0;
    CHECK(mq_send(mq, "f", 2, 0) == -1 && errno == EAGAIN);
    for (int i = 0; i < MAXMSG; i++)
        CHECK(mq_receive(mq, buf, sizeof buf, NULL) == 2);
    errno = 0;
    CHECK(mq_receive(mq, buf, sizeof buf, NULL) == -1 && errno == EAGAIN);
    printf("4) O_NONBLOCK: send on full -> EAGAIN, receive on empty -> "
           "EAGAIN\n");

    /* ---- 5. persistence and unlink ----
     * Closing does NOT destroy the queue: another open finds the messages
     * still there. Only mq_unlink removes the name; the queue itself is
     * freed when the last descriptor is closed (same as unlink on files). */
    CHECK(mq_send(mq, "kept", 5, 3) == 0);
    CHECK(mq_close(mq) == 0);
    mq = mq_open(name, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    CHECK(mq != (mqd_t)-1);
    unsigned prio = 0;
    CHECK(mq_receive(mq, buf, sizeof buf, &prio) == 5);
    CHECK(strcmp(buf, "kept") == 0 && prio == 3);
    printf("5) after mq_close + re-open, the message \"%s\" was still "
           "there (kernel persistence)\n", buf);
    CHECK(mq_unlink(name) == 0);
    errno = 0;
    CHECK(mq_open(name, O_RDONLY) == (mqd_t)-1 && errno == ENOENT);
    CHECK(mq_close(mq) == 0);
    printf("   mq_unlink: the name is gone (ENOENT on open)\n");

    printf("01_posix_mq_basics: all checks passed\n");
    return 0;
}
