/*
 * 08_gdb_practice.c — a CORRECT program built to practise gdb on
 *
 * Concept : Practise breakpoints, conditional breakpoints, watchpoints,
 *           stepping, finish, frames, printing structs/arrays, x (examine
 *           memory), and attaching to a running process. The README walks
 *           through a full session on this file.
 * Build   : make run P=dsa/03-debugging/c/08_gdb_practice.c
 *           gdb -q ./build/dsa/03-debugging/c/08_gdb_practice
 *           Attach practice: ./build/dsa/03-debugging/c/08_gdb_practice --wait &
 *                            gdb -q -p $!          (it waits up to 60 s)
 * Read    : ../README.md  (section "3.1 gdb")
 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct pixel { uint8_t r, g, b; };

struct frame {
    int seq;
    int width, height;
    struct pixel data[4][4];
    uint32_t checksum;
};

static uint32_t checksum(const struct frame *f)            /* good spot for 'finish' */
{
    uint32_t sum = 0;
    for (int y = 0; y < f->height; y++)
        for (int x = 0; x < f->width; x++) {
            const struct pixel *p = &f->data[y][x];
            sum = sum * 31 + p->r + p->g + p->b;           /* 'break 37 if y == 2 && x == 3' */
        }
    return sum;
}

static void fill(struct frame *f, int seq)
{
    f->seq = seq;
    f->width = f->height = 4;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            f->data[y][x] = (struct pixel){(uint8_t)(x * 60), (uint8_t)(y * 60), (uint8_t)seq};
    f->checksum = checksum(f);                              /* 'watch f->checksum' fires here */
}

static volatile int g_keep_waiting = 1;                     /* gdb: 'set var g_keep_waiting = 0' */

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--wait") == 0) {
        printf("pid %d waiting for 'gdb -p %d' (max 60 s)...\n", (int)getpid(), (int)getpid());
        fflush(stdout);
        struct timespec tick = {0, 100 * 1000 * 1000};      /* 100 ms */
        for (int i = 0; i < 600 && g_keep_waiting; i++)
            nanosleep(&tick, NULL);                         /* attach lands in here */
    }

    struct frame frames[3];
    memset(frames, 0, sizeof frames);
    uint32_t total = 0;
    for (int i = 0; i < 3; i++) {                           /* 'break fill' then 'c' x3 */
        fill(&frames[i], i);
        total ^= frames[i].checksum;
        printf("frame %d checksum 0x%08x\n", i, frames[i].checksum);
    }
    assert(frames[1].data[3][3].r == 180 && frames[2].data[0][0].b == 2);
    assert(frames[0].checksum != frames[1].checksum);
    printf("xor of checksums = 0x%08x\nAll checks passed.\n", total);
    return 0;
}
