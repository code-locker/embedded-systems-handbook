/*
 * 14_string_builder.c — a growable string in C: (buffer, length, capacity)
 *
 * Concept : Fixed char arrays force you to guess a size; repeated strcat is
 *           O(n^2) because it re-finds the end every time. A string builder
 *           stores the LENGTH (so append is O(len of piece), not O(total)),
 *           and the CAPACITY (so it can grow by doubling: amortised O(1) per
 *           byte). It always keeps a '\0' after the last byte so buf can be
 *           passed to any C API. This is exactly the shape of std::string,
 *           GLib's GString, the kernel's struct seq_buf, and Redis's sds.
 * Build   : make run P=dsa/05-arrays-and-strings/c/14_string_builder.c
 * Read    : ../notes/strings.md  (section "Dynamic strings")
 */
#include <stdarg.h>
#include <stdbool.h>
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

/*
 *   StrBuf sb;                 heap
 *   +-----------+             +---+---+---+---+----+---+---+---+
 *   | buf  -----+-----------> | h | e | l | l | o  |\0 | ? | ? |
 *   | len  = 5  |             +---+---+---+---+----+---+---+---+
 *   | cap  = 8  |               0   1   2   3   4    5   6   7
 *   +-----------+
 *   invariant: len < cap, buf[len] == '\0'  (cap counts the '\0' slot)
 *
 * Ownership: the StrBuf owns buf. sb_free() releases it; sb_detach() hands
 * it to the caller (who must free() it) and leaves the builder empty.
 */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    unsigned reallocs; /* instrumentation: how many times did we grow? */
} StrBuf;

static void sb_init(StrBuf *sb)
{
    sb->buf = NULL;
    sb->len = 0;
    sb->cap = 0;
    sb->reallocs = 0;
}

/* Make room for at least `extra` more chars (+1 for '\0').
 * Grow geometrically (x2): over N appends of 1 byte, total bytes copied by
 * all reallocs is 1+2+4+...+N < 2N  ->  amortised O(1) per byte.
 * Growing by a CONSTANT (+16) instead would copy 16+32+48+...  = O(N^2). */
static bool sb_reserve(StrBuf *sb, size_t extra)
{
    size_t need = sb->len + extra + 1;
    if (need <= sb->cap)
        return true; /* O(1) fast path: no allocation */
    size_t ncap = sb->cap ? sb->cap : 16;
    while (ncap < need)
        ncap *= 2;
    /* realloc may move the block: never keep old pointers into buf
     * across an append (same rule as iterators into a std::vector).    */
    char *nb = realloc(sb->buf, ncap);
    if (!nb)
        return false; /* old buffer still valid and owned by sb */
    sb->buf = nb;
    sb->cap = ncap;
    sb->reallocs++;
    return true;
}

/* Append n bytes. O(n) amortised. Bytes may include '\0' — the builder
 * tracks len itself, so it can hold binary data (strlen would not). */
static bool sb_append_n(StrBuf *sb, const char *s, size_t n)
{
    if (!sb_reserve(sb, n))
        return false;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0'; /* keep the invariant */
    return true;
}

static bool sb_append(StrBuf *sb, const char *s) { return sb_append_n(sb, s, strlen(s)); }

static bool sb_putc(StrBuf *sb, char c) { return sb_append_n(sb, &c, 1); }

/* printf-style append. Uses the C99 contract of vsnprintf: it returns the
 * number of chars it WOULD have written. Try once into the free space; if
 * it didn't fit, grow to exactly that size and format again.
 * A va_list can only be walked once -> va_copy for the second attempt. */
__attribute__((format(printf, 2, 3)))
static bool sb_appendf(StrBuf *sb, const char *fmt, ...)
{
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    size_t room = sb->cap ? sb->cap - sb->len : 0;
    int n = vsnprintf(room ? sb->buf + sb->len : NULL, room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return false;
    }
    if ((size_t)n >= room) { /* truncated (or no buffer yet) -> grow, retry */
        if (!sb_reserve(sb, (size_t)n)) {
            va_end(ap2);
            return false;
        }
        vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap2);
    }
    va_end(ap2);
    sb->len += (size_t)n;
    return true;
}

/* Insert at pos: O(len - pos + n) because the tail shifts right (memmove,
 * the ranges overlap). Same cost as std::string::insert.               */
static bool sb_insert(StrBuf *sb, size_t pos, const char *s)
{
    size_t n = strlen(s);
    if (pos > sb->len || !sb_reserve(sb, n))
        return false;
    memmove(sb->buf + pos + n, sb->buf + pos, sb->len - pos + 1);
    memcpy(sb->buf + pos, s, n);
    sb->len += n;
    return true;
}

/* Never NULL, even for an empty builder — callers can always printf it. */
static const char *sb_cstr(const StrBuf *sb) { return sb->buf ? sb->buf : ""; }

static void sb_clear(StrBuf *sb) /* O(1): keep capacity for reuse */
{
    sb->len = 0;
    if (sb->buf)
        sb->buf[0] = '\0';
}

static char *sb_detach(StrBuf *sb) /* caller now owns the returned buffer */
{
    char *out = sb->buf;
    sb_init(sb);
    return out;
}

static void sb_free(StrBuf *sb)
{
    free(sb->buf);
    sb_init(sb);
}

int main(void)
{
    puts("== basic appends ==");
    StrBuf sb;
    sb_init(&sb);
    CHECK(strcmp(sb_cstr(&sb), "") == 0);
    CHECK(sb_append(&sb, "uart"));
    CHECK(sb_putc(&sb, '@'));
    CHECK(sb_appendf(&sb, "%x: baud=%d parity=%s", 0x2800000, 115200, "none"));
    printf("  \"%s\"  len=%zu cap=%zu\n", sb_cstr(&sb), sb.len, sb.cap);
    CHECK(strcmp(sb_cstr(&sb), "uart@2800000: baud=115200 parity=none") == 0);
    CHECK(sb.len == strlen(sb.buf));
    CHECK(sb_insert(&sb, 0, "serial "));
    CHECK(strncmp(sb.buf, "serial uart@", 12) == 0);

    puts("== amortised growth: 1,000,000 single-byte appends ==");
    sb_clear(&sb);
    sb.reallocs = 0;
    for (int i = 0; i < 1000000; i++)
        CHECK(sb_putc(&sb, (char)('a' + i % 26)));
    printf("  len=%zu cap=%zu reallocs=%u (doubling: O(log n) grows, 64 -> 2^20)\n",
           sb.len, sb.cap, sb.reallocs);
    CHECK(sb.len == 1000000);
    CHECK(sb.reallocs <= 21);
    CHECK(sb.buf[sb.len] == '\0');
    CHECK(sb.buf[999999] == (char)('a' + 999999 % 26));

    puts("== appendf growing exactly when needed ==");
    StrBuf csv;
    sb_init(&csv);
    for (int i = 0; i < 1000; i++)
        CHECK(sb_appendf(&csv, "%s%d", i ? "," : "", i * i));
    printf("  1000 numbers -> %zu chars, last 20: \"%s\"\n", csv.len,
           csv.buf + csv.len - 20);
    CHECK(strncmp(csv.buf, "0,1,4,9,16", 10) == 0);
    CHECK(strcmp(csv.buf + csv.len - 6, "998001") == 0);

    puts("== binary-safe: embedded NULs are kept ==");
    StrBuf bin;
    sb_init(&bin);
    CHECK(sb_append_n(&bin, "ab\0cd", 5));
    printf("  len=%zu but strlen=%zu: length lives in the struct\n", bin.len,
           strlen(bin.buf));
    CHECK(bin.len == 5 && strlen(bin.buf) == 2);
    sb_free(&bin);

    puts("== detach: hand ownership to the caller ==");
    char *owned = sb_detach(&csv); /* csv is now empty; WE must free owned */
    CHECK(csv.buf == NULL && csv.len == 0);
    CHECK(owned[0] == '0');
    free(owned);

    sb_free(&sb);
    puts("\nall checks passed");
    return 0;
}
