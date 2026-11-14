/*
 * 06_bloom_filter.c — Bloom filter: "definitely not" or "probably yes"
 *
 * Concept : A bit array of m bits and k hash functions. add(x) sets k bits;
 *           query(x) checks those k bits. If any is 0 -> x was NEVER added
 *           (no false negatives). If all are 1 -> PROBABLY added (false
 *           positives possible, rate ~ (1 - e^(-kn/m))^k). Uses tiny memory
 *           (~10 bits per item for 1% error) and never stores the keys.
 * Build   : make run P=dsa/08-hashing/c/06_bloom_filter.c
 * Read    : ../README.md  (section "3.7 Bloom filter")
 */
#include <math.h>
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
 * m = 16 bits, k = 3. add("cat") with hashes 1, 6, 13:
 *
 *   bit:  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5
 *         0 1 0 0 0 0 1 0 0 0 0 0 0 1 0 0
 *   add("dog") -> 3, 6, 10:
 *         0 1 0 1 0 0 1 0 0 0 1 0 0 1 0 0
 *   query("eel") -> 1, 3, 10   all set, but eel was never added:
 *                               FALSE POSITIVE (bits set by cat + dog)
 *   query("fox") -> 2, ...      bit 2 is 0 -> DEFINITELY absent
 *
 * You cannot delete (clearing a bit may erase other keys' bits). Counting
 * Bloom filters use small counters instead of bits to allow that.
 */
struct bloom {
    uint8_t *bits; /* owned, m/8 bytes */
    size_t m;      /* number of bits */
    unsigned k;    /* number of hash functions */
};

static uint64_t fnv1a64(const char *s, uint64_t seed)
{
    uint64_t h = 14695981039346656037ull ^ seed;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

/*
 * Kirsch–Mitzenmacher trick: k hashes from TWO: g_i(x) = h1 + i*h2.
 * Proven to keep the same false-positive rate; saves k-2 hash computations.
 */
static size_t bit_index(const struct bloom *b, uint64_t h1, uint64_t h2, unsigned i)
{
    return (size_t)((h1 + (uint64_t)i * h2) % b->m);
}

/* Optimal sizing for n items and target false-positive rate p:
 *   m = -n ln p / (ln 2)^2      k = (m/n) ln 2                         */
static void bloom_init(struct bloom *b, size_t n, double p)
{
    double ln2 = log(2.0);
    b->m = (size_t)ceil(-(double)n * log(p) / (ln2 * ln2));
    b->k = (unsigned)lround((double)b->m / (double)n * ln2);
    if (b->k < 1) b->k = 1;
    b->bits = calloc((b->m + 7) / 8, 1);
    CHECK(b->bits != NULL);
}

static void bloom_add(struct bloom *b, const char *key) /* O(k) */
{
    uint64_t h1 = fnv1a64(key, 0), h2 = fnv1a64(key, 0x9e3779b97f4a7c15ull) | 1;
    for (unsigned i = 0; i < b->k; i++) {
        size_t bit = bit_index(b, h1, h2, i);
        b->bits[bit / 8] |= (uint8_t)(1u << (bit % 8));
    }
}

static int bloom_maybe(const struct bloom *b, const char *key) /* O(k) */
{
    uint64_t h1 = fnv1a64(key, 0), h2 = fnv1a64(key, 0x9e3779b97f4a7c15ull) | 1;
    for (unsigned i = 0; i < b->k; i++) {
        size_t bit = bit_index(b, h1, h2, i);
        if (!(b->bits[bit / 8] & (1u << (bit % 8)))) return 0; /* definitely not */
    }
    return 1; /* probably */
}

int main(void)
{
    const size_t n = 10000;
    const double target = 0.01;
    struct bloom b;
    bloom_init(&b, n, target);
    printf("n=%zu target p=%.2f -> m=%zu bits (%.1f bits/item, %zu bytes), k=%u\n",
           n, target, b.m, (double)b.m / (double)n, (b.m + 7) / 8, b.k);
    CHECK(b.k == 7); /* textbook: 1% needs ~9.6 bits/item and 7 hashes */

    char key[32];
    for (size_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "/lib/modules/m%zu.ko", i);
        bloom_add(&b, key);
    }
    /* 1. NO false negatives, ever. */
    for (size_t i = 0; i < n; i++) {
        snprintf(key, sizeof key, "/lib/modules/m%zu.ko", i);
        CHECK(bloom_maybe(&b, key));
    }
    /* 2. False-positive rate close to the target on never-added keys. */
    size_t fp = 0, trials = 100000;
    for (size_t i = 0; i < trials; i++) {
        snprintf(key, sizeof key, "/usr/bin/x%zu", i);
        fp += (size_t)bloom_maybe(&b, key);
    }
    double rate = (double)fp / (double)trials;
    printf("false positives: %zu / %zu = %.4f (target %.2f)\n", fp, trials, rate, target);
    CHECK(rate < 2 * target);

    /* Typical use: guard an expensive lookup (disk read, network request).
     * Only "maybe" answers pay the cost; most misses are filtered for free. */
    printf("=> %.1f%% of disk reads for absent keys avoided\n", 100.0 * (1.0 - rate));

    free(b.bits);
    printf("all bloom-filter checks passed\n");
    return 0;
}
