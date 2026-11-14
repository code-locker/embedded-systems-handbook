/*
 * 12_memcpy_vs_memmove.c — why memmove exists: overlapping copies, drawn
 *
 * Concept : memcpy assumes source and destination do NOT overlap; it may
 *           copy in any order and in any chunk size. memmove handles overlap
 *           by choosing the copy DIRECTION: forwards when dst is before src,
 *           backwards when dst is after src. We implement both, show the
 *           corruption a naive forward copy causes, and use memmove for the
 *           everyday overlapping jobs (insert/delete inside a buffer).
 *           (Calling the real memcpy on overlap is undefined behaviour — ASan
 *           reports "memcpy-param-overlap" — so we only overlap OUR naive copy.)
 * Build   : make run P=dsa/05-arrays-and-strings/c/12_memcpy_vs_memmove.c
 * Read    : ../notes/strings.md  (section "memcpy vs memmove")
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/* Forward byte copy. This is a CORRECT memcpy (no overlap allowed), and a
 * WRONG memmove when dst > src and the ranges overlap. O(n).            */
static void *naive_forward_copy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    for (size_t i = 0; i < n; i++)
        d[i] = s[i];
    return dst;
}

/*
 * memmove: pick the direction that never reads a byte we already overwrote.
 *
 *  Case A: dst < src  (shift LEFT)  -> copy FORWARDS, front to back
 *      src:        [c][d][e][f]
 *      dst:  [ ][ ][ ][ ]           each write lands BEHIND the reader
 *
 *  Case B: dst > src  (shift RIGHT) -> copy BACKWARDS, back to front
 *      src:  [a][b][c][d]
 *      dst:        [ ][ ][ ][ ]     a forward copy would write dst[0] =
 *                                    src[2] BEFORE reading src[2]: lost!
 *
 * Comparing two pointers into different objects with < is technically
 * unspecified in C, so real libcs compare as integers (uintptr_t), like here.
 * O(n) time, O(1) extra space (no temporary buffer needed).             */
static void *my_memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    if ((uintptr_t)d < (uintptr_t)s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else if ((uintptr_t)d > (uintptr_t)s) {
        for (size_t i = n; i-- > 0;)
            d[i] = s[i];
    }
    return dst;
}

/* Insert `ins` at position pos of a NUL-terminated string in a buffer of
 * capacity cap. O(len - pos + m): everything after pos shifts right.
 *   before: [h][e][l][l][o][\0]            insert "XY" at 2
 *   memmove tail right by 2:  [h][e][ ][ ][l][l][o][\0]
 *   memcpy  "XY" into gap:    [h][e][X][Y][l][l][o][\0]                */
static int str_insert(char *buf, size_t cap, size_t pos, const char *ins)
{
    size_t len = strlen(buf), m = strlen(ins);
    if (pos > len || len + m + 1 > cap)
        return -1;
    memmove(buf + pos + m, buf + pos, len - pos + 1); /* +1 moves the '\0' */
    memcpy(buf + pos, ins, m);                         /* no overlap: memcpy */
    return 0;
}

/* Delete n chars at pos: shift the tail LEFT over the hole. O(len - pos). */
static void str_erase(char *buf, size_t pos, size_t n)
{
    size_t len = strlen(buf);
    if (pos >= len)
        return;
    if (n > len - pos)
        n = len - pos;
    memmove(buf + pos, buf + pos + n, len - pos - n + 1);
}

int main(void)
{
    puts("== shift RIGHT by 2 inside one buffer (dst > src, overlapping) ==");
    char a[16] = "abcdef";
    char b[16] = "abcdef";
    /* copy 6 bytes from index 0 to index 2 */
    naive_forward_copy(a + 2, a, 6);
    my_memmove(b + 2, b, 6);
    a[8] = b[8] = '\0';
    /*
     * naive forward, step by step:
     *   i=0: a[2]=a[0]='a'   [a][b][a][d][e][f]
     *   i=1: a[3]=a[1]='b'   [a][b][a][b][e][f]
     *   i=2: a[4]=a[2]='a'   <- a[2] was ALREADY overwritten: reads 'a', not 'c'
     *   ...  pattern "ab" repeats forever: "ababababab"
     */
    printf("  naive forward copy : \"%s\"   (corrupted: re-read its own output)\n", a);
    printf("  memmove (backwards): \"%s\"   (correct)\n", b);
    CHECK(strcmp(a, "abababab") == 0);
    CHECK(strcmp(b, "ababcdef") == 0);
    char c[16] = "abcdef";
    memmove(c + 2, c, 6);
    c[8] = '\0';
    CHECK(strcmp(b, c) == 0); /* ours agrees with libc's */

    puts("== shift LEFT by 2 (dst < src): forward copy is safe ==");
    char d[16] = "abcdef";
    my_memmove(d, d + 2, 5); /* "cdef" + '\0' */
    printf("  memmove left -> \"%s\"\n", d);
    CHECK(strcmp(d, "cdef") == 0);

    puts("== exhaustive check of my_memmove vs memmove ==");
    /* every (src offset, dst offset, length) inside a 24-byte buffer */
    int tested = 0;
    for (size_t so = 0; so < 24; so++)
        for (size_t dof = 0; dof < 24; dof++)
            for (size_t n = 0; n + (so > dof ? so : dof) <= 24; n++) {
                unsigned char x[24], y[24];
                for (int i = 0; i < 24; i++)
                    x[i] = y[i] = (unsigned char)(i * 7 + 1);
                my_memmove(x + dof, x + so, n);
                memmove(y + dof, y + so, n);
                CHECK(memcmp(x, y, sizeof x) == 0);
                tested++;
            }
    printf("  %d overlapping/non-overlapping cases agree\n", tested);

    puts("== real use: insert / erase inside a string buffer ==");
    char buf[32] = "BeagleAI-64";
    CHECK(str_insert(buf, sizeof buf, 6, "Bone ") == 0);
    printf("  after insert: \"%s\"\n", buf);
    CHECK(strcmp(buf, "BeagleBone AI-64") == 0);
    str_erase(buf, 0, 6);
    printf("  after erase : \"%s\"\n", buf);
    CHECK(strcmp(buf, "Bone AI-64") == 0);
    CHECK(str_insert(buf, 12, 0, "too long for cap") == -1);

    puts("\nall checks passed");
    return 0;
}
