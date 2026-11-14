/*
 * 07_applications.c — classic hashing problems in C: two-sum, frequency
 *                     counting, dedup, first unique char, longest run
 *
 * Concept : The pattern behind almost every hashing interview problem:
 *           "I need to know, in O(1), whether I have SEEN something before
 *           (or how often, or where)." Trade O(n) memory for turning an
 *           O(n^2) double loop into one O(n) pass.
 * Build   : make run P=dsa/08-hashing/c/07_applications.c
 * Read    : ../README.md  (section "3.8 The seen-before pattern")
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

/* ---- a minimal int -> int map (linear probing, fixed capacity) ----------
 * Good enough for "one pass over n inputs": size it to >= 2n once, never
 * resize, never delete. That is how you write it in 3 minutes in C. */
struct imap {
    int *keys;
    int *vals;
    unsigned char *used;
    size_t cap; /* power of two */
};

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static void imap_init(struct imap *m, size_t n)
{
    m->cap = 16;
    while (m->cap < 2 * n) m->cap <<= 1; /* load <= 0.5 */
    m->keys = malloc(m->cap * sizeof *m->keys);
    m->vals = malloc(m->cap * sizeof *m->vals);
    m->used = calloc(m->cap, 1);
    CHECK(m->keys && m->vals && m->used);
}
static void imap_free(struct imap *m) { free(m->keys); free(m->vals); free(m->used); }

/* Returns the slot of key (existing or new-empty). O(1) average. */
static size_t imap_slot(const struct imap *m, int key)
{
    size_t i = mix((uint32_t)key) & (m->cap - 1);
    while (m->used[i] && m->keys[i] != key) i = (i + 1) & (m->cap - 1);
    return i;
}
static int *imap_get(struct imap *m, int key)
{
    size_t i = imap_slot(m, key);
    return m->used[i] ? &m->vals[i] : NULL;
}
static int *imap_upsert(struct imap *m, int key, int init)
{
    size_t i = imap_slot(m, key);
    if (!m->used[i]) { m->used[i] = 1; m->keys[i] = key; m->vals[i] = init; }
    return &m->vals[i];
}

/*
 * Two-sum: indices i<j with a[i]+a[j]==target.
 * Brute force O(n^2). Hash: one pass, remember value -> index; for each x
 * ask "have I seen target - x?". O(n) time, O(n) space.
 *
 *   a = [2, 7, 11, 15], target 9
 *   x=2: need 7, seen{} -> no;     seen{2:0}
 *   x=7: need 2, seen{2:0} -> YES  answer (0, 1)
 */
static int two_sum(const int *a, size_t n, int target, size_t *i_out, size_t *j_out)
{
    struct imap seen;
    imap_init(&seen, n);
    int found = 0;
    for (size_t j = 0; j < n && !found; j++) {
        int *i = imap_get(&seen, target - a[j]); /* check BEFORE inserting a[j]: */
        if (i) {                                 /* never pair an element with itself */
            *i_out = (size_t)*i;
            *j_out = j;
            found = 1;
        }
        *imap_upsert(&seen, a[j], (int)j) = (int)j;
    }
    imap_free(&seen);
    return found;
}

/* Dedup keeping first occurrence order. O(n) vs O(n log n) sort-based
 * (which also loses the order). Writes in place; returns new length. */
static size_t dedup(int *a, size_t n)
{
    struct imap seen;
    imap_init(&seen, n);
    size_t w = 0;
    for (size_t r = 0; r < n; r++) {
        size_t s = imap_slot(&seen, a[r]);
        if (!seen.used[s]) {
            seen.used[s] = 1;
            seen.keys[s] = a[r];
            a[w++] = a[r];
        }
    }
    imap_free(&seen);
    return w;
}

/*
 * Longest run of consecutive integers (unsorted input), O(n):
 * put all in a set; only START counting at x if x-1 is NOT in the set
 * (x is the beginning of a run). Each number is visited at most twice.
 */
static int longest_consecutive(const int *a, size_t n)
{
    struct imap set;
    imap_init(&set, n);
    for (size_t i = 0; i < n; i++) imap_upsert(&set, a[i], 0);
    int best = 0;
    for (size_t i = 0; i < n; i++) {
        if (imap_get(&set, a[i] - 1)) continue; /* not a run start */
        int len = 1;
        while (imap_get(&set, a[i] + len)) len++;
        if (len > best) best = len;
    }
    imap_free(&set);
    return best;
}

/*
 * When the key space is tiny, the "hash table" is just an array indexed by
 * the key itself (a perfect hash). 256 counters for bytes. O(n), O(1) space.
 */
static int first_unique_char(const char *s)
{
    int count[256] = {0};
    for (const char *p = s; *p; p++) count[(unsigned char)*p]++;
    for (const char *p = s; *p; p++)
        if (count[(unsigned char)*p] == 1) return (int)(p - s);
    return -1;
}

static int is_anagram(const char *a, const char *b)
{
    int count[256] = {0};
    for (; *a; a++) count[(unsigned char)*a]++;
    for (; *b; b++) count[(unsigned char)*b]--;
    for (int i = 0; i < 256; i++)
        if (count[i]) return 0;
    return 1;
}

/* Frequency counting: most common value in O(n). */
static int most_frequent(const int *a, size_t n, int *count_out)
{
    struct imap freq;
    imap_init(&freq, n);
    int best = a[0], best_n = 0;
    for (size_t i = 0; i < n; i++) {
        int *c = imap_upsert(&freq, a[i], 0);
        if (++*c > best_n) { best_n = *c; best = a[i]; }
    }
    imap_free(&freq);
    *count_out = best_n;
    return best;
}

int main(void)
{
    int a[] = {2, 7, 11, 15};
    size_t i, j;
    CHECK(two_sum(a, 4, 9, &i, &j) && i == 0 && j == 1);
    CHECK(two_sum(a, 4, 26, &i, &j) && i == 2 && j == 3);
    CHECK(!two_sum(a, 4, 4, &i, &j)); /* 2+2 would reuse index 0 */
    int b[] = {3, 3};
    CHECK(two_sum(b, 2, 6, &i, &j) && i == 0 && j == 1);
    printf("two-sum: OK\n");

    int d[] = {5, 1, 5, 2, 1, 9, 2, 5};
    size_t n = dedup(d, 8);
    int expect[] = {5, 1, 2, 9};
    CHECK(n == 4 && memcmp(d, expect, sizeof expect) == 0);
    printf("dedup: [5 1 5 2 1 9 2 5] -> [5 1 2 9]\n");

    int c[] = {100, 4, 200, 1, 3, 2, -1, 0};
    CHECK(longest_consecutive(c, 8) == 6); /* -1,0,1,2,3,4 */
    printf("longest consecutive run: 6\n");

    CHECK(first_unique_char("beaglebone") == 2); /* 'a' */
    CHECK(first_unique_char("aabb") == -1);
    CHECK(is_anagram("listen", "silent") && !is_anagram("rat", "car"));
    printf("first unique / anagram: OK\n");

    int irq[] = {33, 41, 33, 7, 33, 41, 7, 33};
    int cnt;
    CHECK(most_frequent(irq, 8, &cnt) == 33 && cnt == 4);
    printf("most frequent IRQ: 33 (x%d)\n", cnt);

    /* scale check: 200k elements two-sum, would be 2e10 ops brute force */
    enum { BIG = 200000 };
    int *big = malloc(BIG * sizeof *big);
    CHECK(big != NULL);
    for (int k = 0; k < BIG; k++) big[k] = k * 3;
    CHECK(two_sum(big, BIG, 3 * (BIG - 1) + 3 * (BIG - 2), &i, &j));
    CHECK(i == BIG - 2 && j == BIG - 1);
    free(big);

    printf("all application checks passed\n");
    return 0;
}
