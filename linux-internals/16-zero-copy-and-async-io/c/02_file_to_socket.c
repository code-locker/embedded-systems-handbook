/*
 * 02_file_to_socket.c — serving a file over a socket: read+write vs
 *                       mmap+write vs sendfile vs splice; a receiver thread
 *                       checksums every byte
 *
 * Concept : The classic zero-copy use case is a web server, a video
 *           streamer or a firmware-update server sending a file to a
 *           socket. With read()+write() every byte is copied into a user
 *           buffer and back into the kernel. sendfile(sock, file) does it in
 *           one call with no user-space copy (on a real NIC with
 *           scatter-gather, the page-cache pages are DMA'd directly).
 *           splice(file -> pipe -> socket) is the general form. Here the
 *           "network" is an AF_UNIX socketpair, so the receiving side is in
 *           the same process: a thread reads exactly the expected number of
 *           bytes and checks the checksum for every method.
 * Build   : make run P=linux-internals/16-zero-copy-and-async-io/c/02_file_to_socket.c
 * Read    : ../README.md  (section 3.3 "File to socket: sendfile and splice")
 */
#define _GNU_SOURCE /* splice, pipe2, mkostemp */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d (%s)\n",     \
                    #cond, __FILE__, __LINE__, errno, strerror(errno));      \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define FILE_SIZE (4u << 20) /* 4 MiB */
#define CHUNK     (64u << 10)

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

/* FNV-1a over the whole buffer (byte-wise; computed outside the timing). */
static uint64_t fnv(uint64_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}
#define FNV_INIT 1469598103934665603ULL

struct receiver {
    int fd;               /* our end of the socketpair                */
    unsigned char *dst;   /* FILE_SIZE bytes, owned by main            */
    size_t got;           /* written by the thread, read after join    */
};

/* The receiver reads straight into its destination buffer (like a client
 * saving a download); hashing happens after the timed section. */
static void *receive(void *arg)
{
    struct receiver *r = arg;
    while (r->got < FILE_SIZE) {
        ssize_t n = read(r->fd, r->dst + r->got, FILE_SIZE - r->got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break; /* error or sender closed: got < FILE_SIZE -> CHECK fails */
        r->got += (size_t)n;
    }
    return NULL;
}

/* write all of buf to a stream socket (short writes happen) */
static int write_all(int fd, const unsigned char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int send_read_write(int file, int sock)
{
    static unsigned char buf[CHUNK];
    ssize_t n;
    while ((n = read(file, buf, sizeof buf)) > 0)
        if (write_all(sock, buf, (size_t)n) != 0)
            return -1;
    return n < 0 ? -1 : 0;
}

static int send_mmap_write(int file, int sock)
{
    unsigned char *p = mmap(NULL, FILE_SIZE, PROT_READ, MAP_PRIVATE, file, 0);
    if (p == MAP_FAILED)
        return -1;
    int rc = write_all(sock, p, FILE_SIZE);
    munmap(p, FILE_SIZE);
    return rc;
}

static int send_sendfile(int file, int sock)
{
    off_t off = 0; /* with an offset pointer, the file position is untouched */
    while (off < FILE_SIZE) {
        ssize_t n = sendfile(sock, file, &off, FILE_SIZE - (size_t)off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
    }
    return 0;
}

/*  file --splice--> pipe --splice--> socket
 *        page refs        page refs          (the pipe is the "buffer") */
static int send_splice(int file, int sock)
{
    int p[2];
    CHECK(pipe2(p, O_CLOEXEC) == 0);
    int rc = 0;
    size_t left = FILE_SIZE;
    while (left > 0 && rc == 0) {
        ssize_t n = splice(file, NULL, p[1], NULL, left < CHUNK ? left : CHUNK, SPLICE_F_MORE);
        if (n <= 0) {
            rc = -1;
            break;
        }
        left -= (size_t)n;
        while (n > 0) {
            ssize_t m = splice(p[0], NULL, sock, NULL, (size_t)n, SPLICE_F_MORE);
            if (m <= 0) {
                rc = -1;
                break;
            }
            n -= m;
        }
    }
    close(p[0]);
    close(p[1]);
    return rc;
}

int main(void)
{
    alarm(30);
    char path[64];
    snprintf(path, sizeof path, "/tmp/zc02-%d-XXXXXX", (int)getpid());
    int file = mkostemp(path, O_CLOEXEC);
    CHECK(file >= 0);
    CHECK(unlink(path) == 0); /* anonymous from now on: cleaned up on any exit */

    /* deterministic content + its expected hash */
    unsigned char *data = malloc(FILE_SIZE);
    CHECK(data);
    for (size_t i = 0; i < FILE_SIZE; i++)
        data[i] = (unsigned char)((i * 2654435761u) >> 13);
    CHECK(write_all(file, data, FILE_SIZE) == 0);
    const uint64_t want = fnv(FNV_INIT, data, FILE_SIZE);
    free(data);

    struct {
        const char *name;
        int (*fn)(int, int);
        const char *path;
    } m[] = {
        {"read + write", send_read_write, "file -> user buf -> socket (2 copies)"},
        {"mmap + write", send_mmap_write, "mapped pages -> socket (1 copy + faults)"},
        {"sendfile", send_sendfile, "file -> socket inside the kernel"},
        {"splice", send_splice, "file -> pipe -> socket, page references"},
    };
    unsigned char *dst = malloc(FILE_SIZE);
    CHECK(dst);
    printf("sending a %u MiB file over an AF_UNIX stream socket:\n", FILE_SIZE >> 20);
    printf("   %-14s %8s %9s   %s\n", "method", "ms", "MiB/s", "path");
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        int sv[2];
        CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
        memset(dst, 0, FILE_SIZE);
        struct receiver r = {.fd = sv[1], .dst = dst};
        pthread_t t;
        CHECK(pthread_create(&t, NULL, receive, &r) == 0);
        CHECK(lseek(file, 0, SEEK_SET) == 0);

        double t0 = now_ms();
        int rc = m[i].fn(file, sv[0]);
        CHECK(shutdown(sv[0], SHUT_WR) == 0); /* EOF for the receiver, whatever happened */
        CHECK(pthread_join(t, NULL) == 0);
        double dt = now_ms() - t0;
        close(sv[0]);
        close(sv[1]);

        CHECK(rc == 0);
        printf("   %-14s %8.2f %9.0f   %s\n", m[i].name, dt, (FILE_SIZE >> 20) / (dt / 1e3),
               m[i].path);
        CHECK(r.got == FILE_SIZE && fnv(FNV_INIT, dst, FILE_SIZE) == want); /* every byte, in order */
    }
    printf("   Over AF_UNIX (and TCP loopback) the kernel still copies the pages into\n"
           "   socket buffers and the receiver's read() copies again: sendfile/splice\n"
           "   save only the sender's user-space copy and syscalls.\n"
           "   The win is on a real NIC with scatter-gather + checksum offload, where\n"
           "   sendfile's page-cache pages are DMA'd to the wire: zero CPU copies.\n");
    free(dst);
    close(file);
    printf("02_file_to_socket: all checks passed\n");
    return 0;
}
