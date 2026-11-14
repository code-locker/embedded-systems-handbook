/*
 * 01_hash_functions.c — djb2, FNV-1a, and turning a hash into a bucket index
 *
 * Concept : A hash function squeezes an arbitrary key (a string) into a fixed
 *           size integer. A table then maps that integer to a bucket with
 *           either "% prime" or "& (pow2 - 1)". We measure how evenly keys
 *           spread and show why a weak hash + power-of-two mask is a trap.
 * Build   : make run P=dsa/08-hashing/c/01_hash_functions.c
 * Read    : ../README.md  (section "3.1 Hash functions")
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
                    __LINE__);                                                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

/*
 * djb2 (Dan Bernstein): h = h*33 + c, starting from 5381.
 * O(len). "h << 5) + h" is h*33 without a multiply (cheap on old CPUs).
 * Why 33? Empirically good for ASCII text; nothing deep. Simple and fast,
 * but low bits mix weakly — keep that in mind for the mask test below.
 */
static uint32_t djb2(const char *s)
{
    uint32_t h = 5381u;
    unsigned char c;
    while ((c = (unsigned char)*s++) != 0)
        h = ((h << 5) + h) + c; /* h * 33 + c, unsigned wrap is well defined */
    return h;
}

/*
 * FNV-1a (Fowler–Noll–Vo), 32-bit: for each byte: h ^= byte; h *= prime.
 * O(len). XOR first, then multiply: the multiply spreads every input bit
 * across many output bits ("avalanche"), so low bits are well mixed too.
 */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;          /* FNV offset basis */
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 16777619u;                /* FNV prime */
    }
    return h;
}

/* A deliberately BAD hash: sum of bytes. Anagrams collide ("abc"=="cba"),
 * and all values are small, so most buckets stay empty. */
static uint32_t sum_hash(const char *s)
{
    uint32_t h = 0;
    while (*s) h += (unsigned char)*s++;
    return h;
}

/* A bad *integer* hash for the mask demo: identity. */
static uint32_t identity(uint32_t x) { return x; }

/*
 * Bucket index from a hash:
 *
 *   h % m          m prime (e.g. 1021): every bit of h affects the result,
 *                  so even a weak hash spreads OK. Cost: a division (~20-40
 *                  cycles on a Cortex-A72).
 *
 *   h & (m - 1)    m power of two (e.g. 1024): ONE AND instruction, but it
 *                  keeps ONLY the low log2(m) bits and throws the rest away.
 *
 *        h = 1011 0110 1100 0000   (32-bit, here 16 shown)
 *   m-1  = 0000 0011 1111 1111     (1023)
 *          ---------------------
 *   idx  = 0000 0010 1100 0000     high bits ignored!
 *
 *   => mask is only safe if the hash mixes its high bits into the low bits.
 */
static size_t idx_mod(uint32_t h, size_t m) { return h % m; }
static size_t idx_mask(uint32_t h, size_t m) { return h & (m - 1); }

/* Count how many of the m buckets got at least one key, and the largest
 * bucket. Perfectly uniform: n keys into m buckets -> max ~ n/m. */
struct spread {
    size_t used;
    size_t max;
};

static struct spread measure(const uint32_t *hashes, size_t n, size_t m,
                             size_t (*to_idx)(uint32_t, size_t))
{
    size_t *count = calloc(m, sizeof *count); /* owned here, freed below */
    CHECK(count != NULL);
    struct spread r = {0, 0};
    for (size_t i = 0; i < n; i++) count[to_idx(hashes[i], m)]++;
    for (size_t b = 0; b < m; b++) {
        if (count[b]) r.used++;
        if (count[b] > r.max) r.max = count[b];
    }
    free(count);
    return r;
}

#define NKEYS 4096

int main(void)
{
    /* Known reference values so you can trust the implementations. */
    CHECK(djb2("") == 5381u);
    CHECK(fnv1a("") == 2166136261u);
    CHECK(fnv1a("a") == 0xe40c292cu); /* published FNV-1a test vector */
    CHECK(sum_hash("abc") == sum_hash("cba")); /* anagram collision */
    CHECK(djb2("abc") != djb2("cba"));         /* order matters in djb2 */

    /* Build keys like "sensor_0", "sensor_1", ... (very similar strings,
     * the realistic worst case: device names, file paths, symbol names). */
    static char keys[NKEYS][32];
    static uint32_t h_djb[NKEYS], h_fnv[NKEYS], h_sum[NKEYS];
    for (int i = 0; i < NKEYS; i++) {
        snprintf(keys[i], sizeof keys[i], "sensor_%d", i);
        h_djb[i] = djb2(keys[i]);
        h_fnv[i] = fnv1a(keys[i]);
        h_sum[i] = sum_hash(keys[i]);
    }

    const size_t m_prime = 1021, m_pow2 = 1024;
    printf("%d keys \"sensor_N\" into ~1024 buckets (ideal max ~ %d)\n",
           NKEYS, NKEYS / 1024);
    printf("%-10s %-12s %8s %8s\n", "hash", "index", "used", "max");

    struct spread s;
    s = measure(h_sum, NKEYS, m_prime, idx_mod);
    printf("%-10s %-12s %8zu %8zu\n", "sum", "% 1021", s.used, s.max);
    CHECK(s.used < 200); /* sums of ~10 ASCII chars only span a few hundred */

    s = measure(h_djb, NKEYS, m_prime, idx_mod);
    printf("%-10s %-12s %8zu %8zu\n", "djb2", "% 1021", s.used, s.max);
    /* Much better than sum, but NOT uniform (~1000 would be ideal): keys
     * that differ only in the last chars give hashes that differ by small
     * amounts (h*33 + c), so they land in neighbouring buckets. */
    CHECK(s.used > 500);

    s = measure(h_djb, NKEYS, m_pow2, idx_mask);
    printf("%-10s %-12s %8zu %8zu\n", "djb2", "& 1023", s.used, s.max);

    s = measure(h_fnv, NKEYS, m_pow2, idx_mask);
    printf("%-10s %-12s %8zu %8zu\n", "fnv1a", "& 1023", s.used, s.max);
    CHECK(s.used > 900);
    CHECK(s.max < 16);

    /* The real trap: integer keys that are multiples of 64 (aligned
     * addresses, frame numbers * 64, ...). Identity hash + mask: the low 6
     * bits are ALWAYS zero, so only 1024/64 = 16 buckets are ever used. */
    uint32_t aligned[NKEYS];
    for (uint32_t i = 0; i < NKEYS; i++) aligned[i] = identity(i * 64u);
    struct spread bad = measure(aligned, NKEYS, m_pow2, idx_mask);
    struct spread good = measure(aligned, NKEYS, m_prime, idx_mod);
    printf("\naligned ints (x*64), identity hash:\n");
    printf("  & 1023 -> used %zu buckets, max chain %zu   (disaster)\n",
           bad.used, bad.max);
    printf("  %% 1021 -> used %zu buckets, max chain %zu   (prime rescues it)\n",
           good.used, good.max);
    CHECK(bad.used == 16);
    CHECK(good.used == 1021);

    /* Same keys, but FIRST mix with a multiplicative (Fibonacci) hash, as
     * the Linux kernel's hash_32() does: x * 0x61C88647 then take the TOP
     * bits. Multiplication pushes low-bit info upward, top bits are best. */
    for (uint32_t i = 0; i < NKEYS; i++)
        aligned[i] = (i * 64u * 0x61C88647u) >> (32 - 10); /* 10 bits = 1024 */
    struct spread fib = measure(aligned, NKEYS, m_pow2, idx_mask);
    printf("  fib-hash top 10 bits -> used %zu buckets, max %zu  (kernel way)\n",
           fib.used, fib.max);
    CHECK(fib.used > 900);

    printf("\nall hash-function checks passed\n");
    return 0;
}
