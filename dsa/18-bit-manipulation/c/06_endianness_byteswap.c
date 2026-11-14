/*
 * 06_endianness_byteswap.c — byte order: detect it, swap it, and (best) write
 *                            code that does not care about it
 *
 * Concept : Endianness is the order in which a multi-byte integer's bytes sit
 *           in memory. Little endian (x86, ARM Linux incl. the TDA4VM's A72)
 *           stores the least significant byte at the lowest address; network
 *           protocols and many file formats are big endian. The portable rule:
 *           to read a wire/file format, assemble the value from bytes with
 *           shifts — that is correct on any host, no #ifdef needed.
 * Build   : make run P=dsa/18-bit-manipulation/c/06_endianness_byteswap.c
 * Read    : ../README.md  (section "3.8 Endianness and byte swapping")
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Runtime detection via memcpy: inspecting the first byte of a known value.
 * memcpy (not a pointer cast) avoids strict-aliasing problems. */
static int host_is_little_endian(void)
{
    uint32_t one = 1;
    uint8_t first;
    memcpy(&first, &one, 1);
    return first == 1;
}

/* Manual 32-bit byte swap.
 *   0x11223344 -> 0x44332211
 *   each byte is masked out, then shifted to its mirrored slot. */
static uint32_t bswap32_manual(uint32_t x)
{
    return ((x & 0x000000FFu) << 24) |
           ((x & 0x0000FF00u) << 8)  |
           ((x & 0x00FF0000u) >> 8)  |
           ((x & 0xFF000000u) >> 24);
}

static uint16_t bswap16_manual(uint16_t x)
{
    return (uint16_t)((x << 8) | (x >> 8));
}

/* 64-bit: swap halves, then swap within each half (divide and conquer). */
static uint64_t bswap64_manual(uint64_t x)
{
    return ((uint64_t)bswap32_manual((uint32_t)x) << 32) | bswap32_manual((uint32_t)(x >> 32));
}

/* Portable big-endian load/store: no dependence on host order at all.
 * This is how robust parsers (and the kernel's get_unaligned_be32) work.
 * Also safe for UNALIGNED pointers, which a plain *(uint32_t*)p is not
 * (alignment fault on some ARM cores, UB in C everywhere). */
static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint32_t load_le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[1] << 8)  |  (uint32_t)p[0];
}
static void store_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static uint16_t load_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* host-to-network: a no-op on big-endian hosts, a swap on little-endian. */
static uint32_t my_htonl(uint32_t x)
{
    return host_is_little_endian() ? bswap32_manual(x) : x;
}

int main(void)
{
    puts("== 1. What does 0x11223344 look like in memory? ==");
    uint32_t v = 0x11223344u;
    uint8_t mem[4];
    memcpy(mem, &v, 4);
    /*
     *  address:        +0   +1   +2   +3
     *  little endian:  44   33   22   11    (x86, ARM Linux)
     *  big endian:     11   22   33   44    (network order, PowerPC, m68k)
     */
    printf("host bytes: %02X %02X %02X %02X -> host is %s endian\n",
           mem[0], mem[1], mem[2], mem[3], host_is_little_endian() ? "little" : "big");
    if (host_is_little_endian())
        assert(mem[0] == 0x44 && mem[3] == 0x11);
    else
        assert(mem[0] == 0x11 && mem[3] == 0x44);

    puts("\n== 2. Byte swaps: manual vs compiler builtin ==");
    assert(bswap32_manual(0x11223344u) == 0x44332211u);
    assert(bswap16_manual(0xABCD) == 0xCDAB);
    assert(bswap64_manual(0x0102030405060708ull) == 0x0807060504030201ull);
    uint32_t r = 12345u;
    for (int i = 0; i < 200000; i++) {
        r = r * 1103515245u + 12345u;
        uint64_t r64 = ((uint64_t)r << 32) | (r ^ 0x5A5A5A5Au);
        assert(bswap32_manual(r) == __builtin_bswap32(r));
        assert(bswap16_manual((uint16_t)r) == __builtin_bswap16((uint16_t)r));
        assert(bswap64_manual(r64) == __builtin_bswap64(r64));
        assert(bswap32_manual(bswap32_manual(r)) == r); /* involution */
    }
    puts("manual bswap16/32/64 == __builtin_bswapXX on 200000 values: OK");
    puts("(gcc turns bswap32_manual into a single BSWAP/REV instruction at -O2)");

    puts("\n== 3. Parsing a big-endian packet header portably ==");
    /* An IPv4-like header fragment as it arrives on the wire. */
    const uint8_t pkt[] = {
        0x45, 0x00, 0x05, 0xDC,    /* ver/ihl, tos, total length = 1500 */
        0xC0, 0xA8, 0x07, 0x02,    /* source address 192.168.7.2 (BeagleBone USB net!) */
    };
    uint16_t total_len = load_be16(pkt + 2);
    uint32_t src = load_be32(pkt + 4);
    printf("total length = %u, src = %u.%u.%u.%u (0x%08X)\n", total_len,
           (unsigned)(src >> 24), (unsigned)(src >> 16) & 0xFF,
           (unsigned)(src >> 8) & 0xFF, (unsigned)src & 0xFF, (unsigned)src);
    assert(total_len == 1500);
    assert(src == 0xC0A80702u);
    /* Unaligned read at offset 1 is fine with byte assembly: */
    assert(load_be32(pkt + 1) == 0x0005DCC0u);
    assert(load_le32(pkt + 4) == 0x0207A8C0u);

    /* The non-portable way: memcpy into a uint32_t then swap if needed. */
    uint32_t raw;
    memcpy(&raw, pkt + 4, 4);
    assert(my_htonl(raw) == src);          /* ntohl == htonl (same swap) */

    puts("\n== 4. Store and reload ==");
    uint8_t out[2];
    store_be16(out, 0x1234);
    assert(out[0] == 0x12 && out[1] == 0x34);
    assert(load_be16(out) == 0x1234);
    puts("store_be16(0x1234) -> bytes 12 34: OK");

    puts("\nall checks passed");
    return 0;
}
