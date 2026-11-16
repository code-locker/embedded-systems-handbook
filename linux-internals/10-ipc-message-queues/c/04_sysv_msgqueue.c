/*
 * 04_sysv_msgqueue.c — System V message queues: ftok, msgget, msgsnd,
 *                      msgrcv with type selection (0, >0, <0, MSG_EXCEPT),
 *                      msgctl(IPC_STAT / IPC_RMID), request/reply by pid
 *
 * Concept : The older (1983) Unix IPC. A queue is identified by a numeric
 *           KEY (often made with ftok(path, id)) and has an integer ID.
 *           Each message starts with a `long mtype` > 0, and msgrcv can
 *           SELECT by type — the feature POSIX mq does not have:
 *               msgtyp == 0  : first message in the queue (FIFO)
 *               msgtyp  > 0  : first message of exactly that type
 *               msgtyp  < 0  : first message with the LOWEST type <= |msgtyp|
 *           The queue persists in the kernel until IPC_RMID (or reboot):
 *           a crashed program leaves it behind — see `ipcs -q` / `ipcrm`.
 * Build   : make run P=linux-internals/10-ipc-message-queues/c/04_sysv_msgqueue.c
 * Read    : ../README.md  (section "3.4 System V message queues")
 */
#define _GNU_SOURCE /* MSG_EXCEPT */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
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

/* The message buffer: `long mtype` first, then any payload. The size you
 * pass to msgsnd/msgrcv is the PAYLOAD size (without mtype) — passing
 * sizeof(struct) is a classic off-by-8 bug. */
struct text_msg {
    long mtype;
    char text[32];
};

#define TEXT_SZ (sizeof(struct text_msg) - sizeof(long))

/* Request/reply: requests go with mtype 1; each client puts its pid in the
 * request, and the server answers with mtype = that pid. Every client then
 * receives ONLY its own replies from the one shared queue. */
#define MTYPE_REQUEST 1
struct req_msg {
    long mtype;
    pid_t client;
    int value;
};
#define REQ_SZ (sizeof(struct req_msg) - sizeof(long))
#define N_CLIENTS 3
#define N_REQS 20

static void snd(int qid, long type, const char *text)
{
    struct text_msg m = {.mtype = type};
    snprintf(m.text, sizeof m.text, "%s", text);
    CHECK(msgsnd(qid, &m, TEXT_SZ, IPC_NOWAIT) == 0);
}

static long rcv(int qid, long type, int flags, char *out)
{
    struct text_msg m;
    ssize_t n = msgrcv(qid, &m, TEXT_SZ, type, flags | IPC_NOWAIT);
    if (n < 0)
        return -1;
    strcpy(out, m.text);
    return m.mtype;
}

int main(void)
{
    alarm(10);

    /* ---- key: ftok(path, proj_id) ----
     * ftok mixes the file's inode + device numbers with proj_id: every
     * program that agrees on (path, id) gets the same key. Collisions are
     * possible (only low bits are used) — IPC_EXCL makes us notice. Using
     * our own mkstemp file keeps the key unique-ish per run. */
    char keyfile[] = "/tmp/sysv_mq_key_XXXXXX";
    int kfd = mkstemp(keyfile);
    CHECK(kfd >= 0);
    close(kfd);
    key_t key = ftok(keyfile, 'Q');
    CHECK(key != -1);
    int qid = msgget(key, IPC_CREAT | IPC_EXCL | 0600);
    if (qid < 0 && errno == EEXIST) {
        /* Someone else owns that key: fall back to a private queue (only
         * reachable through the id, e.g. by fork children). */
        printf("ftok key 0x%x already in use, using IPC_PRIVATE\n",
               (unsigned)key);
        qid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
    }
    CHECK(qid >= 0);
    printf("ftok(\"%s\", 'Q') = 0x%08x -> msgget id %d  (try: ipcs -q)\n",
           keyfile, (unsigned)key, qid);

    /* ---- 1. type selection ----
     * queue (FIFO order):  [3:"c1"] [1:"a1"] [2:"b1"] [3:"c2"] [1:"a2"]  */
    snd(qid, 3, "c1");
    snd(qid, 1, "a1");
    snd(qid, 2, "b1");
    snd(qid, 3, "c2");
    snd(qid, 1, "a2");

    char t[32];
    long ty;
    ty = rcv(qid, 0, 0, t); /* 0: oldest message of any type */
    printf("1) msgrcv(type 0)  -> type %ld \"%s\"   (FIFO head)\n", ty, t);
    CHECK(ty == 3 && strcmp(t, "c1") == 0);
    ty = rcv(qid, 2, 0, t); /* >0: first of exactly type 2 */
    printf("   msgrcv(type 2)  -> type %ld \"%s\"   (exact type)\n", ty, t);
    CHECK(ty == 2 && strcmp(t, "b1") == 0);
    ty = rcv(qid, -3, 0, t); /* <0: lowest type <= 3 -> a type 1 first */
    printf("   msgrcv(type -3) -> type %ld \"%s\"   (lowest type <= 3: a "
           "priority queue!)\n", ty, t);
    CHECK(ty == 1 && strcmp(t, "a1") == 0);
    ty = rcv(qid, 1, MSG_EXCEPT, t); /* Linux: first NOT of type 1 */
    printf("   msgrcv(1, MSG_EXCEPT) -> type %ld \"%s\"   (any type but 1)\n",
           ty, t);
    CHECK(ty == 3 && strcmp(t, "c2") == 0);
    ty = rcv(qid, 1, 0, t);
    CHECK(ty == 1 && strcmp(t, "a2") == 0);
    errno = 0;
    CHECK(rcv(qid, 0, 0, t) == -1 && errno == ENOMSG); /* empty + NOWAIT */
    printf("   empty queue + IPC_NOWAIT -> ENOMSG (POSIX mq says EAGAIN)\n");

    /* Payload too big for the receive buffer -> E2BIG, unless MSG_NOERROR
     * (then it is silently truncated). */
    snd(qid, 5, "this text is longer than 8");
    struct {
        long mtype;
        char text[8];
    } small;
    errno = 0;
    CHECK(msgrcv(qid, &small, sizeof small.text, 0, IPC_NOWAIT) == -1 &&
          errno == E2BIG);
    CHECK(msgrcv(qid, &small, sizeof small.text, 0,
                 IPC_NOWAIT | MSG_NOERROR) == (ssize_t)sizeof small.text);
    printf("   too-small buffer -> E2BIG; with MSG_NOERROR -> truncated "
           "\"%.8s\"\n", small.text);

    /* ---- 2. one server, several clients, ONE queue: reply by mtype=pid
     *
     *   client A --(type 1, A)--+               +--(type A)--> client A
     *   client B --(type 1, B)--+--> [ queue ]--+--(type B)--> client B
     *   client C --(type 1, C)--+    server reads type 1, replies type pid
     */
    pid_t kids[N_CLIENTS];
    fflush(stdout);
    for (int c = 0; c < N_CLIENTS; c++) {
        kids[c] = fork();
        CHECK(kids[c] >= 0);
        if (kids[c] == 0) {
            alarm(5);
            pid_t me = getpid();
            for (int i = 0; i < N_REQS; i++) {
                struct req_msg rq = {.mtype = MTYPE_REQUEST, .client = me,
                                     .value = c * 1000 + i};
                CHECK(msgsnd(qid, &rq, REQ_SZ, 0) == 0);
                struct req_msg rp;
                /* Blocking receive of MY type only. */
                ssize_t n;
                do
                    n = msgrcv(qid, &rp, REQ_SZ, (long)me, 0);
                while (n < 0 && errno == EINTR);
                CHECK(n == (ssize_t)REQ_SZ);
                CHECK(rp.client == me && rp.value == 2 * (c * 1000 + i));
            }
            _exit(0);
        }
    }
    /* Server: answer every request (value * 2) to mtype = client pid. */
    for (int served = 0; served < N_CLIENTS * N_REQS; served++) {
        struct req_msg rq;
        ssize_t n;
        do
            n = msgrcv(qid, &rq, REQ_SZ, MTYPE_REQUEST, 0);
        while (n < 0 && errno == EINTR);
        CHECK(n == (ssize_t)REQ_SZ);
        struct req_msg rp = {.mtype = rq.client, .client = rq.client,
                             .value = rq.value * 2};
        CHECK(msgsnd(qid, &rp, REQ_SZ, 0) == 0);
    }
    for (int c = 0; c < N_CLIENTS; c++) {
        int st;
        CHECK(waitpid(kids[c], &st, 0) == kids[c]);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    printf("2) %d clients x %d requests through ONE queue; every client "
           "got only its own replies (mtype = pid)\n", N_CLIENTS, N_REQS);

    /* ---- 3. IPC_STAT and IPC_RMID ---- */
    struct msqid_ds ds;
    CHECK(msgctl(qid, IPC_STAT, &ds) == 0);
    printf("3) IPC_STAT: msg_qnum=%lu msg_qbytes=%lu (max bytes in queue, "
           "default /proc/sys/kernel/msgmnb)\n",
           (unsigned long)ds.msg_qnum, (unsigned long)ds.msg_qbytes);
    CHECK(ds.msg_qnum == 0);
    /* IPC_RMID destroys the queue IMMEDIATELY — blocked readers/writers get
     * EIDRM. (Unlike POSIX mq_unlink, which waits for the last close.) */
    CHECK(msgctl(qid, IPC_RMID, NULL) == 0);
    struct text_msg late = {.mtype = 1, .text = "too late"};
    errno = 0;
    CHECK(msgsnd(qid, &late, TEXT_SZ, IPC_NOWAIT) == -1 &&
          (errno == EINVAL || errno == EIDRM)); /* the id is invalid now */
    printf("   IPC_RMID: queue %d destroyed (gone from `ipcs -q`)\n", qid);
    CHECK(unlink(keyfile) == 0);

    printf("04_sysv_msgqueue: all checks passed\n");
    return 0;
}
