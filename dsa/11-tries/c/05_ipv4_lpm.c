/*
 * 05_ipv4_lpm.c — binary trie for IPv4 longest-prefix match (routing table)
 *
 * Concept : A router must pick, for a destination address, the route whose
 *           prefix matches the MOST leading bits ("longest prefix match").
 *           A binary trie keyed by address bits (MSB first) does this in at
 *           most 32 steps: walk the address's bits, remember the last node
 *           that carried a route. Verified against a brute-force linear scan
 *           of the route table with random routes and addresses.
 * Build   : make run P=dsa/11-tries/c/05_ipv4_lpm.c
 * Read    : ../README.md  (section "IPv4 longest-prefix match")
 */
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/*
 * Routes on a BeagleBone-ish host:
 *     0.0.0.0/0        -> hop 1 (default gateway)
 *     10.0.0.0/8       -> hop 2
 *     10.1.0.0/16      -> hop 3
 *     192.168.7.0/24   -> hop 4 (USB gadget network)
 *
 * Trie (bit 31 first). Depth d = prefix length; a node at depth d stands for
 * one /d prefix. [h] marks a node that carries a route:
 *
 *                 [1] root = /0
 *                0/   \1
 *              ...     ...
 *     00001010 (10) -> [2] at depth 8
 *                        \ ... 00000001 (.1) -> [3] at depth 16
 *
 * lookup(10.1.2.3): walk bits; pass [1] (best=1), [2] (best=2), [3] (best=3);
 *                   path continues or ends -> answer hop 3 (/16 beats /8, /0).
 * lookup(10.9.9.9): passes [1], [2], then diverges -> hop 2.
 * lookup(8.8.8.8):  only the root route matches -> hop 1 (default).
 */
typedef struct BNode {
    struct BNode *child[2];
    bool has_route;
    int next_hop;
} BNode;

static size_t g_nodes;

static BNode *bnode_new(void)
{
    BNode *n = calloc(1, sizeof *n);
    if (!n) {
        perror("calloc");
        exit(1);
    }
    g_nodes++;
    return n;
}

static void btrie_free(BNode *n) /* O(nodes) */
{
    if (!n)
        return;
    btrie_free(n->child[0]);
    btrie_free(n->child[1]);
    free(n);
    g_nodes--;
}

static uint32_t ip(unsigned a, unsigned b, unsigned c, unsigned d)
{
    return (uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | (uint32_t)d;
}

/* Network mask for a /len prefix. Careful: x << 32 is UNDEFINED in C, so the
 * /0 case is handled explicitly. */
static uint32_t mask_of(int len)
{
    return len == 0 ? 0u : ~0u << (32 - len);
}

/* Bit i of addr counting from the MSB (i = 0 is the top bit). */
static int bit_at(uint32_t addr, int i)
{
    return (int)((addr >> (31 - i)) & 1u);
}

/* add/replace a route — O(len) <= O(32). */
static void route_add(BNode *root, uint32_t prefix, int len, int hop)
{
    BNode *cur = root;
    for (int i = 0; i < len; i++) {
        int b = bit_at(prefix, i);
        if (!cur->child[b])
            cur->child[b] = bnode_new();
        cur = cur->child[b];
    }
    cur->has_route = true;
    cur->next_hop = hop;
}

/* longest-prefix match — O(32): never more than 32 bit tests, whatever the
 * size of the table. Returns -1 if no route (no default route installed). */
static int route_lookup(const BNode *root, uint32_t addr)
{
    int best = -1;
    const BNode *cur = root;
    for (int i = 0; cur; i++) {
        if (cur->has_route)
            best = cur->next_hop; /* deeper = longer prefix = better */
        if (i == 32)
            break;
        cur = cur->child[bit_at(addr, i)];
    }
    return best;
}

/* delete a route, pruning dead branches — O(32). Returns true if it existed. */
static bool del_rec(BNode *n, uint32_t prefix, int len, int depth)
{
    if (depth == len) {
        bool had = n->has_route;
        n->has_route = false;
        return had;
    }
    int b = bit_at(prefix, depth);
    BNode *c = n->child[b];
    if (!c || !del_rec(c, prefix, len, depth + 1))
        return false;
    if (!c->has_route && !c->child[0] && !c->child[1]) {
        free(c);
        g_nodes--;
        n->child[b] = NULL;
    }
    return true;
}

/* ---- brute force reference: O(n) scan, keep the longest matching prefix ---- */
typedef struct {
    uint32_t prefix;
    int len, hop;
    bool live;
} Route;

#define NR 600
static Route table[NR];
static int nt;

static int brute_lookup(uint32_t addr)
{
    int best_len = -1, hop = -1;
    for (int i = 0; i < nt; i++) {
        if (!table[i].live)
            continue;
        uint32_t m = mask_of(table[i].len);
        if ((addr & m) == table[i].prefix && table[i].len > best_len) {
            best_len = table[i].len;
            hop = table[i].hop;
        }
    }
    return hop;
}

static uint32_t rnd32(void)
{
    return (uint32_t)(rand() & 0xffff) << 16 | (uint32_t)(rand() & 0xffff);
}

int main(void)
{
    BNode *root = bnode_new();
    route_add(root, 0, 0, 1);
    route_add(root, ip(10, 0, 0, 0), 8, 2);
    route_add(root, ip(10, 1, 0, 0), 16, 3);
    route_add(root, ip(192, 168, 7, 0), 24, 4);
    route_add(root, ip(192, 168, 7, 2), 32, 5); /* host route */

    struct {
        uint32_t a;
        int want;
        const char *why;
    } q[] = {{ip(10, 1, 2, 3), 3, "10.1.2.3   -> /16 beats /8"},
             {ip(10, 9, 9, 9), 2, "10.9.9.9   -> /8"},
             {ip(8, 8, 8, 8), 1, "8.8.8.8    -> default /0"},
             {ip(192, 168, 7, 1), 4, "192.168.7.1 -> /24"},
             {ip(192, 168, 7, 2), 5, "192.168.7.2 -> /32 host route"}};
    for (size_t i = 0; i < sizeof q / sizeof *q; i++) {
        int h = route_lookup(root, q[i].a);
        printf("  %-32s hop %d\n", q[i].why, h);
        assert(h == q[i].want);
    }
    assert(del_rec(root, ip(10, 1, 0, 0), 16, 0));
    assert(route_lookup(root, ip(10, 1, 2, 3)) == 2); /* falls back to /8 */
    btrie_free(root);
    assert(g_nodes == 0);

    /* ---- randomised: random table, random lookups vs brute force ---- */
    srand(2024);
    root = bnode_new();
    for (nt = 0; nt < NR; nt++) {
        int len = rand() % 33; /* 0..32 */
        uint32_t p = rnd32() & mask_of(len);
        /* Bias half of the routes into 10.0.0.0/8 so prefixes nest deeply. */
        if (len >= 8 && rand() % 2)
            p = (p & ~0xff000000u) | ip(10, 0, 0, 0);
        table[nt] = (Route){p, len, nt + 100, true};
        /* A later duplicate prefix overrides: mark the older one dead. */
        for (int j = 0; j < nt; j++)
            if (table[j].live && table[j].len == len && table[j].prefix == p)
                table[j].live = false;
        route_add(root, p, len, nt + 100);
    }
    long checks = 0;
    for (int i = 0; i < 100000; i++) {
        uint32_t a = rnd32();
        if (i % 2) /* half the queries inside 10/8 to hit the deep routes */
            a = (a & 0x00ffffffu) | ip(10, 0, 0, 0);
        assert(route_lookup(root, a) == brute_lookup(a));
        checks++;
    }
    /* delete half of the routes and re-check */
    for (int i = 0; i < nt; i += 2)
        if (table[i].live) {
            assert(del_rec(root, table[i].prefix, table[i].len, 0));
            table[i].live = false;
        }
    for (int i = 0; i < 50000; i++) {
        uint32_t a = (i % 2) ? ((rnd32() & 0x00ffffffu) | ip(10, 0, 0, 0)) : rnd32();
        assert(route_lookup(root, a) == brute_lookup(a));
        checks++;
    }
    printf("randomised: %d routes, %ld lookups match brute force, trie has %zu nodes\n", nt,
           checks, g_nodes);
    btrie_free(root);
    assert(g_nodes == 0);
    return 0;
}
