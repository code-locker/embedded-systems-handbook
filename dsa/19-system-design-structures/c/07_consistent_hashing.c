/*
 * 07_consistent_hashing.c — consistent hashing ring with virtual nodes
 *
 * Concept : key -> server via hash(key) % N reshuffles almost EVERY key when
 *           N changes. Consistent hashing puts servers and keys on the same
 *           circle of hash values; a key belongs to the first server point
 *           clockwise. Adding/removing a server only moves the keys in the
 *           arc it takes over (~1/N of them). Virtual nodes (many points per
 *           server) even out the load.
 * Build   : make run P=dsa/19-system-design-structures/c/07_consistent_hashing.c
 * Read    : ../README.md  (section "Consistent hashing")
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *                 0 / 2^32
 *                  .--A1--.
 *             C2 /          \  k1 -> walks clockwise -> B1
 *               |            B1
 *               |   ring     |
 *             A2 \          / k2 -> C1
 *                  '--C1--'
 *  Each server S has V points: hash("S#0"), hash("S#1"), ...
 *  Lookup = binary search for the first point >= hash(key), wrapping to 0.
 */

/* FNV-1a then a 64-bit finaliser (from splitmix64): FNV alone clusters
   similar strings like "node#1", "node#2"; the mixer spreads them. */
static uint32_t hash_str(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    h ^= h >> 30; h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27; h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return (uint32_t)h;
}

typedef struct { uint32_t pos; int server; } Point;
typedef struct { Point *pts; size_t n, cap; int vnodes; } Ring;

static int cmp_point(const void *a, const void *b)
{
    const Point *x = a, *y = b;
    if (x->pos != y->pos) return x->pos < y->pos ? -1 : 1;
    return x->server - y->server;          /* deterministic tie-break */
}

/* O((N*V) log(N*V)) — rebuild on membership change is rare and cheap. */
static void ring_add(Ring *r, int server)
{
    if (r->n + (size_t)r->vnodes > r->cap) {
        r->cap = (r->n + (size_t)r->vnodes) * 2;
        r->pts = realloc(r->pts, r->cap * sizeof *r->pts);
        assert(r->pts);
    }
    char name[32];
    for (int v = 0; v < r->vnodes; v++) {
        snprintf(name, sizeof name, "server-%d#%d", server, v);
        r->pts[r->n++] = (Point){ hash_str(name), server };
    }
    qsort(r->pts, r->n, sizeof *r->pts, cmp_point);
}

static void ring_remove(Ring *r, int server)
{
    size_t w = 0;
    for (size_t i = 0; i < r->n; i++)      /* filtering keeps sorted order */
        if (r->pts[i].server != server) r->pts[w++] = r->pts[i];
    r->n = w;
}

/* O(log(N*V)): lower_bound on the sorted ring, wrap around past the end. */
static int ring_lookup(const Ring *r, uint32_t h)
{
    size_t lo = 0, hi = r->n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (r->pts[mid].pos < h) lo = mid + 1; else hi = mid;
    }
    return r->pts[lo == r->n ? 0 : lo].server;
}

/* Brute-force reference: scan every point. */
static int ring_lookup_slow(const Ring *r, uint32_t h)
{
    const Point *best = NULL, *min = &r->pts[0];
    for (size_t i = 0; i < r->n; i++) {
        const Point *p = &r->pts[i];
        if (p->pos >= h && (!best || cmp_point(p, best) < 0)) best = p;
        if (cmp_point(p, min) < 0) min = p;
    }
    return best ? best->server : min->server;
}

enum { KEYS = 20000 };

static double stddev_load(const int *load, int nservers)
{
    double mean = 0, var = 0;
    for (int i = 0; i < nservers; i++) mean += load[i];
    mean /= nservers;
    for (int i = 0; i < nservers; i++) var += (load[i] - mean) * (load[i] - mean);
    return sqrt(var / nservers) / mean;    /* relative: 0.1 = +-10% */
}

int main(void)
{
    static uint32_t key_hash[KEYS];
    char name[32];
    for (int k = 0; k < KEYS; k++) {
        snprintf(name, sizeof name, "user:%d", k);
        key_hash[k] = hash_str(name);
    }

    printf("== binary-search lookup == brute force ==\n");
    Ring r = { NULL, 0, 0, 50 };
    for (int s = 0; s < 5; s++) ring_add(&r, s);
    for (int k = 0; k < KEYS; k++) assert(ring_lookup(&r, key_hash[k]) == ring_lookup_slow(&r, key_hash[k]));
    assert(ring_lookup(&r, 0) == ring_lookup_slow(&r, 0));
    assert(ring_lookup(&r, UINT32_MAX) == ring_lookup_slow(&r, UINT32_MAX));
    printf("%d keys + edge hashes 0 and 2^32-1 agree\n", KEYS);

    printf("\n== add a 6th server: who moves? ==\n");
    static int before[KEYS];
    for (int k = 0; k < KEYS; k++) before[k] = ring_lookup(&r, key_hash[k]);
    ring_add(&r, 5);
    int moved = 0;
    for (int k = 0; k < KEYS; k++) {
        int now = ring_lookup(&r, key_hash[k]);
        if (now != before[k]) {
            assert(now == 5);              /* keys ONLY move to the new server */
            moved++;
        }
    }
    int moved_mod = 0;
    for (int k = 0; k < KEYS; k++)
        if (key_hash[k] % 5 != key_hash[k] % 6) moved_mod++;
    printf("consistent hashing moved %5d keys (%.1f%%, ideal 1/6 = 16.7%%)\n",
           moved, 100.0 * moved / KEYS);
    printf("hash %% N          moved %5d keys (%.1f%%)\n", moved_mod, 100.0 * moved_mod / KEYS);
    assert(moved > KEYS / 12 && moved < KEYS / 4);
    assert(moved_mod > KEYS * 3 / 4);

    printf("\n== remove server 2: only its keys move ==\n");
    for (int k = 0; k < KEYS; k++) before[k] = ring_lookup(&r, key_hash[k]);
    ring_remove(&r, 2);
    for (int k = 0; k < KEYS; k++) {
        int now = ring_lookup(&r, key_hash[k]);
        if (before[k] != 2) assert(now == before[k]);
        else assert(now != 2);
    }
    printf("every key not on server 2 stayed put\n");
    free(r.pts);

    printf("\n== load balance vs number of virtual nodes (10 servers) ==\n");
    double prev = 1e9;
    int vlist[] = { 1, 10, 100, 500 };
    for (size_t i = 0; i < sizeof vlist / sizeof vlist[0]; i++) {
        Ring rv = { NULL, 0, 0, vlist[i] };
        for (int s = 0; s < 10; s++) ring_add(&rv, s);
        int load[10] = { 0 };
        for (int k = 0; k < KEYS; k++) load[ring_lookup(&rv, key_hash[k])]++;
        double sd = stddev_load(load, 10);
        printf("vnodes=%4d  relative stddev of load = %5.1f%%\n", vlist[i], 100 * sd);
        if (vlist[i] >= 100) assert(sd < prev + 0.02);   /* more vnodes => smoother */
        prev = sd;
        free(rv.pts);
    }
    assert(prev < 0.10);
    printf("OK\n");
    return 0;
}
