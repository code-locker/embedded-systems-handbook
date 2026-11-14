/*
 * 15_utf8.c — bytes are not characters: UTF-8 encode, decode, validate, count
 *
 * Concept : C strings are BYTE arrays. ASCII uses one byte per character,
 *           but UTF-8 uses 1 to 4 bytes per code point, so strlen counts
 *           bytes, s[i] is a byte (maybe half a character), and reversing a
 *           string byte by byte destroys it. We hand-write a UTF-8 encoder,
 *           a strict decoder/validator (rejects overlong forms, surrogates,
 *           > U+10FFFF, truncated sequences), a code-point counter, and a
 *           UTF-8-aware reverse. wchar_t is shown briefly at the end.
 * Build   : make run P=dsa/05-arrays-and-strings/c/15_utf8.c
 * Read    : ../notes/strings.md  (section "Encoding: bytes vs characters")
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            exit(EXIT_FAILURE);                                              \
        }                                                                    \
    } while (0)

/*
 * UTF-8 in one table (x = payload bits of the code point):
 *
 *   code point range      bytes  byte 1     byte 2     byte 3     byte 4
 *   U+0000  .. U+007F       1    0xxxxxxx
 *   U+0080  .. U+07FF       2    110xxxxx   10xxxxxx
 *   U+0800  .. U+FFFF       3    1110xxxx   10xxxxxx   10xxxxxx
 *   U+10000 .. U+10FFFF     4    11110xxx   10xxxxxx   10xxxxxx   10xxxxxx
 *
 * Properties that make it great:
 *  - ASCII is unchanged (every old C string is valid UTF-8).
 *  - No byte of a multi-byte char is < 0x80, so '\0', '/', '\n' never
 *    appear inside a character: strlen, strchr(s,'/'), path code all work.
 *  - Self-synchronising: continuation bytes are always 10xxxxxx, so from any
 *    byte you can find the start of the character (skip back over 10xxxxxx).
 *
 * Example: 'é' = U+00E9 = 000 1110 1001
 *          2 bytes: 110 00011  10 101001  = 0xC3 0xA9
 */

#define UTF8_BAD 0xFFFFFFFFu

/* Encode one code point. Returns bytes written (1..4) or 0 if invalid.
 * Surrogates U+D800..U+DFFF are UTF-16 artifacts and illegal in UTF-8. */
static int utf8_encode(uint32_t cp, unsigned char out[4])
{
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF)
        return 0;
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (unsigned char)(0xF0 | (cp >> 18));
        out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (unsigned char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Decode one code point from s[0..n). On success returns the code point and
 * sets *used to its byte length. On error returns UTF8_BAD, *used = 1 (so a
 * caller can skip the bad byte and resynchronise). O(1).
 *
 * Strictness matters for SECURITY: an "overlong" encoding such as
 * 0xC0 0xAF decodes to '/' if you are sloppy — a classic path-traversal
 * bypass (filter checks for "../" bytes, decoder later produces it).    */
static uint32_t utf8_decode(const unsigned char *s, size_t n, int *used)
{
    *used = 1;
    if (n == 0)
        return UTF8_BAD;
    unsigned char b0 = s[0];
    int len;
    uint32_t cp, min;
    if (b0 < 0x80) {
        return b0;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2; cp = b0 & 0x1F; min = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3; cp = b0 & 0x0F; min = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4; cp = b0 & 0x07; min = 0x10000;
    } else {
        return UTF8_BAD; /* 10xxxxxx (stray continuation) or 0xF8..0xFF */
    }
    if ((size_t)len > n)
        return UTF8_BAD; /* truncated sequence */
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return UTF8_BAD; /* continuation byte must be 10xxxxxx */
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min)
        return UTF8_BAD; /* overlong: could have used fewer bytes */
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        return UTF8_BAD;
    *used = len;
    return cp;
}

/* Count code points; returns -1 if the string is not valid UTF-8. O(n). */
static long utf8_count(const char *str)
{
    const unsigned char *s = (const unsigned char *)str;
    size_t n = strlen(str);
    long count = 0;
    for (size_t i = 0; i < n;) {
        int used;
        if (utf8_decode(s + i, n - i, &used) == UTF8_BAD)
            return -1;
        i += (size_t)used;
        count++;
    }
    return count;
}

/* Fast count for ALREADY-VALID UTF-8: count bytes that are NOT
 * continuation bytes (i.e. not 10xxxxxx). One test per byte.          */
static size_t utf8_count_fast(const char *str)
{
    size_t c = 0;
    for (const unsigned char *s = (const unsigned char *)str; *s; s++)
        c += (*s & 0xC0) != 0x80;
    return c;
}

static void reverse_bytes(char *a, size_t n)
{
    for (size_t i = 0, j = n; i + 1 < j; i++, j--) {
        char t = a[i];
        a[i] = a[j - 1];
        a[j - 1] = t;
    }
}

/* UTF-8-aware reverse in place, O(n): reverse each character's bytes, then
 * reverse the whole string. The second reversal puts each character's bytes
 * back in the right order:
 *   "aé"   = [61][C3 A9]
 *   step 1 (per char): [61][A9 C3]
 *   step 2 (whole)   : [C3 A9][61]  = "éa"                            */
static void utf8_reverse(char *str)
{
    size_t n = strlen(str);
    for (size_t i = 0; i < n;) {
        int used;
        utf8_decode((const unsigned char *)str + i, n - i, &used);
        reverse_bytes(str + i, (size_t)used);
        i += (size_t)used;
    }
    reverse_bytes(str, n);
}

static void dump(const char *label, const char *s)
{
    printf("  %-10s bytes:", label);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        printf(" %02X", *p);
    putchar('\n');
}

int main(void)
{
    /* Written as explicit bytes so the result doesn't depend on the source
     * file's encoding or the compiler's execution charset:
     *   "café ✓ 😀"  = c a f [C3 A9] ' ' [E2 9C 93] ' ' [F0 9F 98 80]   */
    const char *text = "caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x98\x80";

    puts("== 1. strlen counts BYTES ==");
    dump("text", text);
    printf("  strlen = %zu bytes, but %ld code points\n", strlen(text),
           utf8_count(text));
    CHECK(strlen(text) == 14);
    CHECK(utf8_count(text) == 8);
    CHECK(utf8_count_fast(text) == 8);
    printf("  text[3] = 0x%02X: half of 'e-acute', not a character\n",
           (unsigned char)text[3]);

    puts("== 2. decode every code point ==");
    const unsigned char *u = (const unsigned char *)text;
    size_t n = strlen(text);
    uint32_t expect[] = {'c', 'a', 'f', 0xE9, ' ', 0x2713, ' ', 0x1F600};
    int k = 0;
    for (size_t i = 0; i < n; k++) {
        int used;
        uint32_t cp = utf8_decode(u + i, n - i, &used);
        printf("  U+%04X  (%d byte%s)\n", (unsigned)cp, used, used > 1 ? "s" : "");
        CHECK(cp == expect[k]);
        i += (size_t)used;
    }

    puts("== 3. encode/decode round trip over ALL code points ==");
    long valid = 0;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp++) {
        unsigned char buf[4];
        int len = utf8_encode(cp, buf);
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            CHECK(len == 0);
            continue;
        }
        int used;
        CHECK(len > 0 && utf8_decode(buf, (size_t)len, &used) == cp && used == len);
        valid++;
    }
    printf("  %ld scalar values round-trip (1,114,112 - 2,048 surrogates)\n", valid);
    CHECK(valid == 0x110000 - 0x800);

    puts("== 4. the validator rejects malformed input ==");
    struct { const char *bytes; const char *why; } bad[] = {
        {"\xC0\xAF", "overlong '/' (security bug if accepted)"},
        {"\xE0\x80\xAF", "overlong 3-byte '/'"},
        {"\xED\xA0\x80", "UTF-16 surrogate U+D800"},
        {"\xF4\x90\x80\x80", "above U+10FFFF"},
        {"\xC3", "truncated 2-byte sequence"},
        {"\xA9", "stray continuation byte"},
        {"\xC3\x28", "bad continuation byte"},
        {"\xFF", "0xFF never appears in UTF-8"},
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        printf("  rejected: %s\n", bad[i].why);
        CHECK(utf8_count(bad[i].bytes) == -1);
    }

    puts("== 5. reverse: bytewise breaks it, char-wise doesn't ==");
    char r1[32], r2[32];
    strcpy(r1, "a\xC3\xA9z");
    strcpy(r2, r1);
    reverse_bytes(r1, strlen(r1));
    utf8_reverse(r2);
    dump("bytewise", r1);
    dump("utf8-wise", r2);
    CHECK(utf8_count(r1) == -1);                 /* now invalid UTF-8 */
    CHECK(strcmp(r2, "z\xC3\xA9" "a") == 0);     /* "zéa" */

    puts("== 6. wchar_t, briefly ==");
    /* wchar_t is a "wide character": 4 bytes (UTF-32) on Linux, 2 bytes
     * (UTF-16, so emoji need TWO units) on Windows. L"..." literals and
     * wcslen count wchar_t units. Linux APIs (open, write, GStreamer, the
     * kernel) all speak UTF-8 char*, so on Linux you almost never need it;
     * use char + UTF-8 everywhere, convert only at a Windows boundary.  */
    const wchar_t *w = L"caf\x00E9";
    printf("  sizeof(wchar_t) = %zu, wcslen(L\"caf\\u00e9\") = %zu units\n",
           sizeof(wchar_t), wcslen(w));
    CHECK(wcslen(w) == 4);
    CHECK(w[3] == 0xE9); /* one unit per code point when wchar_t is 32-bit */

    puts("\nall checks passed");
    return 0;
}
