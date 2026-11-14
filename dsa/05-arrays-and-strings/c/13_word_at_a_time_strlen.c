/*
 * 13_word_at_a_time_strlen.c — how glibc's strlen is fast: 8 bytes per step
 *
 * Concept : A byte-at-a-time strlen does one load + one compare per byte.
 *           Real libcs load a whole 64-bit word and test all 8 bytes for zero
 *           AT ONCE with the "has zero byte" bit trick:
 *               haszero(x) = (x - 0x0101010101010101) & ~x & 0x8080808080808080
 *           (glibc goes further with SIMD: 16/32/64 bytes per step.)
 *           We implement it, prove the trick against a naive check, and
 *           verify our strlen for every alignment and length.
 * Build   : make run P=dsa/05-arrays-and-strings/c/13_word_at_a_time_strlen.c
 * Read    : ../notes/strings.md  (section "How the real libc is fast")
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

#define ONES  0x0101010101010101ULL /* 0x01 in every byte */
#define HIGHS 0x8080808080808080ULL /* 0x80 in every byte */

/*
 * Why the trick works — look at ONE byte b of the word:
 *
 *   (b - 0x01): if b == 0x00 it BORROWS and becomes 0xFF -> top bit set.
 *               if b in 0x01..0x80 the top bit of (b-1) is 0 (0x00..0x7F)
 *               if b in 0x81..0xFF the top bit of (b-1) is 1  <- false alarm
 *   & ~b      : kills the false alarm: those bytes had their top bit set,
 *               so ~b has top bit 0.
 *   & 0x80    : keep only the top bit of each byte.
 *
 *     b     b-1    ~b    (b-1)&~b&0x80
 *    0x00   0xFF  0xFF      0x80   <- zero byte -> flagged
 *    0x41   0x40  0xBE      0x00
 *    0x80   0x7F  0x7F      0x00
 *    0xC3   0xC2  0x3C      0x00
 *
 * In a full word, the borrow out of a zero byte can ripple into the NEXT
 * (more significant) byte and flag it too (e.g. a 0x01 right above a 0x00).
 * But that only happens ABOVE a real zero, so:
 *   - "is the result non-zero?"   is exact (no false positives/negatives)
 *   - the LOWEST flagged byte    is exactly the first zero byte.
 * On little-endian (x86, arm64) the lowest byte is the first in memory, so
 * count trailing zero bits / 8 gives its index.
 */
static inline uint64_t haszero(uint64_t x)
{
    return (x - ONES) & ~x & HIGHS;
}

/* Reference: does any byte of x equal zero? Obviously correct, slow. */
static int haszero_naive(uint64_t x)
{
    for (int i = 0; i < 8; i++)
        if (((x >> (8 * i)) & 0xFF) == 0)
            return 1;
    return 0;
}

static size_t strlen_bytewise(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

/*
 * Word-at-a-time strlen.
 *
 *   memory:  ... [x][x][H][e][l][l][o][\0] [?][?][?][?][?][?][?][?] ...
 *                       ^s (unaligned)
 *            |<----- aligned word 0 ----->|<------ word 1 ------->|
 *
 * Step 1: walk byte by byte until p is 8-byte aligned (at most 7 steps).
 * Step 2: load aligned 8-byte words; stop at the first word with a zero.
 * Step 3: locate the zero byte inside that word.
 *
 * Why reading past the '\0' is "safe" in a real libc: an ALIGNED 8-byte load
 * never crosses a page boundary (pages are 4096-aligned, 4096 % 8 == 0), so
 * if the first byte of the word is mapped, all 8 are — no segfault. It is
 * still out-of-bounds by the C standard, which is why glibc writes it in
 * assembly and ASan has special knowledge of strlen. In THIS program we stay
 * strictly legal: the caller guarantees the buffer is padded so the final
 * aligned word lies inside the allocation (see `padded_buffer` in main).
 *
 * We load through memcpy instead of *(uint64_t *)p: that avoids
 * strict-aliasing UB, and the compiler turns it into a single load.
 * O(n/8) word steps (still O(n), but ~8x fewer iterations).            */
static size_t strlen_word(const char *s)
{
    const char *p = s;
    while ((uintptr_t)p % 8 != 0) { /* step 1: align */
        if (*p == '\0')
            return (size_t)(p - s);
        p++;
    }
    for (;;) { /* step 2: 8 bytes per iteration */
        uint64_t w;
        memcpy(&w, p, sizeof w);
        uint64_t z = haszero(w);
        if (z) {
            /* step 3: lowest set bit marks the first zero byte
             * (little-endian). __builtin_ctzll = count trailing zeros,
             * one instruction (tzcnt on x86, rbit+clz on arm64).        */
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return (size_t)(p - s) + (size_t)(__builtin_ctzll(z) / 8);
#else
            for (int i = 0; i < 8; i++) /* portable fallback */
                if (p[i] == '\0')
                    return (size_t)(p + i - s);
#endif
        }
        p += 8;
    }
}

/* Simple xorshift PRNG — deterministic, no rand() state shared with libc. */
static uint64_t rng = 0x9E3779B97F4A7C15ULL;
static uint64_t next_rand(void)
{
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

int main(void)
{
    puts("== 1. the bit trick, byte by byte ==");
    uint64_t demo;
    memcpy(&demo, "Hello\0xy", 8);
    printf("  word bytes \"Hello\\0xy\" = 0x%016llx\n", (unsigned long long)demo);
    printf("  haszero(word)          = 0x%016llx  -> first zero at byte %d\n",
           (unsigned long long)haszero(demo), __builtin_ctzll(haszero(demo)) / 8);
    CHECK(__builtin_ctzll(haszero(demo)) / 8 == 5);

    /* The borrow-ripple false flag: byte 0 is 0x00, byte 1 is 0x01.
     * Byte 1 also gets flagged, but only ABOVE a true zero. */
    uint64_t ripple = 0x4141414141410100ULL;
    printf("  ripple case 0x%016llx -> mask 0x%016llx (byte 1 falsely "
           "flagged, byte 0 is the real zero)\n",
           (unsigned long long)ripple, (unsigned long long)haszero(ripple));
    CHECK(__builtin_ctzll(haszero(ripple)) / 8 == 0);

    puts("== 2. prove haszero against the naive check ==");
    /* random words, plus words with planted zero bytes and 0x80/0x01 bytes
     * (the tricky values for borrows) */
    const uint8_t tricky[] = {0x00, 0x01, 0x7F, 0x80, 0x81, 0xFF, 0x41};
    long n_tests = 0;
    for (int t = 0; t < 200000; t++) {
        uint64_t x = next_rand();
        if (t % 2) { /* half the time build x from tricky bytes only */
            x = 0;
            for (int i = 0; i < 8; i++)
                x |= (uint64_t)tricky[next_rand() % sizeof tricky] << (8 * i);
        }
        CHECK((haszero(x) != 0) == haszero_naive(x));
        if (haszero(x)) { /* lowest flag == first zero byte */
            int first = 0;
            while (((x >> (8 * first)) & 0xFF) != 0)
                first++;
            CHECK(__builtin_ctzll(haszero(x)) / 8 == first);
        }
        n_tests++;
    }
    printf("  %ld words: trick == naive, lowest flag == first zero\n", n_tests);

    puts("== 3. strlen_word vs strlen, every alignment x length ==");
    /* padded_buffer: 8-byte aligned (uint64_t array) and 16 bytes longer
     * than the longest string we place in it, so the last aligned word our
     * strlen reads is always INSIDE the allocation — ASan stays quiet
     * because we never actually go out of bounds.                      */
    enum { MAXLEN = 300 };
    uint64_t padded_buffer[(MAXLEN + 8 + 16) / 8];
    char *base = (char *)padded_buffer;
    int combos = 0;
    for (size_t align = 0; align < 8; align++)
        for (size_t len = 0; len <= MAXLEN; len++) {
            memset(padded_buffer, 0, sizeof padded_buffer);
            char *s = base + align;
            for (size_t i = 0; i < len; i++)
                s[i] = (char)(1 + next_rand() % 255); /* never 0 */
            s[len] = '\0';
            CHECK(strlen_word(s) == len);
            CHECK(strlen_bytewise(s) == len);
            CHECK(strlen(s) == len);
            combos++;
        }
    printf("  %d (alignment, length) combinations agree\n", combos);

    puts("== 4. iterations saved ==");
    /* not a timing benchmark (sanitizers distort timing), just the count */
    size_t len = 4096;
    printf("  for a %zu-byte string: bytewise ~%zu loop steps, word ~%zu "
           "(+<=7 to align); AVX2 glibc ~%zu\n", len, len, len / 8, len / 32);

    puts("\nall checks passed");
    return 0;
}
